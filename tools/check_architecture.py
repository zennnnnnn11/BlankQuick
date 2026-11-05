"""Check local include/link cycles and the intended module boundaries."""

from pathlib import Path
import argparse
import json
import hashlib
from collections import Counter
import re

ROOT = Path(__file__).resolve().parent.parent


def load_links(path):
    """Read CMake's evaluated graph, including aliases and imported dependencies."""
    content = path.read_text(encoding="utf-8")
    labels = {key: label.split(r"\n", 1)[0] for key, label in
              re.findall(r'"(node\d+)" \[ label = "([^"]+)"', content)}
    if not labels:
        raise SystemExit(f"Missing evaluated targets in {path}")
    graph = {name: set() for name in labels.values()}
    for source, target in re.findall(r'"(node\d+)" -> "(node\d+)"', content):
        graph[labels[source]].add(labels[target])
    return graph


def build_state(build):
    reply = build / ".cmake/api/v1/reply"
    indices = list(reply.glob("index-*.json"))
    if not indices:
        raise SystemExit("Missing CMake File API reply; configure with tools/project.ps1.")
    index = json.loads(max(indices).read_text(encoding="utf-8"))
    model_path = reply / index["reply"]["codemodel-v2"]["jsonFile"]
    model = json.loads(model_path.read_text(encoding="utf-8"))
    if Path(model["paths"]["source"]).resolve() != ROOT.resolve():
        raise SystemExit("Architecture build belongs to a different source tree.")
    cmake_files = json.loads((reply / index["reply"]["cmakeFiles-v1"]["jsonFile"]).read_text(encoding="utf-8"))

    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    inputs = {}
    for item in cmake_files["inputs"]:
        if not item.get("isGenerated"):
            path = Path(item["path"])
            if not path.is_absolute():
                path = ROOT / path
            inputs[path.resolve().as_posix()] = digest(path)
    return {"graph": digest(build / "architecture-targets.dot"),
            "cache": digest(build / "CMakeCache.txt"), "model": digest(model_path),
            "policy": digest(ROOT / "tools/architecture.json"),
            "inputs": inputs}, model


def reachable(graph, start):
    seen = set()
    pending = [start]
    while pending:
        item = pending.pop()
        if item not in seen:
            seen.add(item)
            pending.extend(graph.get(item, ()))
    return seen


def reject_cycles(graph, label):
    finished = set()
    active = []

    def visit(item):
        if item in active:
            cycle = active[active.index(item):] + [item]
            raise SystemExit(f"{label} cycle: {' -> '.join(cycle)}")
        if item in finished:
            return
        active.append(item)
        for target in sorted(graph.get(item, ())):
            visit(target)
        active.pop()
        finished.add(item)

    for item in sorted(graph):
        visit(item)


def feature_owner(path):
    parts = Path(path).parts
    return parts[2] if len(parts) > 2 and parts[0] == "src" and parts[1] in ("domain", "application") else None


def layer_owner(path):
    parts = Path(path).parts
    if len(parts) < 2 or parts[0] != "src":
        return None
    return {"domain": "domain", "application": "application", "platform": "platform",
            "services": "service", "app": "ui", "shared": "shared"}.get(parts[1])


def check_data_source(name, content):
    # Qt Core is used for value types (UTF-8, URLs, JSON). It also contains I/O
    # classes, so the link fence is supplemented by a source include fence.
    forbidden = r"(?:[Ww]indows\.h|winsock2\.h|ws2tcpip\.h|bcrypt\.h|wininet\.h|iphlpapi\.h|" \
                r"QtNetwork/[^>\"]+|QtQml/[^>\"]+|QtConcurrent[^>\"]*|" \
                r"(?:QtCore/)?Q(?:File\w*|Dir\w*|LockFile|SaveFile|Process(?:Environment)?|" \
                r"RandomGenerator|Uuid|Timer|Thread|CoreApplication|StandardPaths|Temporary(?:Dir|File))|" \
                r"Q(?:Network\w*|HostAddress|Tcp\w*|Udp\w*|WebSocket)|thread|fstream)"
    if re.search(r'#include\s*[<"]' + forbidden + r'[>"]', content):
        raise ValueError(f"Data code includes an effect API: {name}")
    if re.search(r'\b(?:QUuid::createUuid\w*|QRandomGenerator::(?:system|global))\s*\(', content):
        raise ValueError(f"Data code obtains system entropy: {name}")
    if re.search(r'\b(?:std::filesystem|fs)::(?:exists|status|is_regular_file|is_directory|'
                 r'create_directories|remove|remove_all|directory_iterator|recursive_directory_iterator)\s*[({]', content):
        raise ValueError(f"Data code accesses the filesystem: {name}")


