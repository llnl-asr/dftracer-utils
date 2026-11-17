# SPDX-License-Identifier: MIT
"""Reader for src/dftracer/utils/python/core/api.def, the one Python API table.

`rows()` returns the PY_METHOD rows in file order, each with the same fields as
the rows the native module gives back from `_api_rows()`.
"""

import ast
import re
from pathlib import Path
from typing import List, NamedTuple, Tuple

API_DEF = Path(__file__).resolve().parents[1] / "src/dftracer/utils/python/core/api.def"


# The argument and result tokens the table and the allowlist may name. The ones
# api.def uses today are implemented; the rest are the row shapes a hand-written
# method still waits for, and tests/python/api_allowlist.txt names them in its
# needs-token lines.
ARG_TOKENS = frozenset(
    {
        "NONE", "SCALAR", "NUMBER2",
        "SERIES", "SERIES_OR_SCALAR", "SCALAR_ARITH", "I64_ARG", "F64_ARG", "STR_ARG",
        "REDUCE", "FLAG", "RANK", "LOGICAL", "CMP", "PRIM", "DTYPE",
    }
)  # fmt: skip
RESULT_TOKENS = frozenset(
    {"SAME", "BOOL", "INT", "FLOAT", "STR", "I64", "F64", "SCALAR", "ELEMENT", "BOOL_VALUE"}
)


class Row(NamedTuple):
    cls: str
    name: str
    op: str
    args: str
    result: str
    name_rule: str
    on_expr: bool
    keywords: Tuple[str, ...]
    err: str
    doc: str


def _split(text: str) -> List[str]:
    parts, depth, start, quote = [], 0, 0, False
    i = 0
    while i < len(text):
        c = text[i]
        if quote:
            if c == "\\":
                i += 1
            elif c == '"':
                quote = False
        elif c == '"':
            quote = True
        elif c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
        i += 1
    parts.append(text[start:].strip())
    return parts


def _literal(text: str) -> str:
    """The value of adjacent C string literals."""
    out, i = [], 0
    while i < len(text):
        if text[i] == '"':
            j = i + 1
            while text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(ast.literal_eval(text[i : j + 1]))
            i = j + 1
        else:
            i += 1
    return "".join(out)


def _strip_comments(text: str) -> str:
    """The text without `/* */` and `//` comments (newlines kept), leaving
    string literals alone."""
    out, i, quote = [], 0, False
    while i < len(text):
        c, two = text[i], text[i : i + 2]
        if quote:
            out.append(c)
            if c == "\\":
                out.append(text[i + 1])
                i += 1
            elif c == '"':
                quote = False
        elif c == '"':
            quote = True
            out.append(c)
        elif two == "/*":
            end = text.find("*/", i + 2)
            end = len(text) if end < 0 else end + 2
            out.append("".join("\n" for ch in text[i:end] if ch == "\n"))
            i = end - 1
        elif two == "//":
            end = text.find("\n", i)
            i = (len(text) if end < 0 else end) - 1
        else:
            out.append(c)
        i += 1
    return "".join(out)


def _strip_directives(text: str) -> str:
    """The text without preprocessor lines (with their backslash
    continuations) and without the inactive part of an `#if 0` block."""
    out, skip_depth, continued = [], 0, False
    for line in text.split("\n"):
        stripped = line.strip()
        directive = continued or stripped.startswith("#")
        continued = directive and stripped.endswith("\\")
        if stripped.startswith("#"):
            word = stripped[1:].split()[0] if stripped[1:].split() else ""
            if skip_depth:
                if word in ("if", "ifdef", "ifndef"):
                    skip_depth += 1
                elif word == "endif":
                    skip_depth -= 1
                elif word in ("else", "elif") and skip_depth == 1:
                    skip_depth = 0
            elif word == "if" and stripped[1:].split()[1:2] == ["0"]:
                skip_depth = 1
        if directive or skip_depth:
            out.append("")
        else:
            out.append(line)
    return "\n".join(out)


def rows(path: Path = API_DEF) -> List[Row]:
    text = _strip_directives(_strip_comments(path.read_text(encoding="utf-8")))
    out = []
    marker = re.compile(r"^[ \t]*PY_METHOD\(", re.M)
    found = marker.search(text)
    while found:
        start = found.end()
        end, depth, quote, i = None, 1, False, start
        while end is None:
            c = text[i]
            if quote:
                if c == "\\":
                    i += 1
                elif c == '"':
                    quote = False
            elif c == '"':
                quote = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    end = i
            i += 1
        f = _split(text[start:end])
        if len(f) != 11:
            raise ValueError(f"{path}: PY_METHOD row of {len(f)} fields: {text[start:end][:60]!r}")
        if f[3] not in ARG_TOKENS or f[4] not in RESULT_TOKENS:
            raise ValueError(f"{path}: unknown token in row {f[1]}: {f[3]}, {f[4]}")
        keywords = tuple(k for k in (_literal(f[7]), _literal(f[8])) if k)
        out.append(
            Row(
                f[0],
                f[1],
                _literal(f[2]),
                f[3],
                f[4],
                f[5],
                f[6] == "YES",
                keywords,
                _literal(f[9]),
                _literal(f[10]),
            )
        )
        found = marker.search(text, end)
    return out
