"""The duql builder: queries as Python values that print as duql text.

A builder query parses to the same syntax tree as the equal text, so the two
give the same plan and rows::

    from dftracer.utils.duql import c, fn, source

    q = (
        source("trace.pfw.gz")
        .where((c("cat") == "POSIX") & (c("dur") > 1000))
        .group("name", n=fn.count(), total=c("dur").sum())
        .sort(-c("total"))
        .take(10)
    )
    q.text()      # 'duql 1\\nfrom "trace.pfw.gz"\\n| where ...'
    q.collect()   # a DataFrame

Every builder call only builds; ``collect``, ``count``, ``first``, ``stream``
and ``explain`` run the query. A method of :class:`Col` that is not listed
here is a call with the column as its first argument: ``c("dur").sum()`` is
``sum(dur)``. Values are never spliced into the text: bind them with
:meth:`Pipe.bind` and read them with :func:`param`.
"""

from __future__ import annotations

import math
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    Iterator,
    Literal,
    Optional,
    Sequence,
    Tuple,
    Union,
)

from .columnar import Expr, _duql_string
from .dftracer_utils_ext import duql_canonical, duql_load_path

if TYPE_CHECKING:
    from .dataframe import DataFrame
    from .trace_viewer import TraceViewer

__all__ = [
    "Col",
    "Named",
    "Pipe",
    "SortKey",
    "Source",
    "c",
    "case_",
    "duration",
    "fn",
    "lit",
    "load_path",
    "param",
    "rowset",
    "source",
    "sub",
    "tup",
]

ParamValue = Union[bool, int, float, str, Sequence[Union[bool, int, float, str]]]
_UNITS = ("ns", "us", "ms", "s", "m", "h", "d")


