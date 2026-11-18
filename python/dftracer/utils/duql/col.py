# SPDX-License-Identifier: MIT
"""Column expressions of the duql builder."""

from __future__ import annotations

import math
from typing import (
    TYPE_CHECKING,
    Callable,
    Dict,
    Literal,
    Optional,
    Protocol,
    Sequence,
    Tuple,
    Union,
    runtime_checkable,
)

from ..enums import DUQL_DURATION_UNITS
from ..expr import duql_string

if TYPE_CHECKING:
    pass

ParamValue = Union[bool, int, float, str, Sequence[Union[bool, int, float, str]]]


@runtime_checkable
class Query(Protocol):
    """A sub-query: a :class:`~dftracer.utils.duql.Pipe`."""

    def inline(self) -> str: ...


def value_text(v: object) -> str:
    if isinstance(v, Col):
        return v._text
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        if not math.isfinite(v):
            raise ValueError("a duql number must be finite")
        return repr(v)
    if isinstance(v, str):
        return duql_string(v)
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(value_text(x) for x in v) + "]"
    raise TypeError(f"no duql literal for {type(v).__name__}")


def _is_path(text: str) -> bool:
    if not text or text[0].isdigit():
        return False
    depth = 0
    tick = False
    for ch in text:
        if ch == "`":
            tick = not tick
        elif tick:
            continue
        elif ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        elif depth == 0 and not (ch.isascii() and (ch.isalnum() or ch in "_.^")):
            return False
    return True


def _wrap(text: str) -> "Col":
    return Col("(" + text + ")")


