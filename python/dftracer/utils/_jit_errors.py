# SPDX-License-Identifier: MIT
"""The JIT's compile error and the source positions it names.

Kept outside ``dftracer.utils.jit`` so ``jit_op`` can raise it without an
import cycle; the public name is ``dftracer.utils.jit.JitError``.
"""

from __future__ import annotations

import ast
import inspect
import linecache
import os
import sys
import textwrap
from typing import TYPE_CHECKING, Any, List, Tuple

if TYPE_CHECKING:
    from types import FrameType

_UTILS_DIR = os.path.dirname(os.path.abspath(__file__))
# Frames in these places are the JIT's own: an error points at the caller beyond them.
_JIT_DIR = os.path.join(_UTILS_DIR, "jit")
_JIT_FILES = (
    os.path.join(_UTILS_DIR, "plugins.py"),
    os.path.join(_UTILS_DIR, "jit_op.py"),
)


class JitError(Exception):
    """A compile error from the JIT: a construct outside the supported subset,
    a bad annotation or a bad config.

    ``str(error)`` has one stable form, ``<file>:<line>:<col>: <reason>``
    followed by the offending source line. ``file``, ``line`` (1-based),
    ``col`` (1-based), ``source`` (the source line) and ``reason`` hold the parts;
    an error that has no source position (an unreadable source) leaves the first
    four as ``None`` and its text is the bare reason."""

    def __init__(self, reason: str, *, locate: bool = True) -> None:
        super().__init__(reason)
        self.reason = reason
        self.file: str | None = None
        self.line: int | None = None
        self.col: int | None = None
        self.source: str | None = None
        # The AST node the compiler should point at, and whether a node already
        # fixed the position (as opposed to the call site that built the error).
        self.node: ast.AST | None = None
        self.precise = False
        if locate:
            site = _call_site()
            if site is not None:
                self._set_location(*site)

    def __reduce__(self) -> "Tuple[Any, ...]":
        # Rebuilt from the parts, so a copy or a pickle does not look the
        # location up again and add a second one.
        state = (self.reason, self.file, self.line, self.col, self.source, self.precise)
        return (_rebuild_error, (type(self), *state))

    def _set_location(
        self, file: str, line: int, col: int, source: str, precise: bool = False
    ) -> None:
        self.file, self.line, self.col, self.source = file, line, col, source
        self.precise = precise
        self.args = (f"{file}:{line}:{col}: {self.reason}\n{source}",)


def _rebuild_error(
    kind: "type[JitError]",
    reason: str,
    file: "str | None",
    line: "int | None",
    col: "int | None",
    source: "str | None",
    precise: bool,
) -> JitError:
    err = kind(reason, locate=False)
    if file is not None and line is not None and col is not None and source is not None:
        err._set_location(file, line, col, source, precise)
    return err


def _is_jit_file(filename: str) -> bool:
    path = os.path.abspath(filename)
    return os.path.dirname(path) == _JIT_DIR or path in _JIT_FILES or path == __file__


def _call_site() -> "Tuple[str, int, int, str] | None":
    """The first frame outside the JIT: where the user wrote the construct
    that raised."""
    frame: "FrameType | None" = sys._getframe(2)
    while frame is not None and _is_jit_file(frame.f_code.co_filename):
        frame = frame.f_back
    if frame is None:
        return None
    file = frame.f_code.co_filename
    line = frame.f_lineno
    source = linecache.getline(file, line).rstrip("\n")
    col = len(source) - len(source.lstrip()) + 1
    positions = getattr(frame.f_code, "co_positions", None)
    if positions is not None:
        for idx, pos in enumerate(positions()):
            if idx == frame.f_lasti // 2:
                if pos[0] == line and pos[2] is not None:
                    col = pos[2] + 1
                break
    return file, line, col, source


