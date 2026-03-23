"""Query DSL for filtering DFTracer trace events.

Build query expressions using Field objects with Python operators:

    from dftracer.utils.query import Field

    cat = Field("cat")
    dur = Field("dur")
    name = Field("name")

    # Simple comparison
    q = cat == "POSIX"

    # Combined with AND/OR
    q = (cat == "POSIX") & (dur > 1000)
    q = (cat == "POSIX") | (cat == "STDIO")

    # NOT
    q = ~(cat == "POSIX")

    # IN / NOT IN
    q = cat.is_in(["POSIX", "STDIO"])
    q = cat.not_in(["MPI"])

    # Nested field paths
    level = Field("args.level")
    q = level == "DEBUG"

    # Convert to query string for C++ parser
    query_string = str(q)
"""

from __future__ import annotations

from typing import Sequence, Union

Value = Union[str, int, float, bool]


class Expr:
    """Base class for query expressions."""

    def __and__(self, other: Expr) -> Expr:
        return _BinaryExpr("and", self, other)

    def __or__(self, other: Expr) -> Expr:
        return _BinaryExpr("or", self, other)

    def __invert__(self) -> Expr:
        return _NotExpr(self)

    def __str__(self) -> str:
        raise NotImplementedError


class _CompareExpr(Expr):
    def __init__(self, field: str, op: str, value: Value) -> None:
        self._field = field
        self._op = op
        self._value = value

    def __str__(self) -> str:
        return f"{self._field} {self._op} {_format_value(self._value)}"


class _InExpr(Expr):
    def __init__(self, field: str, values: Sequence[Value]) -> None:
        self._field = field
        self._values = list(values)

    def __str__(self) -> str:
        items = ", ".join(_format_value(v) for v in self._values)
        return f"{self._field} in [{items}]"


class _NotInExpr(Expr):
    def __init__(self, field: str, values: Sequence[Value]) -> None:
        self._field = field
        self._values = list(values)

    def __str__(self) -> str:
        items = ", ".join(_format_value(v) for v in self._values)
        return f"{self._field} not in [{items}]"


class _BinaryExpr(Expr):
    def __init__(self, op: str, left: Expr, right: Expr) -> None:
        self._op = op
        self._left = left
        self._right = right

    def __str__(self) -> str:
        return f"({self._left} {self._op} {self._right})"


class _NotExpr(Expr):
    def __init__(self, operand: Expr) -> None:
        self._operand = operand

    def __str__(self) -> str:
        return f"not ({self._operand})"


class Field:
    """A field reference for building query expressions.

    Supports arbitrary field names including dotted paths for nested
    JSON fields (e.g., "args.level", "args.io.size").
    """

    def __init__(self, name: str) -> None:
        self._name = name

    def __eq__(self, other: Value) -> Expr:  # type: ignore[override]
        return _CompareExpr(self._name, "==", other)

    def __ne__(self, other: Value) -> Expr:  # type: ignore[override]
        return _CompareExpr(self._name, "!=", other)

    def __gt__(self, other: Value) -> Expr:
        return _CompareExpr(self._name, ">", other)

    def __lt__(self, other: Value) -> Expr:
        return _CompareExpr(self._name, "<", other)

    def __ge__(self, other: Value) -> Expr:
        return _CompareExpr(self._name, ">=", other)

    def __le__(self, other: Value) -> Expr:
        return _CompareExpr(self._name, "<=", other)

    def is_in(self, values: Sequence[Value]) -> Expr:
        return _InExpr(self._name, values)

    def not_in(self, values: Sequence[Value]) -> Expr:
        return _NotInExpr(self._name, values)


def _format_value(v: Value) -> str:
    if isinstance(v, str):
        escaped = v.replace('"', '\\"')
        return f'"{escaped}"'
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)
