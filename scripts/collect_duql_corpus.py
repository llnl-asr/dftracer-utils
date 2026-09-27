#!/usr/bin/env python3
"""Collect query DSL filter strings from tests, docs and examples.

Prints one JSON string per line on stdout. With --sources, prints
``{"q": ..., "from": [...]}`` objects instead, listing every file that
contains the query.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ["tests", "python", "docs/source", "examples", "benchmarks"]
CPP_EXT = {".cpp", ".cc", ".c", ".h", ".hpp"}
PY_EXT = {".py", ".pyi"}
DOC_EXT = {".rst", ".md", ".txt", ".sh"}

QUERY_CALL = re.compile(
    r"(?:\bfrom_string|\bduql::parse|\bparse_or_throw|\btry_parse|\bdftu_duql_parse"
    r"|\.duql|\.query|\.filter|\bwith_query|\bexplain|\bsubsumes|\bduql_compile"
    r"|(?<![\w:.])(?:parse|tokenize|eval))"
    r"\(\s*$"
    r"|--duql[\"']?\s*,?\s*$"
    r"|--parg\s+duql=$"
    r"|\bduql\s*=\s*$"
)
QUERY_SHAPE = re.compile(
    r"^\s*(?:(?:not\b|\()\s*)*"
    r"(?:[A-Za-z_][\w.]*(?:\([\w.]*\))?|\"[^\"]*\"|'[^']*'|-?\d[\d.]*)"
    r"(?:\s*(?:==|!=|>=|<=|!~\*?|~\*?|[<>](?=[\s\d\"'-]))"
    r"|\s+(?:not\s+)?(?:in\s*\[|in\s+any\(|like\s*[\"']|ilike\s*[\"']))"
    r"|^\s*(?:(?:not\b|\()\s*)*(?:\"[^\"]*\"|'[^']*')\s+(?:not\s+)?in\s+[A-Za-z_]",
    re.IGNORECASE,
)
NON_QUERY_CONTEXT = re.compile(
    r"\b(?:TEST_CASE\w*|SUBCASE|SECTION|INFO|MESSAGE|CAPTURE|FAIL|WARN|TEST_SUITE|Contains)\(\s*$"
    r"|\bmatch\s*=\s*$"
    r"|(?:\+|<<)\s*$"
)
FRAGMENT_AFTER = re.compile(r"\s*(?:\+|<<|%|\.join\b|\.format\b)")

C_ESCAPES = {
    "n": "\n",
    "t": "\t",
    "r": "\r",
    "0": "\0",
    "a": "\a",
    "b": "\b",
    "f": "\f",
    "v": "\v",
    "\\": "\\",
    '"': '"',
    "'": "'",
    "?": "?",
}


def decode_escapes(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c != "\\" or i + 1 == len(s):
            out.append(c)
            i += 1
            continue
        n = s[i + 1]
        if n == "x":
            m = re.match(r"[0-9a-fA-F]+", s[i + 2 :])
            out.append(chr(int(m.group(0), 16)) if m else "x")
            i += 2 + (len(m.group(0)) if m else 0)
        elif n == "u" and re.match(r"[0-9a-fA-F]{4}", s[i + 2 : i + 6]):
            out.append(chr(int(s[i + 2 : i + 6], 16)))
            i += 6
        elif n in "01234567" and re.match(r"[0-7]{1,3}", s[i + 1 :]):
            m = re.match(r"[0-7]{1,3}", s[i + 1 :])
            out.append(chr(int(m.group(0), 8)))
            i += 1 + len(m.group(0))
        elif n in C_ESCAPES:
            out.append(C_ESCAPES[n])
            i += 2
        elif n == "\n":
            i += 2
        else:
            out.append("\\" + n)
            i += 2
    return "".join(out)


def cpp_literals(text):
    """Yield (value, start) for every C/C++ string literal outside comments."""
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            i = text.find("\n", i)
            i = n if i < 0 else i
        elif text.startswith("/*", i):
            i = text.find("*/", i + 2)
            i = n if i < 0 else i + 2
        elif c == "'" and not (i and text[i - 1].isalnum()):
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        elif c == '"':
            m = re.search(r"(?:u8|[uUL])?R$", text[max(0, i - 3) : i])
            if m and not (i - len(m.group(0)) and text[i - len(m.group(0)) - 1].isalnum()):
                open_paren = text.find("(", i)
                delim = text[i + 1 : open_paren]
                close = text.find(")" + delim + '"', open_paren)
                if close < 0:
                    return
                yield text[open_paren + 1 : close], i - len(m.group(0)), close + len(delim) + 2
                i = close + len(delim) + 2
                continue
            j = i + 1
            while j < n and text[j] not in '"\n':
                j += 2 if text[j] == "\\" else 1
            yield decode_escapes(text[i + 1 : j]), i, j + 1
            i = j + 1
        else:
            i += 1


PY_STR = re.compile(
    r"(?P<prefix>[rRbBuUfF]{0,2})(?P<q>'''|\"\"\"|'|\")",
)


def py_literals(text, comments=True):
    """Yield (value, start) for Python string literals; f-strings are skipped."""
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if comments and c == "#":
            i = text.find("\n", i)
            i = n if i < 0 else i
            continue
        if c not in "'\"":
            i += 1
            continue
        k = i
        while k > 0 and text[k - 1] in "rRbBuUfF" and i - k < 2:
            k -= 1
        if k > 0 and (text[k - 1].isalnum() or text[k - 1] == "_"):
            k = i
        prefix = text[k:i].lower()
        q = text[i : i + 3] if text[i : i + 3] in ("'''", '"""') else c
        j = i + len(q)
        while j < n and not text.startswith(q, j):
            if len(q) == 1 and text[j] == "\n":
                break
            j += 2 if text[j] == "\\" and "r" not in prefix else 1
        body = text[i + len(q) : j]
        if j < n and text.startswith(q, j) and "f" not in prefix and "b" not in prefix:
            yield (body if "r" in prefix else decode_escapes(body)), k, j + len(q)
        i = j + len(q) if j < n and text.startswith(q, j) else j + 1