class Col:
    """A duql expression. Operators and methods return new expressions:
    ``==``, ``!=``, ``<``, ``<=``, ``>``, ``>=``, ``+``, ``-``, ``*``, ``/``,
    ``//``, ``%``, unary ``-``, and ``&``, ``|``, ``~`` for ``and``, ``or``,
    ``not``."""

    __slots__ = ("_text",)

    def __init__(self, text: str) -> None:
        self._text = text

    def raw(self) -> str:
        """The expression's duql text, not canonical."""
        return self._text

    def __repr__(self) -> str:
        return f"Col({self._text})"

    def __bool__(self) -> bool:
        raise TypeError("a duql expression has no truth value: use &, | and ~, not and, or, not")

    def _bin(self, op: str, other: object) -> "Col":
        return _wrap(f"{self._text} {op} {value_text(other)}")

    def _rbin(self, op: str, other: object) -> "Col":
        return _wrap(f"{value_text(other)} {op} {self._text}")

    def __eq__(self, other: object) -> "Col":  # type: ignore[override]  # ty: ignore[incompatible-method-override]
        return self._bin("==", other)

    def __ne__(self, other: object) -> "Col":  # type: ignore[override]  # ty: ignore[incompatible-method-override]
        return self._bin("!=", other)

    def __lt__(self, other: object) -> "Col":
        return self._bin("<", other)

    def __le__(self, other: object) -> "Col":
        return self._bin("<=", other)

    def __gt__(self, other: object) -> "Col":
        return self._bin(">", other)

    def __ge__(self, other: object) -> "Col":
        return self._bin(">=", other)

    def __add__(self, other: object) -> "Col":
        return self._bin("+", other)

    def __radd__(self, other: object) -> "Col":
        return self._rbin("+", other)

    def __sub__(self, other: object) -> "Col":
        return self._bin("-", other)

    def __rsub__(self, other: object) -> "Col":
        return self._rbin("-", other)

    def __mul__(self, other: object) -> "Col":
        return self._bin("*", other)

    def __rmul__(self, other: object) -> "Col":
        return self._rbin("*", other)

    def __truediv__(self, other: object) -> "Col":
        return self._bin("/", other)

    def __rtruediv__(self, other: object) -> "Col":
        return self._rbin("/", other)

    def __floordiv__(self, other: object) -> "Col":
        return self._bin("//", other)

    def __rfloordiv__(self, other: object) -> "Col":
        return self._rbin("//", other)

    def __mod__(self, other: object) -> "Col":
        return self._bin("%", other)

    def __rmod__(self, other: object) -> "Col":
        return self._rbin("%", other)

    def __and__(self, other: object) -> "Col":
        return self._bin("and", other)

    def __rand__(self, other: object) -> "Col":
        return self._rbin("and", other)

    def __or__(self, other: object) -> "Col":
        return self._bin("or", other)

    def __ror__(self, other: object) -> "Col":
        return self._rbin("or", other)

    def __invert__(self) -> "Col":
        return _wrap("not " + self._text)

    def __neg__(self) -> "Col":
        """Negation; as a sort key it sorts descending, as ``-x`` does."""
        return Col("-(" + self._text + ")")

    def coalesce(self, other: object) -> "Col":
        """``self ?? other``."""
        return self._bin("??", other)

    def is_in(self, values: "Union[Sequence[object], Query, Col]") -> "Col":
        """``in`` a list, a sub-query, or ``param("p")`` bound to a list."""
        return self._in("in", values)

    def not_in(self, values: "Union[Sequence[object], Query, Col]") -> "Col":
        return self._in("not in", values)

    def _in(self, op: str, values: "Union[Sequence[object], Query, Col]") -> "Col":
        if isinstance(values, Query):
            return _wrap(f"{self._text} {op} ({values.inline()})")
        if isinstance(values, Col):
            return _wrap(f"{self._text} {op} {values._text}")
        return _wrap(f"{self._text} {op} {value_text(list(values))}")

    def between(self, low: object, high: object) -> "Col":
        return _wrap(f"{self._text} between {value_text(low)} and {value_text(high)}")

    def not_between(self, low: object, high: object) -> "Col":
        return _wrap(f"{self._text} not between {value_text(low)} and {value_text(high)}")

    def like(self, pattern: "Union[str, Col]", escape: Optional[str] = None) -> "Col":
        """SQL ``like``; ``pattern`` is a string or ``param("p")``."""
        return self._like("like", pattern, escape)

    def ilike(self, pattern: "Union[str, Col]", escape: Optional[str] = None) -> "Col":
        return self._like("ilike", pattern, escape)

    def not_like(self, pattern: "Union[str, Col]", escape: Optional[str] = None) -> "Col":
        return self._like("not like", pattern, escape)

    def not_ilike(self, pattern: "Union[str, Col]", escape: Optional[str] = None) -> "Col":
        return self._like("not ilike", pattern, escape)

    def _like(self, op: str, pattern: "Union[str, Col]", escape: Optional[str]) -> "Col":
        text = f"{self._text} {op} {pattern._text if isinstance(pattern, Col) else duql_string(pattern)}"
        if escape is not None:
            text += " escape " + duql_string(escape)
        return _wrap(text)

    def regex(self, pattern: object) -> "Col":
        return self._bin("~", pattern)

    def iregex(self, pattern: object) -> "Col":
        return self._bin("~*", pattern)

    def not_regex(self, pattern: object) -> "Col":
        return self._bin("!~", pattern)

    def not_iregex(self, pattern: object) -> "Col":
        return self._bin("!~*", pattern)

    def is_null(self) -> "Col":
        return _wrap(self._text + " is null")

    def is_not_null(self) -> "Col":
        return _wrap(self._text + " is not null")

    def is_missing(self) -> "Col":
        return _wrap(self._text + " is missing")

    def is_not_missing(self) -> "Col":
        return _wrap(self._text + " is not missing")

    def icontains(self, text: str, any: bool = False) -> "Col":
        """The case-insensitive substring test ``"text" in path`` (``any``:
        ``in any(path)``); this expression must be a path."""
        return self._contains("in", text, any)

    def not_icontains(self, text: str, any: bool = False) -> "Col":
        return self._contains("not in", text, any)

    def _contains(self, op: str, text: str, any: bool) -> "Col":
        target = f"any({self._text})" if any else self._text
        return _wrap(f"{duql_string(text)} {op} {target}")

    def ref(self, rowset: str, path: str, key: Optional[str] = None) -> "Col":
        """``self -> rowset.path``, or ``self -> rowset(key).path``."""
        target = f"{rowset}({key})" if key else rowset
        return _wrap(f"{self._text} -> {target}.{path}")

    def over(self, width: object = None, *, rows: Optional[int] = None) -> "Col":
        """A frame on a window call: ``.over(rows=n)`` is ``over n rows``,
        ``.over(width)`` is ``over <width>`` (a number, ``duration(...)`` or
        ``param(...)``). Exactly one of the two."""
        if (width is None) == (rows is None):
            raise ValueError("over: give exactly one of width or rows")
        if rows is not None:
            return _wrap(f"{self._text} over {int(rows)} rows")
        return _wrap(f"{self._text} over {value_text(width)}")

    def alias(self, name: str) -> "Named":
        """This expression under ``name`` in ``select``, ``group``, ``window``
        and ``session`` keys."""
        return Named(name, self)

    def asc(self, nulls: Optional[Literal["first", "last"]] = None) -> "SortKey":
        return SortKey(self, False, nulls)

    def desc(self, nulls: Optional[Literal["first", "last"]] = None) -> "SortKey":
        return SortKey(self, True, nulls)

    def __getitem__(self, index: object) -> "Col":
        """``path[index]``: an int, ``param("n")`` or any expression. Raises
        TypeError unless this is a field path."""
        if not _is_path(self._text):
            raise TypeError("an index follows a field path")
        return Col(f"{self._text}[{value_text(index)}]")

    def __getattr__(self, name: str) -> Callable[..., "Col"]:
        if name.startswith("_"):
            raise AttributeError(name)
        return lambda *args, **named: _call(name, (self, *args), named)


