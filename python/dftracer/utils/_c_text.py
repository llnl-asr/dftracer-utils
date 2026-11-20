# SPDX-License-Identifier: MIT
"""C source text helpers shared by the JIT emitters."""

from __future__ import annotations


def c_str_literal(s: str) -> str:
    """A C string literal for ``s``: printable ASCII as is, a backslash and a
    quote escaped, every other byte (control, NUL, non-ASCII UTF-8) as a
    three-digit octal escape, which never merges with the next character."""
    out: list[str] = []
    for byte in s.encode("utf-8"):
        ch = chr(byte)
        if ch == "\\" or ch == '"':
            out.append("\\" + ch)
        elif 0x20 <= byte < 0x7F and ch != "?":
            out.append(ch)
        else:
            out.append(f"\\{byte:03o}")
    return '"' + "".join(out) + '"'