CPP_RAW = re.compile(r'\bR"([^(\s"]*)\((.*?)\)\1"', re.S)


def literals(path, text):
    ext = path.suffix
    if ext in CPP_EXT:
        yield from cpp_literals(text)
    elif ext in PY_EXT:
        for value, start, end in py_literals(text):
            yield value, start, end
            if "\n" in value:
                for inner, _, _ in py_literals(value, comments=False):
                    yield inner, start, end
    else:
        for m in CPP_RAW.finditer(text):
            yield m.group(2), m.start(), m.end()
        blanked = CPP_RAW.sub(lambda m: " " * len(m.group(0)), text)
        yield from py_literals(blanked, comments=False)


def is_query(value, before, after):
    if not value.strip() or "\n" in value.strip() and not QUERY_SHAPE.match(value):
        return False
    if NON_QUERY_CONTEXT.search(before) or FRAGMENT_AFTER.match(after):
        return False
    return bool(QUERY_CALL.search(before) or QUERY_SHAPE.match(value))


def collect():
    found = {}
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or "_generated" in path.parts:
                continue
            if path.suffix not in CPP_EXT | PY_EXT | DOC_EXT:
                continue
            text = path.read_text(errors="replace")
            rel = path.relative_to(ROOT).as_posix()
            for value, start, end in literals(path, text):
                before = text[max(0, start - 120) : start]
                if is_query(value, before, text[end : end + 16]):
                    found.setdefault(value, []).append(rel)
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sources", action="store_true", help="also print source files")
    args = ap.parse_args()
    for q, files in collect().items():
        if args.sources:
            print(json.dumps({"q": q, "from": sorted(set(files))}))
        else:
            print(json.dumps(q))


def _selftest():
    lits = [
        v
        for v, *_ in cpp_literals(
            'f(R"(a == "x")"); g(R"DSL(b ~ "(c)")DSL"); h("c == \\"y\\"\\n"); // "no"'
        )
    ]
    assert lits == ['a == "x"', 'b ~ "(c)"', 'c == "y"\n'], lits
    lits = [v for v, *_ in py_literals("a('x == 1') # 'no'\nb(r\"y ~ '\\d'\") f'z{a}'")]
    assert lits == ["x == 1", "y ~ '\\d'"], lits
    assert QUERY_SHAPE.match('any(tags) not in ["a"]') and QUERY_SHAPE.match("'a' in name")
    assert not QUERY_SHAPE.match("plugin: ") and not QUERY_SHAPE.match("begin=1")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        _selftest()
        print("ok")
    else:
        main()
