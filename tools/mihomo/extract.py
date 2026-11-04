"""Extract source facts; reconcile them with explicit client mapping decisions."""

import hashlib
import json
from pathlib import Path
import re

from .go_source import Source

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "generated/mihomo"
POLICY = Path(__file__).with_name("model_mapping.json")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def json_text(value):
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def source_location(source, path, expression):
    text = (source.root / path).read_text(encoding="utf-8")
    source.files[path] = text
    matches = list(re.finditer(expression, text))
    if len(matches) != 1:
        raise ValueError(f"Expected one upstream declaration in {path}: {expression}")
    return {"file": path, "line": text.count("\n", 0, matches[0].start()) + 1}


def extract(root, revision):
    source = Source(root)
    policy = read_json(POLICY)
    dispatch = source.outbound()
    if dispatch.keys() != policy["protocols"].keys():
        raise ValueError("Outbound protocols changed; review tools/mihomo/model_mapping.json")
    plugins = source.plugin_dispatch()
    expected_plugins = {name.removeprefix("plugin-"): selector["struct"]
                        for name, selector in policy["selectors"].items() if name.startswith("plugin-")}
    if plugins != expected_plugins:
        raise ValueError(f"Plugin dispatch changed; review mappings: {plugins}")
    type_location = source_location(source, "adapter/parser.go", r'mapping\["type"\]')
    mux_location = source_location(source, "adapter/parser.go", r'mapping\["smux"\]')
    sections = []
    mux = source.walk("adapter/outbound", "SingMuxOption", "proxy", "proxies[].smux.")
    for protocol, record in dispatch.items():
        fields = source.walk("adapter/outbound", record, "proxy", "proxies[].")
        fields += [{"path": "proxies[].smux", "go_type": "SingMuxOption",
                    "required_by_decoder": False, "source": mux_location}] + mux
        fields += [{"path": "proxies[].type", "go_type": "string",
                    "required_by_decoder": True, "source": type_location}]
        sections.append({"id": "proxy-" + protocol, "go_struct": record, "fields": fields})

    for context, selector in policy["selectors"].items():
        fields = source.walk(selector["package"], selector["struct"], selector["tag"], selector["prefix"])
        if context.startswith("snell-obfs-") and context != "snell-obfs-simple":
            location = source_location(source, "adapter/outbound/snell.go",
                                       r'opt := &' + selector["struct"] + r'\{')
            fields.append({"path": "proxies[].obfs-opts.mode", "go_type": "string",
                           "required_by_decoder": False, "source": location})
        sections.append({"id": context, "go_struct": selector["struct"], "fields": fields})

    selected = {(row["context"], row["kernel_path"]): row for row in policy["rows"]}
    if len(selected) != len(policy["rows"]):
        raise ValueError("Duplicate client field mapping")
    rows = []
    seen = set()
    for section in sections:
        for field in section["fields"]:
            identity = section["id"], field["path"]
            decision = selected.get(identity)
            if decision is None:
                raise ValueError(f"Unreviewed kernel field {identity}: {field['source']}")
            if decision["kernel_type"] != field["go_type"]:
                raise ValueError(f"Kernel field type changed: {identity}: {field['go_type']}")
            if decision["required_by_decoder"] != field["required_by_decoder"]:
                raise ValueError(f"Kernel decoder presence policy changed: {identity}")
            if decision["disposition"] == "excluded" and not decision.get("reason"):
                raise ValueError(f"Excluded field requires a reason: {identity}")
            if decision["disposition"] not in ("mapped", "excluded"):
                raise ValueError(f"Invalid mapping decision: {identity}")
            rows.append(dict(decision, required_by_decoder=field["required_by_decoder"], source=field["source"]))
            seen.add(identity)
    if seen != selected.keys():
        raise ValueError(f"Mappings no longer exist upstream: {sorted(selected.keys() - seen)}")

    schema = source.walk("config", "RawConfig", "yaml")
    defaults = source.defaults()
    provenance = {"repository": "https://github.com/MetaCubeX/mihomo.git", "commit": revision,
                  "hash_format": "sha256-utf8-lf",
                  "files": {name: hashlib.sha256(text.encode()).hexdigest()
                            for name, text in sorted(source.files.items())},
                  "mapping_sha256": hashlib.sha256(POLICY.read_bytes()).hexdigest()}
    return {
        "config-fields.json": {"commit": revision, "dispatch": {"outbound": dispatch, "plugins": plugins}, "sections": sections},
        "raw-defaults.json": {"commit": revision, "expressions": defaults,
                              "note": "Unevaluated Go expressions; runtime function calls are not Python defaults."},
        "unified_node_fields.json": {"kernel_revision": revision, "protocols": policy["protocols"],
                                     "raw_defaults_consulted": {"Proxy": defaults["Proxy"]}, "rows": rows},
        "runtime_config_schema.json": {"kernel_revision": revision, "fields": schema},
        "source-provenance.json": provenance,
    }


def write_outputs(outputs, check):
    for path, text in outputs.items():
        current = path.read_text(encoding="utf-8") if path.is_file() else None
        if check:
            if current != text:
                label = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
                raise ValueError(f"Missing or stale generated file: {label}")
        elif current != text:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
