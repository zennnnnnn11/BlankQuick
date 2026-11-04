"""Read the declarative Go configuration subset without executing upstream code.

This is deliberately not a general Go interpreter. Unsupported reachable field
syntax is an error, rather than a silently incomplete configuration schema.
"""

from dataclasses import dataclass
from pathlib import Path
import re


TOKEN = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|`[^`]*`|"(?:\\.|[^"\\])*"|\w+|[^\s]', re.M)


def tokens(text):
    return [(m.group(), m.start()) for m in TOKEN.finditer(text)
            if not m.group().startswith(("//", "/*"))]


def closing(items, start, opening="{", end="}"):
    depth = 0
    for index in range(start, len(items)):
        value = items[index][0]
        depth += value == opening
        depth -= value == end
        if depth == 0:
            return index
    raise ValueError("Unbalanced Go declaration")


@dataclass(frozen=True)
class Field:
    name: str
    kind: str
    tags: dict
    file: str
    line: int
    package: str
    imports: dict
    embedded: bool = False


class Source:
    def __init__(self, root):
        self.root = Path(root)
        self.records = {}
        self.files = {}
        self.loaded = set()
        self.aliases = {}

    def load(self, package):
        if package in self.loaded:
            return
        self.loaded.add(package)
        for path in sorted((self.root / package).glob("*.go")):
            if path.name.endswith("_test.go"):
                continue
            relative = path.relative_to(self.root).as_posix()
            content = path.read_text(encoding="utf-8")
            self.files[relative] = content
            for alias, target in re.findall(r'^type\s+(\w+)\s+(?:=\s*)?(\w+)\s*$', content, re.M):
                self.aliases[package, alias] = target
            imports = {}
            for alias, imported in re.findall(
                    r'^\s*(?:import\s+)?(?:(\w+)\s+)?"github.com/metacubex/mihomo/([^"]+)"', content, re.M):
                imports[alias or imported.rsplit("/", 1)[-1]] = imported
            items = tokens(content)
            for index in range(len(items) - 3):
                if items[index + 1][0] != "struct" or not re.fullmatch(r'\w+', items[index][0]):
                    continue
                if items[index + 2][0] != "{":
                    raise ValueError(f"Unsupported struct: {relative}")
                stop = closing(items, index + 2)
                name = items[index][0]
                body_start = items[index + 2][1] + 1
                body_stop = items[stop][1]
                body = content[body_start:body_stop]
                fields = []
                offset = body_start
                for line in body.splitlines(keepends=True):
                    line_tokens = tokens(line)
                    offset_line = content.count("\n", 0, offset) + 1
                    offset += len(line)
                    if not line_tokens:
                        continue
                    tag_values = [value[1:-1] for value, _ in line_tokens if value.startswith("`")]
                    tags = dict(re.findall(r'(\w+):"([^"]*)"', " ".join(tag_values)))
                    declaration = [value for value, _ in line_tokens if not value.startswith("`")]
                    embedded_tokens = declaration[1:] if declaration[0] == "*" else declaration
                    embedded = len(embedded_tokens) == 1 or (
                        len(embedded_tokens) == 3 and embedded_tokens[1] == ".")
                    if embedded:
                        field_name = declaration[-1]
                        kind = "".join(declaration)
                    else:
                        field_name = declaration[0]
                        kind = "".join(declaration[1:]).replace(",", ", ")
                    fields.append(Field(field_name, kind, tags, relative, offset_line,
                                        package, imports, embedded))
                key = (package, name)
                previous = self.records.get(key)
                signature = lambda values: [(f.name, f.kind, f.tags, f.embedded) for f in values]
                if key in self.records and (previous is None or signature(previous) != signature(fields)):
                    # Build-specific runtime types are not configuration records.
                    self.records[key] = None
                elif previous is None or not path.name.endswith("_stub.go"):
                    self.records[key] = fields

    def record(self, package, name):
        self.load(package)
        result = self.records.get((package, name))
        if result is None:
            raise ValueError(f"Missing or build-dependent Go configuration struct: {package}.{name}")
        return result

    def child(self, field):
        kind = field.kind.lstrip("*")
        suffix = ""
        if kind.startswith("[]"):
            kind = kind[2:]
            suffix = "[]"
        elif kind.startswith("map[string]"):
            kind = kind[len("map[string]"):]
            suffix = "{*}"
        if not re.fullmatch(r'\w+(?:\.\w+)?', kind):
            return None
        package = field.package
        if "." in kind:
            alias, kind = kind.split(".")
            package = field.imports.get(alias)
            if package is None:
                return None
        self.load(package)
        while (package, kind) in self.aliases:
            kind = self.aliases[package, kind]
        if (package, kind) not in self.records:
            return None
        return package, kind, suffix

    def walk(self, package, name, tag, prefix="", stack=()):
        identity = (package, name)
        if identity in stack:
            raise ValueError(f"Recursive configuration struct: {identity}")
        result = []
        for field in self.record(package, name):
            child = self.child(field)
            tagged = field.tags.get(tag)
            if field.embedded and (tagged is None or tagged.endswith(",squash")):
                if child is None:
                    raise ValueError(f"Unresolved embedded configuration field: {field}")
                result.extend(self.walk(*child[:2], tag, prefix, stack + (identity,)))
                continue
            if tagged is None:
                if field.name[0].isupper():
                    raise ValueError(f"Reachable field has no explicit {tag} tag: {field}")
                continue
            if tagged.split(",")[0] == "-":
                continue
            key, *options = tagged.split(",")
            key = key or field.name
            suffix = child[2] if child else ""
            path = prefix + key
            result.append({"path": path, "go_type": field.kind,
                           "required_by_decoder": "omitempty" not in options,
                           "source": {"file": field.file, "line": field.line}})
            if child:
                result.extend(self.walk(*child[:2], tag, prefix + key + suffix + ".", stack + (identity,)))
        if len({row["path"] for row in result}) != len(result):
            raise ValueError(f"Duplicate decoded fields in {package}.{name}")
        return result

    def outbound(self):
        path = "adapter/parser.go"
        content = (self.root / path).read_text(encoding="utf-8")
        self.files[path] = content
        items = tokens(content)
        start = next(index + 2 for index in range(len(items) - 2)
                     if items[index][0] == "switch" and items[index + 1][0] == "proxyType")
        stop = closing(items, start)
        body = content[items[start][1] + 1:items[stop][1]]
        result = {}
        for match in re.finditer(r'case\s+([^:]+):([\s\S]*?)(?=\n\s*(?:case |default:)|\Z)', body):
            option = re.search(r'&outbound\.(\w+)\{', match[2])
            if option is None:
                raise ValueError(f"Unsupported outbound dispatch: {match[1]}")
            for label in match[1].split(","):
                if not re.fullmatch(r'\s*"[^"\n]+"\s*', label):
                    raise ValueError(f"Unsupported outbound label: {label}")
                protocol = label.strip()[1:-1]
                if protocol in result:
                    raise ValueError(f"Duplicate outbound protocol: {protocol}")
                result[protocol] = option[1]
        if not result:
            raise ValueError("No outbound dispatch found")
        return result

    def plugin_dispatch(self):
        path = "adapter/outbound/shadowsocks.go"
        content = (self.root / path).read_text(encoding="utf-8")
        self.files[path] = content
        imports = dict((alias or package.rsplit("/", 1)[-1], package) for alias, package in re.findall(
            r'^\s*(?:import\s+)?(?:(\w+)\s+)?"github.com/metacubex/mihomo/([^"]+)"', content, re.M))
        result = {}
        branches = list(re.finditer(r'if option\.Plugin == ("[^"]+"|\w+\.\w+)\s*\{', content))
        for index, branch in enumerate(branches):
            label = branch[1]
            if label.startswith('"'):
                label = label.strip('"')
            else:
                alias, constant = label.split(".")
                self.load(imports[alias])
                values = [value for name, text in self.files.items()
                          if name.startswith(imports[alias] + "/")
                          for value in re.findall(r'^\s*(?:const\s+)?' + constant + r'(?:\s+string)?\s*=\s*"([^"]+)"', text, re.M)]
                if len(set(values)) != 1:
                    raise ValueError(f"Unresolved plugin discriminator: {branch[1]}")
                label = values[0]
            body = content[branch.end():branches[index + 1].start() if index + 1 < len(branches) else len(content)]
            record = re.search(r':=\s*&?(\w+)\{', body)
            if record is None or label in result:
                raise ValueError(f"Unsupported plugin branch: {label}")
            result[label] = record[1]
        return result

    def defaults(self):
        path = "config/config.go"
        content = (self.root / path).read_text(encoding="utf-8")
        self.files[path] = content
        start = content.index("func DefaultRawConfig()")
        items = tokens(content[start:])
        begin = next(i for i in range(len(items) - 1)
                     if items[i][0] == "RawConfig" and items[i - 1][0] == "&"
                     and items[i + 1][0] == "{") + 1
        end = closing(items, begin)
        # Preserve expressions, including function calls and composite values;
        # evaluating them in Python would invent runtime defaults.
        entries = {}
        index = begin + 1
        while index < end:
            name = items[index][0]
            if items[index + 1][0] != ":":
                raise ValueError(f"Unsupported default initializer: {name}")
            index += 2
            values = []
            depth = 0
            while index < end:
                value = items[index][0]
                if value == "," and depth == 0:
                    break
                depth += value in ("{", "[", "(")
                depth -= value in ("}", "]", ")")
                values.append(value)
                index += 1
            entries[name] = "".join(values)
            index += 1
        return entries
