# SPDX-License-Identifier: MIT
"""The JIT's compile error and the source positions it names.

Kept outside ``dftracer.utils.jit`` so ``jit_op`` can raise it without an
import cycle; the public name is ``dftracer.utils.jit.JitError``.
"""

from __future__ import annotations

import ast
import linecache
import os
import re
import sys
import textwrap
from typing import TYPE_CHECKING, Any, Callable, Optional

if TYPE_CHECKING:
    from types import FrameType

_UTILS_DIR = os.path.dirname(os.path.abspath(__file__))
# Frames in these places are the JIT's own: an error points at the caller beyond them.
_JIT_DIR = os.path.join(_UTILS_DIR, "jit")
_JIT_FILES = (
    os.path.join(_UTILS_DIR, "plugins.py"),
    os.path.join(_UTILS_DIR, "_jit_op.py"),
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

    def __reduce__(self) -> tuple[Any, ...]:
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
    kind: type[JitError],
    reason: str,
    file: str | None,
    line: int | None,
    col: int | None,
    source: str | None,
    precise: bool,
) -> JitError:
    err = kind(reason, locate=False)
    if file is not None and line is not None and col is not None and source is not None:
        err._set_location(file, line, col, source, precise)
    return err


def _is_jit_file(filename: str) -> bool:
    path = os.path.abspath(filename)
    return os.path.dirname(path) == _JIT_DIR or path in _JIT_FILES or path == __file__


def _call_site() -> tuple[str, int, int, str] | None:
    """The first frame outside the JIT: where the user wrote the construct
    that raised."""
    frame: FrameType | None = sys._getframe(2)
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


def _class_node(cls: type) -> tuple[str, list[str], ast.ClassDef]:
    """The file, its lines and the ClassDef of ``cls``. ``inspect`` finds the
    first class of that name in the file, so the match is by qualified name, and
    by a method's line when two classes share one."""
    import inspect

    file = inspect.getsourcefile(cls) or "<unknown>"
    lines = linecache.getlines(file) or open(file, encoding="utf-8").read().splitlines(True)
    tree = ast.parse("".join(lines))
    found: list[ast.ClassDef] = []

    def walk(body: list[ast.stmt], prefix: str) -> None:
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


class Source:
    """Where a function or class sits in its file, so a node of its AST maps back
    to ``file:line:col``. For a function the nodes come from the dedented
    ``text``; for a class they are already file positions."""

    def __init__(self, obj: type | Callable[..., object]) -> None:
        import inspect

        self.file = inspect.getsourcefile(obj) or "<unknown>"
        self.lines, self.first = inspect.getsourcelines(obj)
        text = "".join(self.lines)
        dedented = textwrap.dedent(text)
        self.margin = len(text.splitlines()[0]) - len(dedented.splitlines()[0])
        self.text = dedented
        self.cur: ast.AST | None = None
        self.cls_node: ast.ClassDef | None = None

    @classmethod
    def of_class(cls, klass: type) -> Source:
        file, lines, node = _class_node(klass)
        src = cls.__new__(cls)
        src.file, src.lines, src.first, src.margin = file, lines, 1, 0
        src.text = ""
        src.cur = None
        src.cls_node = node
        return src

    def error(
        self, reason: str, node: ast.AST | None, kind: type[JitError] | None = None
    ) -> JitError:
        lineno = getattr(node, "lineno", 1)
        col = getattr(node, "col_offset", 0)
        err = (kind or JitError)(reason, locate=False)
        source = self.lines[lineno - 1].rstrip("\n") if lineno <= len(self.lines) else ""
        err._set_location(self.file, self.first + lineno - 1, col + self.margin + 1, source, True)
        return err

    def relocate(self, exc: JitError, fallback: ast.AST | None) -> JitError:
        """``exc`` pointed at its node (or ``fallback``) in this source."""
        if exc.precise:
            return exc
        return self.error(exc.reason, exc.node if exc.node is not None else fallback, type(exc))

    def find(self, name: str | None) -> ast.AST | None:
        """The class statement, or the first assignment to ``name`` in its body."""
        cls_node: ast.AST | None = self.cls_node
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