def _value_text(v: object) -> str:
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
        return _duql_string(v)
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(_value_text(x) for x in v) + "]"
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
        return _wrap(f"{self._text} {op} {_value_text(other)}")

    def _rbin(self, op: str, other: object) -> "Col":
        return _wrap(f"{_value_text(other)} {op} {self._text}")

    def __eq__(self, other: object) -> "Col":  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._bin("==", other)

    def __ne__(self, other: object) -> "Col":  # type: ignore[override]  # ty: ignore[invalid-method-override]
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

    def is_in(self, values: "Union[Sequence[object], Pipe, Col]") -> "Col":
        """``in`` a list, a sub-query, or ``param("p")`` bound to a list."""
        return self._in("in", values)

    def not_in(self, values: "Union[Sequence[object], Pipe, Col]") -> "Col":
        return self._in("not in", values)

    def _in(self, op: str, values: "Union[Sequence[object], Pipe, Col]") -> "Col":
        if isinstance(values, Pipe):
            return _wrap(f"{self._text} {op} ({values._inline()})")
        if isinstance(values, Col):
            return _wrap(f"{self._text} {op} {values._text}")
        return _wrap(f"{self._text} {op} {_value_text(list(values))}")

    def between(self, low: object, high: object) -> "Col":
        return _wrap(f"{self._text} between {_value_text(low)} and {_value_text(high)}")

    def not_between(self, low: object, high: object) -> "Col":
        return _wrap(f"{self._text} not between {_value_text(low)} and {_value_text(high)}")

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
        text = f"{self._text} {op} {pattern._text if isinstance(pattern, Col) else _duql_string(pattern)}"
        if escape is not None:
            text += " escape " + _duql_string(escape)
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
        return _wrap(f"{_duql_string(text)} {op} {target}")

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
        return _wrap(f"{self._text} over {_value_text(width)}")

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
        return Col(f"{self._text}[{_value_text(index)}]")

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
    parts = [_value_text(a) for a in args]
    parts += [f"{k} = {_value_text(v)}" for k, v in named.items()]
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
    element), ``^.x`` (the enclosing row), backtick keys."""
    return Col(path)


def lit(value: object) -> Col:
    """A literal: ``None``, a bool, an int, a finite float, a str or a list of
    them. Raises ValueError for a float that is not finite."""
    return Col(_value_text(value))


def param(name: str) -> Col:
    """The parameter ``$name``, bound with :meth:`Pipe.bind`."""
    return Col("$" + name)


def duration(amount: Union[int, float], unit: str) -> Col:
    """``amount`` in ``unit`` (``ns``, ``us``, ``ms``, ``s``, ``m``, ``h``,
    ``d``): ``duration(250, "ms")`` is ``250ms``."""
    if unit not in _UNITS:
        raise ValueError(f"unknown duration unit {unit!r}; use one of {', '.join(_UNITS)}")
    if isinstance(amount, bool) or not isinstance(amount, (int, float)) or amount < 0:
        raise ValueError("a duration amount is a non-negative number")
    return Col(_value_text(amount) + unit)


def tup(*items: object) -> Col:
    """The tuple ``(a, b, ...)``, for keys such as ``tup(a, b).is_in(...)``."""
    return Col("(" + ", ".join(_value_text(i) for i in items) + ")")


def case_(pairs: Sequence[Tuple[object, object]], default: object = None) -> Col:
    """``case { cond => value, ..., else => default }``: the value of the first
    true condition, else ``default`` (null when left out)."""
    return _call("case", [x for pair in pairs for x in pair] + [default], {})


def sub(pipeline: "Pipe") -> Col:
    """The scalar sub-query ``(from ...)``: the only cell of ``pipeline``."""
    return Col("(" + pipeline._inline() + ")")


def _path(p: Union[str, Col]) -> str:
    return p._text if isinstance(p, Col) else p


def _item(item: object) -> str:
    if isinstance(item, Named):
        return item.text()
    if isinstance(item, str):
        return item
    if isinstance(item, Col):
        return item._text
    raise TypeError("an item is a path string, a Col or a Named (col.alias(name))")


def _items(items: Sequence[object], named: Dict[str, object]) -> str:
    parts = [_item(i) for i in items]
    parts += [Named(k, v).text() for k, v in named.items()]
    return ", ".join(parts)


def _block(fields: Dict[str, object], unnamed: Sequence[object] = ()) -> str:
    parts = [_value_text(v) for v in unnamed]
    parts += [Named(k, v).text() for k, v in fields.items()]
    return "{ " + ", ".join(parts) + " }" if parts else "{ }"


def _sort_keys(keys: Sequence[Union[str, Col, SortKey]]) -> str:
    return ", ".join(k.text() if isinstance(k, SortKey) else _path(k) for k in keys)


JoinKey = Union[str, Col, Tuple[Union[str, Col], Union[str, Col]]]


def _join_key(k: JoinKey) -> str:
    if isinstance(k, tuple):
        left, right = k
        return f"{_path(left)} == {_path(right)}"
    return _path(k)


class Pipe:
    """A duql query under construction. Every stage method returns a new
    Pipe. A Pipe with no source reads the data of the TraceViewer that runs
    it."""

    __slots__ = ("_from", "_decls", "_stages", "_params", "_file")

    def __init__(self) -> None:
        self._from = ""
        self._decls: Tuple[str, ...] = ()
        self._stages: Tuple[str, ...] = ()
        self._params: Dict[str, ParamValue] = {}
        self._file: Optional[Union[str, Col]] = None

    def _copy(self) -> "Pipe":
        p = Pipe()
        p._from = self._from
        p._decls = self._decls
        p._stages = self._stages
        p._params = dict(self._params)
        p._file = self._file
        return p

    def _stage(self, text: str) -> "Pipe":
        p = self._copy()
        p._stages = (*self._stages, text)
        return p

    def _decl(self, text: str) -> "Pipe":
        p = self._copy()
        p._decls = (*self._decls, text)
        return p

    def _inline(self) -> str:
        parts = [self._from] if self._from else []
        return " | ".join(parts + list(self._stages))

    def raw(self) -> str:
        """The builder's duql text, not canonical."""
        return "".join(d + ";\n" for d in self._decls) + self._inline()

    def text(self) -> str:
        """The canonical duql text, ``duql 1`` first. Raises
        DFTUtilsValueError with the parser's message when it does not parse."""
        return duql_canonical(self.raw())

    def __repr__(self) -> str:
        return f"Pipe({self.raw()!r})"

    def bind(self, **params: ParamValue) -> "Pipe":
        """Binds ``$name`` parameters; the values never enter the text."""
        p = self._copy()
        p._params.update(params)
        return p

    @property
    def params(self) -> Dict[str, ParamValue]:
        return dict(self._params)

    def let(self, name: str, pipeline: "Pipe") -> "Pipe":
        """``let name = pipeline;`` before the query."""
        return self._decl(f"let {name} = {pipeline._inline()}")

    def define(self, name: str, body: object, params: Sequence[str] = ()) -> "Pipe":
        """The macro ``def name(params) = body;`` before the query. A ``Pipe``
        body without a source defines a pipeline macro."""
        head = f"{name}({', '.join(params)})" if params else name
        text = body._inline() if isinstance(body, Pipe) else _value_text(body)
        return self._decl(f"def {head} = {text}")

    def where(self, condition: Union[Col, Expr]) -> "Pipe":
        """Keeps the rows where ``condition`` holds; a columnar ``Expr``
        (``F("dur") > 5``) is taken as its duql filter."""
        text = (
            "(" + condition.to_duql() + ")"
            if isinstance(condition, Expr)
            else _value_text(condition)
        )
        return self._stage("where " + text)

    def derive(self, **fields: object) -> "Pipe":
        return self._stage("derive " + _items((), fields))

    def select(self, *items: object, **named: object) -> "Pipe":
        return self._stage("select " + _items(items, named))

    def drop(self, *paths: Union[str, Col]) -> "Pipe":
        return self._stage("drop " + ", ".join(_path(p) for p in paths))

    def rename(self, **new_to_old: Union[str, Col]) -> "Pipe":
        """``rename new = old``: ``rename(n="name")``."""
        return self._stage(
            "rename " + ", ".join(f"{k} = {_path(v)}" for k, v in new_to_old.items())
        )

    def distinct(self, *keys: object) -> "Pipe":
        return self._stage("distinct " + ", ".join(_item(k) for k in keys) if keys else "distinct")

    def group(self, *keys: object, **aggregates: object) -> "Pipe":
        head = _items(keys, {}) + " " if keys else ""
        return self._stage("group " + head + _block(aggregates))

    def agg(self, *unnamed: object, **aggregates: object) -> "Pipe":
        """``agg { ... }``; a positional aggregate is named from its text,
        so ``agg(c("dur").sum())`` gives the column ``sum_dur``."""
        return self._stage("agg " + _block(aggregates, unnamed))

    def window(
        self, *partition: object, sort: Sequence[Union[str, Col, SortKey]] = (), **fields: object
    ) -> "Pipe":
        text = "window "
        if partition:
            text += _items(partition, {}) + " "
        if sort:
            text += "sort " + _sort_keys(sort) + " "
        return self._stage(text + _block(fields))

    def pivot(
        self,
        key: object,
        values: Sequence[object] = (),
        labels: Sequence[str] = (),
        **aggregates: object,
    ) -> "Pipe":
        """``pivot key in [v as label, ...] { ... }``; ``labels`` pairs with
        ``values``, and an empty label keeps the default ``agg.value`` name."""
        text = "pivot " + _item(key)
        if values:
            parts = [_value_text(v) for v in values]
            for i, label in enumerate(labels):
                if label:
                    parts[i] += " as " + label
            text += " in [" + ", ".join(parts) + "]"
        return self._stage(text + " " + _block(aggregates))

    def unpivot(self, *paths: Union[str, Col], key: str, value: str) -> "Pipe":
        return self._stage(f"unpivot {', '.join(_path(p) for p in paths)} as {key}, {value}")

    def sort(self, *keys: Union[str, Col, SortKey]) -> "Pipe":
        """Sorts by ``keys``; ``-c("x")`` or ``c("x").desc()`` descends."""
        return self._stage("sort " + _sort_keys(keys))

    def take(
        self,
        n: Union[int, Col],
        by: Sequence[object] = (),
        sort: Sequence[Union[str, Col, SortKey]] = (),
    ) -> "Pipe":
        text = "take " + _value_text(n)
        if by:
            text += " by " + ", ".join(_item(b) for b in by)
        if sort:
            text += " sort " + _sort_keys(sort)
        return self._stage(text)

    def take_range(self, first: Union[int, Col], last: Union[int, Col]) -> "Pipe":
        """``take first..last``: rows ``first`` to ``last``, 1-based and inclusive."""
        return self._stage(f"take {_value_text(first)}..{_value_text(last)}")

    def skip(self, n: Union[int, Col]) -> "Pipe":
        return self._stage("skip " + _value_text(n))

    def sample(
        self,
        amount: Union[int, float, Col],
        percent: bool = False,
        seed: Optional[Union[int, Col]] = None,
    ) -> "Pipe":
        text = "sample " + _value_text(amount) + ("%" if percent else "")
        if seed is not None:
            text += f" seed {_value_text(seed)}"
        return self._stage(text)

    def expand(
        self,
        path: Union[str, Col],
        as_: Optional[str] = None,
        with_index: Optional[str] = None,
        keep_empty: bool = False,
    ) -> "Pipe":
        text = "expand " + _path(path)
        if as_:
            text += " as " + as_
        if with_index:
            text += " with_index " + with_index
        if keep_empty:
            text += " keep_empty"
        return self._stage(text)

    def parse(self, col: Union[str, Col], pattern: object) -> "Pipe":
        """``parse col ~ pattern``; ``pattern`` is a string or ``param("p")``
        bound to a string, and its named groups become columns."""
        return self._stage(f"parse {_path(col)} ~ {_value_text(pattern)}")

    def _side(self, side: "Union[Pipe, str]") -> str:
        return side if isinstance(side, str) else f"({side._inline()})"

    def lookup(
        self,
        side: "Union[Pipe, str]",
        on: Sequence[JoinKey],
        into: Optional[str] = None,
        how: Literal["left", "inner", "anti"] = "left",
    ) -> "Pipe":
        """``lookup side on k, ...``; ``side`` is a row-set name or a Pipe with
        a source. A key is a path, a Col, or a pair ``(this_side, side_key)``
        for ``a == b``. ``how="inner"`` drops rows with no match; ``"anti"``
        keeps only those and takes no ``into``."""
        if how not in ("left", "inner", "anti"):
            raise ValueError(f"lookup how must be 'left', 'inner' or 'anti', not {how!r}")
        if how == "anti" and into:
            raise ValueError("lookup how='anti' cannot take into")
        text = f"lookup {self._side(side)} on {', '.join(_join_key(k) for k in on)}"
        if how != "left":
            text += " " + how
        return self._stage(text + (f" into {into}" if into else ""))

    def lookup_asof(
        self,
        side: "Union[Pipe, str]",
        on: Sequence[JoinKey],
        time: JoinKey,
        direction: Literal["backward", "forward", "nearest"] = "backward",
        within: Optional[object] = None,
    ) -> "Pipe":
        text = f"lookup {self._side(side)} on {', '.join(_join_key(k) for k in on)} asof {_join_key(time)}"
        if direction != "backward":
            text += " " + direction
        if within is not None:
            text += " within " + _value_text(within)
        return self._stage(text)

    def lookup_overlap(
        self, side: "Union[Pipe, str]", on: Sequence[JoinKey], into: Optional[str] = None
    ) -> "Pipe":
        text = f"lookup {self._side(side)} on {', '.join(_join_key(k) for k in on)} overlap"
        return self._stage(text + (f" into {into}" if into else ""))

    def union(self, other: "Union[Pipe, str]") -> "Pipe":
        """Appends the rows of ``other``: a Pipe with a source, or the name of
        a row set or ``let``."""
        if isinstance(other, str):
            return self._stage("union " + other)
        return self._stage(f"union ({other._inline()})")

    def call(self, call: Col) -> "Pipe":
        return self._stage("call " + call._text)

    def time_range(self, low: object = None, high: object = None, overlap: bool = False) -> "Pipe":
        """``time_range low .. high``; leave a bound out for an open side."""
        if low is None and high is None:
            raise ValueError("time_range needs a low or a high bound")
        text = "time_range " + (_value_text(low) + " " if low is not None else "") + ".."
        if high is not None:
            text += " " + _value_text(high)
        return self._stage(text + (" overlap" if overlap else ""))

    def use(self, name: str, *args: object) -> "Pipe":
        """A stage that calls the pipeline macro ``name`` with ``args``."""
        return self._stage(f"{name}({', '.join(_value_text(a) for a in args)})")

    def call_tree(self) -> "Pipe":
        return self._stage("call_tree")

    def bucket(
        self,
        width: object,
        fill: bool = False,
        as_: Optional[str] = None,
        mode: str = "zero",
        low: Optional[object] = None,
        high: Optional[object] = None,
        every: Optional[object] = None,
        at: Optional[object] = None,
    ) -> "Pipe":
        """``bucket width [every e] [at t] [fill [mode] [from low to high]] [as name]``; ``mode`` is
        ``"zero"``, ``"forward"`` or ``"linear"``, and a mode or range needs ``fill``."""
        if mode not in ("zero", "forward", "linear"):
            raise ValueError("bucket mode is zero, forward or linear")
        if not fill and (mode != "zero" or low is not None or high is not None):
            raise ValueError("a bucket mode or range needs fill")
        if (low is None) != (high is None):
            raise ValueError("a bucket range needs both low and high")
        text = "bucket " + _value_text(width)
        if every is not None:
            text += " every " + _value_text(every)
        if at is not None:
            text += " at " + _value_text(at)
        if fill:
            text += " fill"
        if mode != "zero":
            text += " " + mode
        if low is not None:
            text += f" from {_value_text(low)} to {_value_text(high)}"
        return self._stage(text + (f" as {as_}" if as_ else ""))

    def session(
        self, *keys: object, gap: object, max: Optional[object] = None, as_: Optional[str] = None
    ) -> "Pipe":
        text = "session "
        if keys:
            text += _items(keys, {}) + " "
        text += "gap " + _value_text(gap)
        if max is not None:
            text += " max " + _value_text(max)
        if as_:
            text += " as " + as_
        return self._stage(text)

    def on(self, viewer: "TraceViewer") -> "TraceViewer":
        """The query applied to ``viewer`` with the bound parameters."""
        return viewer.duql(self.raw(), **self._params)

    def _viewer(self) -> "TraceViewer":
        from .trace_viewer import TraceViewer

        f = self._file
        if isinstance(f, Col):
            bound = self._params.get(f._text[1:])
            f = bound if isinstance(bound, str) else None
        if f is None:
            raise ValueError(
                "this query reads no trace file (no source(path) or bound "
                "source(param(...))); run it with q.on(viewer)"
            )
        return TraceViewer(f)

    def collect(self) -> "DataFrame":
        """Runs the query on its source file."""
        return self.on(self._viewer()).collect()

    def stream(self, batch_size: int = 65536) -> "Iterator[DataFrame]":
        return self.on(self._viewer()).stream(batch_size)

    def count(self) -> int:
        """The number of rows the query gives."""
        got = self.agg(__rows=fn.count()).collect().to_dict()
        n = got["__rows"][0]
        return n if isinstance(n, int) else 0

    def first(self) -> Optional[Dict[str, Any]]:
        """The first row as a dict, or None when there is none."""
        got = self.take(1).collect().to_dict()
        if not got or not next(iter(got.values())):
            return None
        return {k: v[0] for k, v in got.items()}

    def explain(self) -> str:
        """The plan, one step per line; nothing is scanned."""
        return self._viewer().explain_duql(self.raw(), **self._params)