def _class_node(cls: type) -> "Tuple[str, List[str], ast.ClassDef]":
    """The file, its lines and the ClassDef of ``cls``. ``inspect`` finds the
    first class of that name in the file, so the match is by qualified name, and
    by a method's line when two classes share one."""
    file = inspect.getsourcefile(cls) or "<unknown>"
    lines = linecache.getlines(file) or open(file, encoding="utf-8").read().splitlines(True)
    tree = ast.parse("".join(lines))
    found: List[ast.ClassDef] = []

    def walk(body: "List[ast.stmt]", prefix: str) -> None:
        for stmt in body:
            if isinstance(stmt, ast.ClassDef):
                qual = prefix + stmt.name
                if qual == cls.__qualname__:
                    found.append(stmt)
                walk(stmt.body, qual + ".")
            elif isinstance(stmt, (ast.FunctionDef, ast.AsyncFunctionDef)):
                walk(stmt.body, prefix + stmt.name + ".<locals>.")
            else:
                for field in ("body", "orelse", "finalbody"):
                    inner = getattr(stmt, field, None)
                    if isinstance(inner, list):
                        walk([s for s in inner if isinstance(s, ast.stmt)], prefix)
                for handler in getattr(stmt, "handlers", []):
                    walk(handler.body, prefix)

    walk(tree.body, "")
    if not found:
        raise OSError(f"class {cls.__qualname__} not found in {file}")
    first = getattr(cls, "__firstlineno__", None)
    method_lines = [v.__code__.co_firstlineno for v in vars(cls).values() if hasattr(v, "__code__")]
    for node in found:
        end = getattr(node, "end_lineno", node.lineno)
        if first is not None and node.lineno <= first <= end:
            return file, lines, node
    if len(found) > 1:
        for node in found:
            end = getattr(node, "end_lineno", node.lineno)
            if any(node.lineno <= ln <= end for ln in method_lines):
                return file, lines, node
    return file, lines, found[0]


class _Source:
    """Where a function or class sits in its file, so a node of its AST maps back
    to ``file:line:col``. For a function the nodes come from the dedented
    ``text``; for a class they are already file positions."""

    def __init__(self, obj: Any) -> None:
        self.file = inspect.getsourcefile(obj) or "<unknown>"
        self.lines, self.first = inspect.getsourcelines(obj)
        text = "".join(self.lines)
        dedented = textwrap.dedent(text)
        self.margin = len(text.splitlines()[0]) - len(dedented.splitlines()[0])
        self.text = dedented
        self.cur: ast.AST | None = None
        self.cls_node: ast.ClassDef | None = None

    @classmethod
    def of_class(cls, klass: type) -> "_Source":
        file, lines, node = _class_node(klass)
        src = cls.__new__(cls)
        src.file, src.lines, src.first, src.margin = file, lines, 1, 0
        src.text = ""
        src.cur = None
        src.cls_node = node
        return src

    def error(
        self, reason: str, node: "ast.AST | None", kind: "type[JitError] | None" = None
    ) -> JitError:
        lineno = getattr(node, "lineno", 1)
        col = getattr(node, "col_offset", 0)
        err = (kind or JitError)(reason, locate=False)
        source = self.lines[lineno - 1].rstrip("\n") if lineno <= len(self.lines) else ""
        err._set_location(self.file, self.first + lineno - 1, col + self.margin + 1, source, True)
        return err

    def relocate(self, exc: JitError, fallback: "ast.AST | None") -> JitError:
        """``exc`` pointed at its node (or ``fallback``) in this source."""
        if exc.precise:
            return exc
        return self.error(exc.reason, exc.node if exc.node is not None else fallback, type(exc))

    def find(self, name: "str | None") -> "ast.AST | None":
        """The class statement, or the first assignment to ``name`` in its body."""
        cls_node = self.cls_node
        if cls_node is None:
            cls_node = ast.parse(self.text).body[0]
        if name is None or not isinstance(cls_node, ast.ClassDef):
            return cls_node
        for stmt in cls_node.body:
            if isinstance(stmt, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == name for t in stmt.targets
            ):
                return stmt
            if (
                isinstance(stmt, ast.AnnAssign)
                and isinstance(stmt.target, ast.Name)
                and stmt.target.id == name
            ):
                return stmt
        return cls_node


def _class_error(cls: type, reason: str, attr: "str | None" = None) -> JitError:
    """A JitError located at ``cls``'s statement, or at its ``attr`` assignment."""
    try:
        src = _Source.of_class(cls)
        return src.error(reason, src.find(attr))
    except (OSError, TypeError, SyntaxError):
        return JitError(reason)