def class_error(cls: type, reason: str, attr: str | None = None) -> JitError:
    """A JitError located at ``cls``'s statement, or at its ``attr`` assignment."""
    try:
        src = Source.of_class(cls)
        return src.error(reason, src.find(attr))
    except (OSError, TypeError, SyntaxError):
        return JitError(reason)


def decorator_error(fn: Callable[..., Any], decorator: str, reason: str) -> JitError:
    """A JitError located at the ``@...decorator(`` line above ``fn``. Before
    Python 3.11 a frame that applies a decorator reports the ``def`` line, so
    the error goes to that decorator there."""
    code = getattr(fn, "__code__", None)
    if sys.version_info >= (3, 11) or code is None:
        return JitError(reason)
    file, line = code.co_filename, code.co_firstlineno
    while True:
        source = linecache.getline(file, line).rstrip("\n")
        if not source.lstrip().startswith("@"):
            return JitError(reason)
        if f"{decorator}(" in source:
            break
        line += 1
    err = JitError(reason, locate=False)
    err._set_location(file, line, source.index("@") + 2, source)
    return err


class _Located:
    """The parts a runtime failure of a compiled plugin carries: the message
    and the ``file:line:col`` of the ``raise`` (or the call that failed)."""

    message: str
    file: Optional[str]
    line: Optional[int]
    col: Optional[int]

    def _locate(self, message: str, file: str | None, line: int | None, col: int | None) -> None:
        self.message = message
        self.file = file
        self.line = line
        self.col = col

    def __reduce__(self) -> tuple[object, ...]:
        return (_rebuild_runtime, (type(self), self.message, self.file, self.line, self.col))


def _runtime_text(message: str, file: str | None, line: int | None, col: int | None) -> str:
    if file is None or line is None or col is None:
        return message
    return f"{file}:{line}:{col}: {message}"


class Error(_Located, Exception):
    """A call a compiled plugin made failed with a status (a missing file, a
    parse error, a store read). ``message`` is the text of the failure, ``file``,
    ``line`` and ``col`` the place of the call in the plugin source. A body
    catches it with ``except jit.Error`` or one of its subclasses."""

    def __init__(
        self,
        message: str = "",
        *,
        file: str | None = None,
        line: int | None = None,
        col: int | None = None,
    ) -> None:
        Exception.__init__(self, _runtime_text(message, file, line, col))
        self._locate(message, file, line, col)


class NotFound(Error):
    """A path or key the plugin named does not exist (status ``DFTU_E_NOT_FOUND``)."""


class Closed(NotFound):
    """``await channel.recv()`` found the channel closed and drained (status
    ``DFTU_E_NOT_FOUND``). A body catches it with ``except jit.Closed``;
    ``async for v in channel`` ends instead of raising it."""


class IoError(Error):
    """A read or write failed (status ``DFTU_E_IO``)."""


class ParseError(Error):
    """A text did not parse; ``message`` starts with ``line:column:`` for a JSON or
    YAML text (status ``DFTU_E_PARSE``)."""


class Fatal(_Located, Exception):
    """``raise jit.Fatal("msg")`` in a compiled body fails the run. The scan stops
    and the caller of the run gets this exception with the message and the
    location of the ``raise``. A body cannot catch it."""

    def __init__(
        self,
        message: str = "",
        *,
        file: str | None = None,
        line: int | None = None,
        col: int | None = None,
    ) -> None:
        Exception.__init__(self, _runtime_text(message, file, line, col))
        self._locate(message, file, line, col)


class SkipRest(_Located, Exception):
    """``raise jit.SkipRest()`` in a compiled body stops the rest of the current
    batch (or the rest of a task) without an error: the work done so far is kept.
    It never reaches the caller of a run."""

    def __init__(
        self,
        message: str = "",
        *,
        file: str | None = None,
        line: int | None = None,
        col: int | None = None,
    ) -> None:
        Exception.__init__(self, _runtime_text(message, file, line, col))
        self._locate(message, file, line, col)


