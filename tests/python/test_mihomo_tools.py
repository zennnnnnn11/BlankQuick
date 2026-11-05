import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from mihomo.go_source import Source
from mihomo.extract import read_json, write_outputs
import sync_mihomo
import check_architecture


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_embedded_pointer_alias_nested_sequences_and_source_lines(self):
        self.write("config/types.go", '''package config
type Base struct {
    Enabled bool `yaml:"enabled"`
    Internal string `yaml:"-"`
}
type Child struct {
    Value *string `yaml:"value,omitempty"`
}
type ChildAlias Child
type RawConfig struct {
    *Base
    Children []ChildAlias `yaml:"children"`
    Values map[string]Child `yaml:"values"`
    Literal string `yaml:"literal"` // } does not close the struct
}
''')
        rows = Source(self.root).walk("config", "RawConfig", "yaml")
        self.assertEqual([row["path"] for row in rows], [
            "enabled", "children", "children[].value", "values", "values{*}.value", "literal"])
        self.assertEqual(rows[2]["go_type"], "*string")
        self.assertFalse(rows[2]["required_by_decoder"])
        self.assertEqual(rows[2]["source"], {"file": "config/types.go", "line": 7})

    def test_import_alias_and_identical_build_stubs(self):
        self.write("config/types.go", '''package config
import X "github.com/metacubex/mihomo/options"
type RawConfig struct {
    Child X.Options `yaml:"child"`
}
''')
        declaration = '''package options
type Options struct {
    Enabled bool `yaml:"enable"`
}
'''
        self.write("options/full.go", declaration)
        self.write("options/full_stub.go", "//go:build disabled\n" + declaration)
        rows = Source(self.root).walk("config", "RawConfig", "yaml")
        self.assertEqual([row["path"] for row in rows], ["child", "child.enable"])
        self.assertEqual(rows[-1]["source"]["file"], "options/full.go")

    def test_proxy_array_container_keeps_the_decoder_key(self):
        self.write("config/types.go", '''package config
type Peer struct {
    Server string `proxy:"server"`
}
type Options struct {
    Peers []Peer `proxy:"peers,omitempty"`
}
''')
        rows = Source(self.root).walk("config", "Options", "proxy", "proxies[].")
        self.assertEqual([row["path"] for row in rows], ["proxies[].peers", "proxies[].peers[].server"])

    def test_ambiguous_build_struct_is_rejected(self):
        self.write("config/a.go", 'package config\ntype Options struct {\n Value int `yaml:"value"`\n}\n')
        self.write("config/b.go", 'package config\ntype Options struct {\n Value string `yaml:"value"`\n}\n')
        with self.assertRaisesRegex(ValueError, "build-dependent"):
            Source(self.root).walk("config", "Options", "yaml")

    def test_unreviewed_tagless_exported_field_is_rejected(self):
        self.write("config/a.go", 'package config\ntype Options struct {\n Value string\n}\n')
        with self.assertRaisesRegex(ValueError, "no explicit yaml tag"):
            Source(self.root).walk("config", "Options", "yaml")

    def test_recursive_configuration_is_rejected(self):
        self.write("config/a.go", 'package config\ntype Options struct {\n Next *Options `yaml:"next"`\n}\n')
        with self.assertRaisesRegex(ValueError, "Recursive"):
            Source(self.root).walk("config", "Options", "yaml")

    def test_new_outbound_alias_is_not_silently_skipped(self):
        self.write("adapter/parser.go", '''package adapter
func Parse() {
 switch proxyType {
 case "one", "two":
   option := &outbound.TestOption{}
 default:
   panic("unsupported")
 }
}
''')
        self.assertEqual(Source(self.root).outbound(), {"one": "TestOption", "two": "TestOption"})

    def test_defaults_preserve_runtime_expressions_and_empty_collections(self):
        self.write("config/config.go", '''package config
func DefaultRawConfig() *RawConfig {
 return &RawConfig{
  Proxy: []map[string]any{},
  Runtime: compute("a,{b}"),
  Nested: Child{Enabled: true},
 }
}
''')
        self.assertEqual(Source(self.root).defaults(), {
            "Proxy": "[]map[string]any{}", "Runtime": 'compute("a,{b}")', "Nested": "Child{Enabled:true}"})

    def test_stale_output_check_does_not_rewrite_it(self):
        path = self.root / "generated.txt"
        path.write_text("previous", encoding="utf-8")
        with self.assertRaises(ValueError):
            write_outputs({path: "updated"}, True)
        self.assertEqual(path.read_text(encoding="utf-8"), "previous")

    def test_noop_generation_preserves_build_input_timestamp(self):
        path = self.root / "generated.inc"
        write_outputs({path: "unchanged"}, False)
        timestamp = path.stat().st_mtime_ns
        write_outputs({path: "unchanged"}, False)
        self.assertEqual(path.stat().st_mtime_ns, timestamp)


class PipelineTests(unittest.TestCase):
    def facts(self):
        return {name: read_json(ROOT / "generated/mihomo" / name) for name in (
            "unified_node_fields.json", "runtime_config_schema.json", "source-provenance.json",
            "config-fields.json", "raw-defaults.json")}

    def test_render_failure_publishes_nothing(self):
        facts = self.facts()
        with patch.object(sys, "argv", ["sync_mihomo.py", "sync"]), \
                patch.object(sync_mihomo, "checkout", return_value=Path("unused")), \
                patch.object(sync_mihomo, "extract", return_value=facts), \
                patch.object(sync_mihomo, "generate", side_effect=ValueError("invalid mapping")), \
                patch.object(sync_mihomo, "write_outputs") as write:
            with self.assertRaisesRegex(ValueError, "invalid mapping"):
                sync_mihomo.main()
            write.assert_not_called()

    def test_stable_release_resolver_rejects_prereleases(self):
        release = {"tag_name": "v9.0.0", "draft": False, "prerelease": True}
        with patch.object(sync_mihomo, "urlopen", return_value=io.StringIO(json.dumps(release))):
            with self.assertRaisesRegex(ValueError, "stable"):
                sync_mihomo.resolve_release()

    def test_model_policy_accounts_for_every_extracted_field(self):
        facts = self.facts()
        extracted = {(section["id"], field["path"]) for section in facts["config-fields.json"]["sections"]
                     for field in section["fields"]}
        mapped = [(row["context"], row["kernel_path"]) for row in facts["unified_node_fields.json"]["rows"]]
        self.assertEqual(len(mapped), len(set(mapped)))
        self.assertEqual(extracted, set(mapped))
        self.assertTrue(all(row.get("reason") for row in facts["unified_node_fields.json"]["rows"]
                            if row["disposition"] == "excluded"))

    def test_architecture_rejects_cycles(self):
        with self.assertRaisesRegex(SystemExit, "cycle"):
            check_architecture.reject_cycles({"profiles": {"runtime"}, "runtime": {"profiles"}}, "Feature")

    def test_storage_does_not_link_http_transport(self):
        build = os.environ.get("BLANK_ARCHITECTURE_BUILD")
        if not build:
            self.skipTest("Run under CTest for the current evaluated build graph")
        graph = check_architecture.load_links(Path(build) / "architecture-targets.dot")
        self.assertFalse(check_architecture.reachable(graph, "profile_storage") & {"subscription_fetcher", "Qt6::Network"})


if __name__ == "__main__":
    unittest.main()