def check_sources():
    policy = json.loads((ROOT / "tools/architecture.json").read_text(encoding="utf-8"))
    features = {name: set(dependencies) for name, dependencies in policy["features"].items()}
    reject_cycles(features, "Declared feature")
    files = {path.relative_to(ROOT).as_posix(): path for directory in ("src", "tests/cpp", "generated/mihomo")
             for path in (ROOT / directory).rglob("*")
             if path.suffix in (".cpp", ".hpp", ".h", ".inc")}
    public = {}
    for name in files:
        if "/include/" in name:
            header = name.split("/include/", 1)[1]
            if header in public:
                raise ValueError(f"Duplicate public header: {header}")
            public[header] = name
    includes = {name: set() for name in files}
    feature_graph = {name: set() for name in features}
    for name, path in files.items():
        owner = feature_owner(name)
        if owner is not None and owner not in features:
            raise ValueError(f"Undeclared feature: {owner}")
        content = path.read_text(encoding="utf-8")
        layer = layer_owner(name)
        if name.startswith("src/") and layer is None:
            raise ValueError(f"Unregistered source layer: {name}")
        if layer in ("domain", "shared"):
            check_data_source(name, content)
        if name.startswith("src/platform/system_proxy/") and re.search(r'\b(?:RuntimeSession|KernelController)\b', content):
            raise ValueError(f"System proxy depends on runtime orchestration: {name}")
        if not name.startswith("src/app/") and re.search(r'#include\s*[<"](?:QtGui|QtQuick|QQuick|QGui|QWindow)', content):
            raise ValueError(f"Non-UI code includes GUI headers: {name}")
        for header in re.findall(r'^\s*#include\s+[<"]([^>"\n]+)[>"]', content, re.M):
            target = public.get(header)
            local = (path.parent / header).resolve()
            if target is None and local.is_relative_to(ROOT):
                candidate = local.relative_to(ROOT).as_posix()
                if candidate in files:
                    target = candidate
            if target is None:
                if header.startswith("blank/") or header.endswith(".inc"):
                    raise ValueError(f"Unresolved local include: {name}: {header}")
                continue
            includes[name].add(target)
            other_layer = layer_owner(target)
            if layer and other_layer and other_layer not in policy["layers"][layer]:
                raise ValueError(f"Forbidden layer include: {layer} -> {other_layer}: {name}: {target}")
            other = feature_owner(target)
            if "/include/" in name and "/include/" not in target:
                raise ValueError(f"Public header exposes a private implementation: {name}: {target}")
            if other and other != owner:
                if not header.startswith("blank/") or "/include/" not in target:
                    raise ValueError(f"Cross-feature private include: {name}: {target}")
                if owner:
                    if other not in features[owner]:
                        raise ValueError(f"Undeclared feature dependency: {owner} -> {other}: {name}")
                    feature_graph[owner].add(other)
            if name.startswith("src/shared/") and not target.startswith("src/shared/"):
                raise ValueError(f"Shared code depends on higher-level code: {name}: {target}")
    reject_cycles(includes, "Include")
    reject_cycles(feature_graph, "Actual feature")
    return files, includes, policy


