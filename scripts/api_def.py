# SPDX-License-Identifier: MIT
"""Reader for src/dftracer/utils/python/core/api.def, the one Python API table.

`rows()` returns the PY_METHOD rows in file order, each with the same fields as
the rows the native module gives back from `_api_rows()`.
"""

import ast
from pathlib import Path
from typing import List, NamedTuple, Tuple

API_DEF = Path(__file__).resolve().parents[1] / "src/dftracer/utils/python/core/api.def"


class Row(NamedTuple):
    cls: str
    name: str
    op: str
    args: str
    result: str
    name_rule: str
    on_expr: bool
    keywords: Tuple[str, ...]
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


def rows(path: Path = API_DEF) -> List[Row]:
    text = path.read_text(encoding="utf-8")
    out = []
    marker = "\nPY_METHOD("
    pos = text.find(marker)
    while pos >= 0:
        start = pos + len(marker)
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
            )
        )
        pos = text.find(marker, end)
    return out
