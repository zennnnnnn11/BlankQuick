"""Reject malformed, unpaired or unbounded C++ clang-tidy suppressions."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]

# Skip quoted/raw literals before examining comments. A JavaScript string or a
# test fixture containing the word NOLINT is not a C++ suppression directive.
TOKENS = re.compile(
    r'(?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delimiter)")'
    r'''|(?P<quoted>(?:u8|u|U|L)?(?:"(?:\\[\s\S]|[^"\\])*"|'(?:\\[\s\S]|[^'\\\n])*'))'''
    r'|(?P<line>//[^\n]*)|(?P<block>/\*[\s\S]*?\*/)'
)
COMMAND = re.compile(r'\bNOLINT(?:NEXTLINE|BEGIN|END)?\b')
CHECK_NAME = re.compile(r'[a-zA-Z][a-zA-Z0-9_-]*')


def check_comments(source: str, name: str = 'source') -> int:
    """Each directive must name checks; BEGIN/END must be properly nested."""
    blocks = []
    count = 0
    for token in TOKENS.finditer(source):
        if token.lastgroup not in {'line', 'block'}:
            continue
        for command in COMMAND.finditer(token.group()):
            line = source.count('\n', 0, token.start() + command.start()) + 1
            location = f'{name}:{line}'
            arguments = re.match(r'\(([^()\r\n]*)\)', token.group()[command.end():])
            if arguments is None:
                raise ValueError(f'{location}: suppression requires a closed check list')
            checks = tuple(part.strip() for part in arguments[1].split(','))
            if any(not CHECK_NAME.fullmatch(check) for check in checks):
                raise ValueError(f'{location}: suppression must name individual checks')
            count += 1
            if command.group() == 'NOLINTBEGIN':
                blocks.append((checks, location))
            elif command.group() == 'NOLINTEND':
                if not blocks or blocks[-1][0] != checks:
                    raise ValueError(f'{location}: suppression END does not match BEGIN')
                blocks.pop()
    if blocks:
        raise ValueError(f'{blocks[-1][1]}: suppression BEGIN has no END')
    return count


def check_tree() -> tuple[int, int]:
    files = 0
    directives = 0
    for directory in ('src', 'tests/cpp', 'generated/mihomo'):
        for path in sorted((ROOT / directory).rglob('*')):
            if path.suffix not in {'.cpp', '.hpp', '.h', '.inc'}:
                continue
            files += 1
            directives += check_comments(path.read_text(encoding='utf-8'),
                                         path.relative_to(ROOT).as_posix())
    return files, directives


if __name__ == '__main__':
    try:
        files, directives = check_tree()
    except ValueError as error:
        raise SystemExit(str(error)) from error
    print(f'Lint comments passed: {files} files, {directives} bounded directives.')