class Source:
    """The member text of a duql source, in the order added. Every method
    returns a new Source."""

    __slots__ = ("_members",)

    def __init__(self) -> None:
        self._members: Tuple[str, ...] = ()

    def _add(self, text: str) -> "Source":
        s = Source()
        s._members = (*self._members, text)
        return s

    def rowset(self, name: str, pipeline: "Pipe") -> "Source":
        """``name = pipeline``. Raises ValueError when the pipeline starts
        with a source."""
        body = pipeline._inline()
        if pipeline._from:
            raise ValueError("a source row set cannot start with from")
        return self._add(f"{name} = {body}")

    def define(self, name: str, body: object, params: Sequence[str] = ()) -> "Source":
        """``def name(params) = body``; a ``Pipe`` body defines a pipeline
        macro."""
        return self._add(Pipe().define(name, body, params).raw()[:-2])

    def flag(self, name: str, value: bool) -> "Source":
        """``def name = true`` or ``def name = false``."""
        return self._add(f"def {name} = {'true' if value else 'false'}")

    def text(self) -> str:
        """The members as canonical text joined by ``;`` and a newline.
        Raises DFTUtilsValueError when a member does not parse."""
        if not self._members:
            return ""
        out = duql_canonical("source s {\n" + ";\n".join(self._members) + "\n}")
        return ";\n".join(line[2:-1] for line in out.splitlines()[2:-1])

    def __repr__(self) -> str:
        return f"Source({self.text()!r})"


def source(*files: Union[str, Col]) -> Pipe:
    """A query over trace files: ``from "a.pfw.gz", "b.pfw.gz"``, or over the
    file bound to a parameter: ``source(param("f"))``."""
    if not files:
        raise ValueError("source() needs a file")
    p = Pipe()
    p._from = "from " + ", ".join(f._text if isinstance(f, Col) else _duql_string(f) for f in files)
    p._file = files[0]
    return p


def rowset(name: str) -> Pipe:
    """A query over the row set or ``let`` ``name`` (``all``, ``data``, ...)."""
    p = Pipe()
    p._from = "from " + name
    return p


def load_path(path: str) -> None:
    """Load the ``def`` macros of the ``.duql`` file ``path``, or of every
    ``.duql`` file in the directory ``path``, for every later query of the
    process. They expand after a query's and a source's own macros, like
    those on ``$DFTRACER_DUQL_PATH``.

    Raises:
        DFTUtilsIOError: If a file cannot be read.
        DFTUtilsValueError: If a file does not parse, or defines a macro that
            an already loaded file defines.
    """
    duql_load_path(path)