class Named:
    """An expression under a name: ``name = value``."""

    __slots__ = ("name", "value")

    def __init__(self, name: str, value: object) -> None:
        self.name = name
        self.value = value if isinstance(value, Col) else lit(value)

    def text(self) -> str:
        return f"{self.name} = {self.value._text}"


class SortKey:
    """A sort key with its direction and null order."""

    __slots__ = ("value", "descending", "nulls")

    def __init__(
        self, value: Col, descending: bool = False, nulls: Optional[Literal["first", "last"]] = None
    ) -> None:
        self.value = value
        self.descending = descending
        self.nulls = nulls

    def text(self) -> str:
        out = ("-(" + self.value._text + ")") if self.descending else self.value._text
        if self.nulls:
            out += " nulls " + self.nulls
        return out


def _call(name: str, args: Sequence[object], named: Dict[str, object]) -> Col:
    parts = [value_text(a) for a in args]
    parts += [f"{k} = {value_text(v)}" for k, v in named.items()]
    return Col(f"{name}({', '.join(parts)})")


class _Fn:
    __slots__ = ("_name",)

    def __init__(self, name: str) -> None:
        self._name = name

    def __call__(self, *args: object, **named: object) -> Col:
        return _call(self._name, args, named)

    def __getattr__(self, name: str) -> "_Fn":
        if name.startswith("_"):
            raise AttributeError(name)
        return _Fn(f"{self._name}.{name}")


class _Functions:
    """``fn.count()``, ``fn.quantile(c("dur"), 0.99)``, ``fn.ns.f(x, k=3)``
    and ``fn("odd name")(x)`` build calls; keyword arguments are named
    arguments."""

    def __call__(self, name: str) -> _Fn:
        return _Fn(name)

    def __getattr__(self, name: str) -> _Fn:
        if name.startswith("_"):
            raise AttributeError(name)
        return _Fn(name)


fn = _Functions()


def c(path: str) -> Col:
    """A record path as written in duql: ``a.b[0]``, ``.x`` (the current
    element), ``^.x`` (the enclosing row), backtick keys. A ``*`` key after
    the first, as in ``args.counters.*.p50``, is a wildcard path, allowed in
    ``select``, ``drop``, ``unpivot`` and ``fn.any`` / ``fn.all``."""
    return Col(path)


def lit(value: object) -> Col:
    """A literal: ``None``, a bool, an int, a finite float, a str or a list of
    them. Raises ValueError for a float that is not finite."""
    return Col(value_text(value))


def param(name: str) -> Col:
    """The parameter ``$name``, bound with :meth:`Pipe.bind`."""
    return Col("$" + name)


def duration(amount: Union[int, float], unit: str) -> Col:
    """``amount`` in ``unit`` (``ns``, ``us``, ``ms``, ``s``, ``m``, ``h``,
    ``d``): ``duration(250, "ms")`` is ``250ms``."""
    if unit not in DUQL_DURATION_UNITS:
        raise ValueError(
            f"unknown duration unit {unit!r}; use one of {', '.join(DUQL_DURATION_UNITS)}"
        )
    if isinstance(amount, bool) or not isinstance(amount, (int, float)) or amount < 0:
        raise ValueError("a duration amount is a non-negative number")
    return Col(value_text(amount) + unit)


def tup(*items: object) -> Col:
    """The tuple ``(a, b, ...)``, for keys such as ``tup(a, b).is_in(...)``."""
    return Col("(" + ", ".join(value_text(i) for i in items) + ")")


def case_(pairs: Sequence[Tuple[object, object]], default: object = None) -> Col:
    """``case { cond => value, ..., else => default }``: the value of the first
    true condition, else ``default`` (null when left out)."""
    return _call("case", [x for pair in pairs for x in pair] + [default], {})
