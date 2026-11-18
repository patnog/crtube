#!/usr/bin/env python3
"""Check that localized printf-style strings match the English argument format."""
import ast
import re
import sys
from pathlib import Path

LANGUAGES = ("EN", "PT_BR", "ES", "FR", "DE")
ENTRY_RE = re.compile(r'\[(STR_[A-Z0-9_]+)\]\s*=\s*("(?:\\.|[^"\\])*")\s*,')
FORMAT_RE = re.compile(
    r"%(?:[-+#0 ']*\d*(?:\.\d+)?(?:hh|h|ll|l|z|t|L)?)([diuoxXfFeEgGaAcspn])"
)


def catalog(source: str, language: str) -> dict[str, str]:
    marker = re.search(r'\[APP_LANGUAGE_' + re.escape(language) + r'\]\s*=\s*\{', source)
    if not marker:
        raise ValueError(f"catalog APP_LANGUAGE_{language} not found")
    next_catalog = re.search(r'\n\s*\[APP_LANGUAGE_[A-Z_]+\]\s*=\s*\{', source[marker.end():])
    end = marker.end() + next_catalog.start() if next_catalog else source.find('\n};', marker.end())
    if end < marker.end():
        raise ValueError(f"catalog APP_LANGUAGE_{language} is not terminated")
    body = source[marker.end():end]
    result: dict[str, str] = {}
    for match in ENTRY_RE.finditer(body):
        key, literal = match.groups()
        if key in result:
            raise ValueError(f"duplicate {key} in APP_LANGUAGE_{language}")
        result[key] = ast.literal_eval(literal)
    return result


def signature(value: str) -> tuple[str, ...]:
    # %% is an escaped percent and does not consume an argument.
    value = value.replace('%%', '')
    return tuple(FORMAT_RE.findall(value))


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} localization.c", file=sys.stderr)
        return 2
    source = Path(sys.argv[1]).read_text(encoding='utf-8')
    catalogs = {language: catalog(source, language) for language in LANGUAGES}
    english = catalogs['EN']
    failures = []
    for language in LANGUAGES[1:]:
        for key, translated in catalogs[language].items():
            if key not in english:
                failures.append(f"{language}: {key} has no English reference")
                continue
            expected = signature(english[key])
            actual = signature(translated)
            if actual != expected:
                failures.append(
                    f"{language}: {key}: expected printf arguments {expected}, found {actual}"
                )
    if failures:
        print('\n'.join(failures), file=sys.stderr)
        return 1
    print("localization format test: all translated printf placeholders match English")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
