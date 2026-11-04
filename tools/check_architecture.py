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


def check_links(links, foundation, local_targets):
    reject_cycles(links, "Evaluated link")
    for name in foundation:
        forbidden = reachable(links, name) & (local_targets - foundation - {"rapidyaml"})
        if forbidden:
            raise SystemExit(f"Foundation links upper/local orchestration: {name}: {sorted(forbidden)}")
    for name in links:
        if name.startswith("backend_") or name == "runtime_backend":
            if reachable(links, name) & {"Qt6::Gui", "Qt6::Quick", "Qt6::QuickControls2", "blank_quick"}:
                raise SystemExit(f"Backend links the frontend: {name}")
    if reachable(links, "profile_storage") & {"subscription_fetcher", "Qt6::Network"}:
        raise SystemExit("Storage links the download/network backend")


def check_graph_coverage(links, required_targets):
    missing = required_targets - links.keys()
    if missing:
        raise SystemExit(f"Evaluated graph omits build targets: {sorted(missing)}")


def implementation_graph(includes):
    # These pairs are private implementations of one existing library contract.
    cohesive = ({"src/node_input", "src/clash_material"},
                {"src/override_engine", "src/override_dom", "src/override_detail"},
                {"src/profile_storage", "src/profile_codec"},
                {"src/runtime_config", "src/runtime_config_detail"})

    def unit(name):
        result = name.rsplit(".", 1)[0]
        result = re.sub(r"(/foundation/[^/]+)/include/blank/[^/]+/", r"\1/", result)
        for group in cohesive:
            if result in group:
                return min(group)
        return result

    graph = {unit(name): set() for name in includes if not name.startswith("backend/tests/")}
    for name, dependencies in includes.items():
        if unit(name) not in graph:
            continue
        graph[unit(name)].update(unit(target) for target in dependencies
                                 if unit(target) in graph and unit(name) != unit(target))
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


def main():
    parser = argparse.ArgumentParser()
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
        parser.error("Use --build for the full check, or --includes-only for a scoped source check.")
    files = {path.relative_to(ROOT).as_posix(): path for directory in ("src", "backend")
             for path in (ROOT / directory).rglob("*")
             if path.suffix in (".cpp", ".hpp", ".h", ".inc")}
    includes = {name: set() for name in files}
    public_headers = {}
    for name in files:
        if "/include/" in name:
            public_headers[name.split("/include/", 1)[1]] = name
    for name, path in files.items():
        content = path.read_text(encoding="utf-8")
        if name.startswith("backend/session/system_proxy_controller.") and re.search(
                r"\b(?:RuntimeSession|KernelController)\b", content):
            raise SystemExit(f"Proxy transaction depends on session/kernel orchestration: {name}")
        for header in re.findall(r'^\s*#include\s+[<"]([^>"\n]+)[>"]', content, re.M):
            if header in public_headers:
                includes[name].add(public_headers[header])
                continue
            candidates = [path.parent / header, ROOT / "src" / header,
                          ROOT / "backend/session" / header]
            for target in candidates:
                if not target.is_relative_to(ROOT):
                    continue
                key = target.resolve().relative_to(ROOT).as_posix()
                if key in files:
                    includes[name].add(key)
                    break
        if name.startswith("backend/") and re.search(r'#include\s*[<"](?:QtGui|QtQuick|QQml|QQuick|QGui|QWindow)', content):
            raise SystemExit(f"Backend contains frontend dependency: {name}")
    for name in files:
        if name.startswith("backend/foundation/"):
            forbidden = reachable(includes, name) & {key for key in files
                         if key.startswith(("backend/session/", "backend/service/", "src/runtime_"))}
            if forbidden:
                raise SystemExit(f"Foundation depends on application orchestration: {name}: {forbidden}")
    if options.build:
        database = json.loads((options.build / "compile_commands.json").read_text(encoding="utf-8"))
        compiled = Counter(Path(entry["file"]).resolve() for entry in database)
        for name, path in files.items():
            if path.suffix == ".cpp" and compiled[path.resolve()] != 1:
                raise SystemExit(f"Source must compile exactly once: {name}: {compiled[path.resolve()]}")
    reject_cycles(includes, "Include")
    reject_cycles(implementation_graph(includes), "Implementation unit")
    storage = ("profile_storage.cpp", "profile_storage.hpp", "profile_codec.cpp", "profile_codec.hpp")
    for name in storage:
        forbidden = reachable(includes, "src/" + name) & {
            "src/subscription_fetcher.hpp", "src/subscription_metadata.hpp"}
        if forbidden:
            raise SystemExit(f"Storage depends on download implementation: {name}: {forbidden}")
    for name in files:
        if name.startswith("src/runtime_") or name == "src/override_dom.hpp":
            if "src/override_detail.hpp" in reachable(includes, name):
                raise SystemExit(f"DOM integration depends on override internals: {name}")
    for name in ("src/json_document.hpp", "src/subscription_data.hpp", "src/subscription_account.hpp"):
        forbidden = reachable(includes, name) & {
            "src/runtime_config.hpp", "src/override_detail.hpp", "src/profile_storage.hpp",
            "src/subscription_fetcher.hpp", "src/subscription_metadata.hpp"}
        if forbidden:
            raise SystemExit(f"Shared data depends on upper layers: {name}: {forbidden}")

    for name in ("src/subscription_account.hpp", "src/subscription_metadata.hpp", "src/subscription_metadata.cpp"):
        forbidden = reachable(includes, name) & {"src/node_input.hpp", "src/unified_node.hpp", "src/subscription_data.hpp"}
        if forbidden:
            raise SystemExit(f"Account metadata depends on node/transport results: {name}: {forbidden}")

    if options.build:
        state, model = build_state(options.build)
        snapshot = options.build / "architecture-configuration.json"
        if not snapshot.exists() or state != json.loads(snapshot.read_text(encoding="utf-8")):
            raise SystemExit("Evaluated graph is stale; reconfigure with tools/project.ps1.")
        links = load_links(options.build / "architecture-targets.dot")
        foundation = {"backend_" + path.name for path in (ROOT / "backend/foundation").iterdir()
                      if path.is_dir()}
        targets = {target["name"] for config in model["configurations"] for target in config["targets"]}
        required = set()
        reply = options.build / ".cmake/api/v1/reply"
        for config in model["configurations"]:
            for target in config["targets"]:
                details = json.loads((reply / target["jsonFile"]).read_text(encoding="utf-8"))
                if details["type"] != "UTILITY":
                    required.add(target["name"])
        check_graph_coverage(links, required)
        check_links(links, foundation, targets)
        print(f"Architecture passed: {len(files)} local files, {len(links)} evaluated link nodes; "
              "include/implementation/link cycles and declared boundaries checked.")
    else:
        print(f"Source architecture passed: {len(files)} local files; link dependencies NOT checked.")


if __name__ == "__main__":
    main()
