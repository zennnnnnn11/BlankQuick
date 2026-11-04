"""Check audit coverage and emit unevaluated C++ expressions for all mapped fields."""
import argparse
import collections
import json
import re
import subprocess
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--audit', type=Path, default=root.parent / 'mihomo-config-audit')
    parser.add_argument('--kernel', type=Path, default=root.parent / 'mihomo')
    parser.add_argument('--emit-cpp', type=Path, default=root / 'out/unified_node_coverage.cpp')
    args = parser.parse_args()
    manifest = json.loads((root / 'docs/unified_node_fields.json').read_text(encoding='utf-8'))
    audit = json.loads((args.audit / 'config-fields.json').read_text(encoding='utf-8'))
    defaults = json.loads((args.audit / 'raw-defaults.json').read_text(encoding='utf-8'))
    if defaults.get('Proxy') != manifest['raw_defaults_consulted']['Proxy']:
        raise SystemExit('Global proxy initialization has changed; review node default policy.')
    revision = subprocess.check_output(['git', '-C', str(args.kernel), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != manifest['kernel_revision'] or revision != audit['commit']:
        raise SystemExit('Kernel/audit/model revisions differ; review the field mappings first.')
    expected = {(s['id'], r['path']) for s in audit['sections'] for r in s['fields']
                if s['id'].removeprefix('proxy-') in audit['dispatch']['outbound']
                or s['id'].startswith(('plugin-', 'snell-obfs-'))}
    actual = [(r['context'], r['kernel_path']) for r in manifest['rows']]
    if len(actual) != len(set(actual)) or set(actual) != expected:
        raise SystemExit(f'Coverage differs: missing={expected-set(actual)}, extra={set(actual)-expected}')

    header = (root / 'src/unified_node.hpp').read_text(encoding='utf-8')
    if revision not in header:
        raise SystemExit('The header revision does not match the reviewed manifest.')
    mapped = [r for r in manifest['rows'] if r['disposition'] == 'mapped']
    excluded = [r for r in manifest['rows'] if r['disposition'] == 'excluded']
    if any(not r.get('reason') for r in excluded):
        raise SystemExit('Every excluded kernel field must have a reason.')
    for row in manifest['rows']:
        source = row['source']
        lines = (args.kernel / source['file']).read_text(encoding='utf-8').splitlines()
        if not 0 < source['line'] <= len(lines):
            raise SystemExit(f'Invalid source line: {row}')
        declaration = lines[source['line'] - 1]
        key = row['kernel_path'].rsplit('.', 1)[-1].removesuffix('[]')
        tagged = re.search(r'(?:proxy|obfs):"' + re.escape(key) + r'(?:,|")', declaration)
        if not tagged and f'["{key}"]' not in declaration:
            raise SystemExit(f'The live kernel declaration no longer matches {row}')

    code = ['#include "unified_node.hpp"', '#include <utility>', '#include <variant>',
            'using namespace node_model;',
            'template <typename Value>',
            'constexpr decltype(auto) unwrap(const Value& value) noexcept {',
            '  if constexpr (requires { *value; }) { return *value; }',
            '  else { return (value); }', '}', '']
    for kernel_name, model_name in manifest['protocols'].items():
        code.append(f'static_assert({model_name}_param::protocol_tag == protocol_type::{model_name});')
        code.append(f'static_assert(kernel_protocol_name({model_name}_param::protocol_tag) == "{kernel_name}");')
    for member in ('heartbeat_interval', 'request_timeout', 'keep_alive_interval'):
        code.append(f'static_assert(std::same_as<decltype(quic_control{{}}.{member}), std::chrono::milliseconds>);')
    code.append('static_assert(std::same_as<decltype(link_control{}.handshake_timeout), std::chrono::seconds>);')
    expressions = set()
    for row in mapped:
        path = row['model_path'].removesuffix('.active_alternative')
        expression = 'std::declval<const unified_node&>()'
        for part in path.split('.'):
            match = re.fullmatch(r'(\w+)(?:<(\w+)>)?(\[\])?', part)
            if not match:
                raise SystemExit(f'Unrecognized model path: {path}')
            member, alternative, sequence = match.groups()
            expression = f'unwrap({expression}).{member}'
            if alternative:
                expression = f'std::get<{alternative}>({expression})'
            if sequence:
                expression = f'unwrap({expression}).front()'
        expressions.add(expression)
    for expression in sorted(expressions):
        code.append(f'static_assert(requires {{ {expression}; }});')
    args.emit_cpp.parent.mkdir(parents=True, exist_ok=True)
    args.emit_cpp.write_text('\n'.join(code) + '\n', encoding='utf-8')
    counts = collections.Counter(r['layer'] for r in mapped)
    print(f'{len(expected)} audited paths accounted for: {len(mapped)} mapped, {len(excluded)} excluded.')
    print(f'Mapped by layer: {dict(sorted(counts.items()))}; {len(expressions)} distinct C++ access expressions.')
    print(f'Compile {args.emit_cpp} with C++20 and -I{root / "src"} to validate every model path.')


if __name__ == '__main__':
    main()