class Cancelled(_Located, BaseException):
    """``raise jit.Cancelled()`` in a compiled body ends the run with status
    ``DFTU_E_CANCELLED``. It derives from ``BaseException``, so ``except
    Exception`` in the caller does not swallow it; a body cannot catch it."""

    def __init__(
        self,
        message: str = "",
        *,
        file: str | None = None,
        line: int | None = None,
        col: int | None = None,
    ) -> None:
        BaseException.__init__(self, _runtime_text(message, file, line, col))
        self._locate(message, file, line, col)


def _rebuild_runtime(
    kind: type[Error] | type[Fatal] | type[Cancelled],
    message: str,
    file: str | None,
    line: int | None,
    col: int | None,
) -> Error | Fatal | Cancelled:
    return kind(message, file=file, line=line, col=col)


_RUNTIME_KINDS: dict[str, type[Error] | type[Fatal] | type[Cancelled]] = {
    "Fatal": Fatal,
    "Cancelled": Cancelled,
    "Error": Error,
    "NotFound": NotFound,
    "Closed": Closed,
    "IoError": IoError,
    "ParseError": ParseError,
}

# A compiled plugin fails a run with "jit.<Kind> [<file>:<line>:<col>]: <message>".
# The tag comes first so that a long message cannot cut it; the host prefixes the
# plugin and the callback.
_RUNTIME_TAG = re.compile(
    r"jit\.(Fatal|Cancelled|Error|NotFound|Closed|IoError|ParseError) \[(.+?):(\d+):(\d+)\]: (.*)\Z",
    re.S,
)


def translate_run_error(exc: BaseException) -> BaseException | None:
    """The ``jit`` exception a failed run's native error stands for, or None for
    an error that no compiled plugin tagged."""
    found = _RUNTIME_TAG.search(str(exc))
    if found is None:
        return None
    kind, file, line, col, message = found.groups()
    return _RUNTIME_KINDS[kind](message, file=file, line=int(line), col=int(col))


def check_port_wiring(plugins: list[Any]) -> None:
    """Raise a :class:`JitError`, located at the port, for a port that refers to
    another ``@jit.plugin``'s latest value or phase result when that plugin is not
    in ``plugins``: the host plans channels, but it has no way to know that a latest
    or a phase has no owner. A reference by name (a string) is the host's to check."""
    classes = [c for c in plugins if not isinstance(c, str)]
    owned = {
        decl.full
        for cls in classes
        for decl in (getattr(cls, "_jit_ports", None) or {}).values()
        if decl.owner
    }
    owner_of = {
        decl.full: decl
        for cls in classes
        for decl in (getattr(cls, "_jit_ports", None) or {}).values()
        if decl.owner
    }
    for cls in classes:
        for attr, decl in (getattr(cls, "_jit_ports", None) or {}).items():
            if isinstance(decl.source, str):
                # A reference by name takes no type from the owner, so a JIT owner in
                # this set is matched here; a hand-written owner's wire is 8 raw bytes.
                found = owner_of.get(decl.full)
                if found is not None and (found.kind, found.elem) != (decl.kind, decl.elem):
                    word = {"i64": "int", "f64": "float"}
                    raise class_error(
                        cls,
                        f"the port '{attr}' carries {word.get(decl.elem or '', decl.elem)} but "
                        f"{decl.full} is a {found.kind} port that carries "
                        f"{word.get(found.elem or '', found.elem)}",
                        attr,
                    )
            if not decl.owner and not isinstance(decl.source, str) and decl.full not in owned:
                raise class_error(
                    cls,
                    f"the port '{attr}' refers to {decl.full}, which no plugin of this set "
                    "declares; load the plugin that owns it in the same run",
                    attr,
                )


def translate_plan_error(plugins: list[Any], exc: BaseException) -> JitError | None:
    """The :class:`JitError` a failed plan stands for when the host's message names
    a port of one of the ``@jit.plugin`` classes in ``plugins`` (a receiver with no
    sender, options that differ between two ends, two plugins of one name), located
    at that port's declaration; None for any other failure."""
    text = str(exc)
    for cls in plugins:
        if isinstance(cls, str):
            continue
        for attr, decl in (getattr(cls, "_jit_ports", None) or {}).items():
            if decl.full and decl.full in text:
                return class_error(cls, text, attr)
    return None