def check_build(build, files, policy):
    state, model = build_state(build)
    snapshot = build / "architecture-configuration.json"
    if not snapshot.exists() or state != json.loads(snapshot.read_text(encoding="utf-8")):
        raise ValueError("Evaluated graph is stale; reconfigure with tools/project.ps1.")
    database = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    compiled = Counter(Path(entry["file"]).resolve() for entry in database)
    testing_disabled = re.search(r'^BUILD_TESTING:BOOL=OFF$',
                                 (build / "CMakeCache.txt").read_text(encoding="utf-8"), re.M) is not None
    for name, path in files.items():
        if testing_disabled and name.startswith(("tests/", "generated/")):
            continue
        if path.suffix == ".cpp" and compiled[path.resolve()] != 1:
            raise ValueError(f"Source must compile exactly once: {name}: {compiled[path.resolve()]}")
    links = load_links(build / "architecture-targets.dot")
    reject_cycles(links, "Evaluated link")
    owners = policy["targets"]
    for name, dependencies in links.items():
        owner = owners.get(name)
        for dependency in dependencies:
            other = owners.get(dependency)
            if owner and other and other != owner and other not in policy["features"][owner]:
                raise ValueError(f"Undeclared feature link: {name} -> {dependency}")
            layer = policy["target_layers"].get(name)
            other_layer = policy["target_layers"].get(dependency)
            if layer and other_layer and other_layer not in policy["layers"][layer]:
                raise ValueError(f"Forbidden layer link: {name} -> {dependency}")
        if owner and reachable(links, name) & {"Qt6::Gui", "Qt6::Quick", "blank_quick"}:
            raise ValueError(f"Feature links UI code: {name}")
    for name, layer in policy["target_layers"].items():
        if layer in ("domain", "shared"):
            forbidden = {target for target in reachable(links, name)
                         if policy["target_layers"].get(target) in ("platform", "application", "service", "ui")}
            forbidden |= reachable(links, name) & {"Qt6::Network", "Qt6::Qml", "Qt6::Concurrent", "Qt6::WebSockets"}
            if forbidden:
                raise ValueError(f"Data target reaches effect code: {name}: {sorted(forbidden)}")
    if reachable(links, "profile_storage") & {"subscription_fetcher", "Qt6::Network"}:
        raise ValueError("Profile storage links HTTP transport")
    required = set()
    reply = build / ".cmake/api/v1/reply"
    for config in model["configurations"]:
        for target in config["targets"]:
            details = json.loads((reply / target["jsonFile"]).read_text(encoding="utf-8"))
            if details["type"] != "UTILITY":
                required.add(target["name"])
                for source in details.get("sources", []):
                    path = Path(source["path"])
                    if not path.is_absolute():
                        path = ROOT / path
                    if path.resolve().is_relative_to(ROOT / "src"):
                        source_layer = layer_owner(path.resolve().relative_to(ROOT).as_posix())
                        if source_layer != policy["target_layers"].get(target["name"]):
                            raise ValueError(f"Source compiled in another layer: {path}: {target['name']}")
                        actual = feature_owner(path.resolve().relative_to(ROOT).as_posix())
                        if actual and owners.get(target["name"]) != actual:
                            raise ValueError(f"Feature source belongs to another build target: {path}: {target['name']}")
    if required - links.keys():
        raise ValueError(f"Evaluated graph omits build targets: {sorted(required - links.keys())}")
    return len(links)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--record-build", type=Path)
    parser.add_argument("--includes-only", action="store_true")
    options = parser.parse_args()
    if options.record_build:
        state, _ = build_state(options.record_build)
        (options.record_build / "architecture-configuration.json").write_text(
            json.dumps(state, sort_keys=True), encoding="utf-8")
        return
    if not options.build and not options.includes_only:
        parser.error("Use --build for evaluated link/source checks, or --includes-only.")
    files, _, policy = check_sources()
    if options.build:
        count = check_build(options.build, files, policy)
        print(f"Architecture passed: {len(files)} files, {len(policy['features'])} features, {count} evaluated targets.")
    else:
        print(f"Source architecture passed: {len(files)} files, {len(policy['features'])} features; links not checked.")


if __name__ == "__main__":
    main()
