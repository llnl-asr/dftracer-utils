# SPDX-License-Identifier: MIT
"""Pipelines and sources of the duql builder."""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Iterator,
    Literal,
    Mapping,
    Sequence,
    Tuple,
    Union,
)

from ..dataframe import DataFrame
from ..dftracer_utils_ext import duql_canonical, duql_load_path
from ..expr import Expr, duql_string
from ..trace.viewer import TraceViewer
from .col import Col, Item, Named, ParamValue, SortKey, Value, fn, value_text

if TYPE_CHECKING:
    from ..dftracer_utils_ext import Scalar

    pass


def sub(pipeline: Pipe) -> Col:
    """The scalar sub-query ``(from ...)``: the only cell of ``pipeline``."""
    return Col("(" + pipeline.inline() + ")")


def _path(p: str | Col) -> str:
    return p.raw() if isinstance(p, Col) else p


def _item(item: Item) -> str:
    if isinstance(item, Named):
        return item.text()
    if isinstance(item, str):
        return item
    if isinstance(item, Col):
        return item.raw()
    raise TypeError("an item is a path string, a Col or a Named (col.alias(name))")


def _items(items: Sequence[Item], named: Mapping[str, Value]) -> str:
    parts = [_item(i) for i in items]
    parts += [Named(k, v).text() for k, v in named.items()]
    return ", ".join(parts)


def _block(fields: Mapping[str, Value], unnamed: Sequence[Value] = ()) -> str:
    parts = [value_text(v) for v in unnamed]
    parts += [Named(k, v).text() for k, v in fields.items()]
    return "{ " + ", ".join(parts) + " }" if parts else "{ }"


def _sort_keys(keys: Sequence[str | Col | SortKey]) -> str:
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
        self._decls: tuple[str, ...] = ()
        self._stages: tuple[str, ...] = ()
        self._params: dict[str, ParamValue] = {}
        self._file: str | Col | None = None

    def _copy(self) -> Pipe:
        p = Pipe()
        p._from = self._from
        p._decls = self._decls
        p._stages = self._stages
        p._params = dict(self._params)
        p._file = self._file
        return p

    def _stage(self, text: str) -> Pipe:
        p = self._copy()
        p._stages = (*self._stages, text)
        return p

    def _decl(self, text: str) -> Pipe:
        p = self._copy()
        p._decls = (*self._decls, text)
        return p

    def inline(self) -> str:
        parts = [self._from] if self._from else []
        return " | ".join(parts + list(self._stages))

    def raw(self) -> str:
        """The builder's duql text, not canonical."""
        return "".join(d + ";\n" for d in self._decls) + self.inline()

    def text(self) -> str:
        """The canonical duql text, with the engine's ``duql MAJOR.MINOR`` first. Raises
        DFTUtilsValueError with the parser's message when it does not parse."""
        return duql_canonical(self.raw())

    def __repr__(self) -> str:
        return f"Pipe({self.raw()!r})"

    def bind(self, **params: ParamValue) -> Pipe:
        """Binds ``$name`` parameters; the values never enter the text."""
        p = self._copy()
        p._params.update(params)
        return p

    @property
    def params(self) -> dict[str, ParamValue]:
        return dict(self._params)

    def let(self, name: str, pipeline: Pipe) -> Pipe:
        """``let name = pipeline;`` before the query."""
        return self._decl(f"let {name} = {pipeline.inline()}")

    def define(self, name: str, body: Pipe | Value, params: Sequence[str] = ()) -> Pipe:
        """The macro ``def name(params) = body;`` before the query. A ``Pipe``
        body without a source defines a pipeline macro."""
        head = f"{name}({', '.join(params)})" if params else name
        text = body.inline() if isinstance(body, Pipe) else value_text(body)
        return self._decl(f"def {head} = {text}")

    def where(self, condition: Col | Expr) -> Pipe:
        """Keeps the rows where ``condition`` holds; a columnar ``Expr``
        (``F("dur") > 5``) is taken as its duql filter."""
        text = (
            "(" + condition.to_duql() + ")"
            if isinstance(condition, Expr)
            else value_text(condition)
        )
        return self._stage("where " + text)

    def derive(self, **fields: Value) -> Pipe:
        return self._stage("derive " + _items((), fields))

    def select(self, *items: Item, **named: Value) -> Pipe:
        return self._stage("select " + _items(items, named))

    def drop(self, *paths: str | Col) -> Pipe:
        return self._stage("drop " + ", ".join(_path(p) for p in paths))

    def rename(self, **new_to_old: str | Col) -> Pipe:
        """``rename new = old``: ``rename(n="name")``."""
        return self._stage(
            "rename " + ", ".join(f"{k} = {_path(v)}" for k, v in new_to_old.items())
        )

    def distinct(self, *keys: Item) -> Pipe:
        return self._stage("distinct " + ", ".join(_item(k) for k in keys) if keys else "distinct")

    def group(self, *keys: Item, **aggregates: Value) -> Pipe:
        head = _items(keys, {}) + " " if keys else ""
        return self._stage("group " + head + _block(aggregates))

    def agg(self, *unnamed: Value, **aggregates: Value) -> Pipe:
        """``agg { ... }``; a positional aggregate is named from its text,
        so ``agg(c("dur").sum())`` gives the column ``sum_dur``."""
        return self._stage("agg " + _block(aggregates, unnamed))

    def window(
        self, *partition: Item, sort: Sequence[str | Col | SortKey] = (), **fields: Value
    ) -> Pipe:
        text = "window "
        if partition:
            text += _items(partition, {}) + " "
        if sort:
            text += "sort " + _sort_keys(sort) + " "
        return self._stage(text + _block(fields))

    def pivot(
        self,
        key: Item,
        values: Sequence[Value] = (),
        labels: Sequence[str] = (),
        **aggregates: Value,
    ) -> Pipe:
        """``pivot key in [v as label, ...] { ... }``; ``labels`` pairs with
        ``values``, and an empty label keeps the default ``agg.value`` name."""
        text = "pivot " + _item(key)
        if values:
            parts = [value_text(v) for v in values]
            for i, label in enumerate(labels):
                if label:
                    parts[i] += " as " + label
            text += " in [" + ", ".join(parts) + "]"
        return self._stage(text + " " + _block(aggregates))

    def unpivot(self, *paths: str | Col, key: str, value: str) -> Pipe:
        return self._stage(f"unpivot {', '.join(_path(p) for p in paths)} as {key}, {value}")

    def sort(self, *keys: str | Col | SortKey) -> Pipe:
        """Sorts by ``keys``; ``-c("x")`` or ``c("x").desc()`` descends."""
        return self._stage("sort " + _sort_keys(keys))

    def take(
        self,
        n: int | Col,
        by: Sequence[Item] = (),
        sort: Sequence[str | Col | SortKey] = (),
    ) -> Pipe:
        text = "take " + value_text(n)
        if by:
            text += " by " + ", ".join(_item(b) for b in by)
        if sort:
            text += " sort " + _sort_keys(sort)
        return self._stage(text)

    def take_range(self, first: int | Col, last: int | Col) -> Pipe:
        """``take first..last``: rows ``first`` to ``last``, 1-based and inclusive."""
        return self._stage(f"take {value_text(first)}..{value_text(last)}")

    def skip(self, n: int | Col) -> Pipe:
        return self._stage("skip " + value_text(n))

    def sample(
        self,
        amount: int | float | Col,
        percent: bool = False,
        seed: int | Col | None = None,
    ) -> Pipe:
        text = "sample " + value_text(amount) + ("%" if percent else "")
        if seed is not None:
            text += f" seed {value_text(seed)}"
        return self._stage(text)

    def expand(
        self,
        path: str | Col,
        as_: str | None = None,
        with_index: str | None = None,
        keep_empty: bool = False,
    ) -> Pipe:
        text = "expand " + _path(path)
        if as_:
            text += " as " + as_
        if with_index:
            text += " with_index " + with_index
        if keep_empty:
            text += " keep_empty"
        return self._stage(text)

    def parse(self, col: str | Col, pattern: str | Col) -> Pipe:
        """``parse col ~ pattern``; ``pattern`` is a string or ``param("p")``
        bound to a string, and its named groups become columns."""
        return self._stage(f"parse {_path(col)} ~ {value_text(pattern)}")

    def _side(self, side: Pipe | str) -> str:
        return side if isinstance(side, str) else f"({side.inline()})"

    def lookup(
        self,
        side: Pipe | str,
        on: Sequence[JoinKey],
        into: str | None = None,
        how: Literal["left", "inner", "anti"] = "left",
    ) -> Pipe:
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
        side: Pipe | str,
        on: Sequence[JoinKey],
        time: JoinKey,
        direction: Literal["backward", "forward", "nearest"] = "backward",
        within: int | float | Col | None = None,
    ) -> Pipe:
        text = f"lookup {self._side(side)} on {', '.join(_join_key(k) for k in on)} asof {_join_key(time)}"
        if direction != "backward":
            text += " " + direction
        if within is not None:
            text += " within " + value_text(within)
        return self._stage(text)

    def lookup_overlap(
        self, side: Pipe | str, on: Sequence[JoinKey], into: str | None = None
    ) -> Pipe:
        text = f"lookup {self._side(side)} on {', '.join(_join_key(k) for k in on)} overlap"
        return self._stage(text + (f" into {into}" if into else ""))

    def union(self, other: Pipe | str) -> Pipe:
        """Appends the rows of ``other``: a Pipe with a source, or the name of
        a row set or ``let``."""
        if isinstance(other, str):
            return self._stage("union " + other)
        return self._stage(f"union ({other.inline()})")

    def call(self, call: Col) -> Pipe:
        return self._stage("call " + call.raw())

    def sink(self, name: str, **options: str) -> Pipe:
        """Ends the pipeline by writing its rows to the sink ``name``, e.g.
        ``sink("csv", path="out.csv")``. Every option is a string: the sink
        reads them as text. A sink is the last stage of a pipeline, so a stage
        after it does not parse."""
        if not isinstance(name, str):
            raise TypeError(f"a sink name must be a str, not {type(name).__name__}")
        text = "sink " + duql_string(name)
        for key, value in options.items():
            if not isinstance(value, str):
                raise TypeError(f"sink option {key!r} must be a str, not {type(value).__name__}")
            text += f" {key} {duql_string(value)}"
        return self._stage(text)

    def time_range(self, low: Value = None, high: Value = None, overlap: bool = False) -> Pipe:
        """``time_range low .. high``; leave a bound out for an open side."""
        if low is None and high is None:
            raise ValueError("time_range needs a low or a high bound")
        text = "time_range " + (value_text(low) + " " if low is not None else "") + ".."
        if high is not None:
            text += " " + value_text(high)
        return self._stage(text + (" overlap" if overlap else ""))

    def use(self, name: str, *args: Value) -> Pipe:
        """A stage that calls the pipeline macro ``name`` with ``args``."""
        return self._stage(f"{name}({', '.join(value_text(a) for a in args)})")

    def call_tree(self) -> Pipe:
        return self._stage("call_tree")

    def bucket(
        self,
        width: int | float | Col,
        fill: bool = False,
        as_: str | None = None,
        mode: str = "zero",
        low: Value = None,
        high: Value = None,
        every: int | float | Col | None = None,
        at: Value = None,
    ) -> Pipe:
        """``bucket width [every e] [at t] [fill [mode] [from low to high]] [as name]``; ``mode`` is
        ``"zero"``, ``"forward"`` or ``"linear"``, and a mode or range needs ``fill``."""
        if mode not in ("zero", "forward", "linear"):
            raise ValueError("bucket mode is zero, forward or linear")
        if not fill and (mode != "zero" or low is not None or high is not None):
            raise ValueError("a bucket mode or range needs fill")
        if (low is None) != (high is None):
            raise ValueError("a bucket range needs both low and high")
        text = "bucket " + value_text(width)
        if every is not None:
            text += " every " + value_text(every)
        if at is not None:
            text += " at " + value_text(at)
        if fill:
            text += " fill"
        if mode != "zero":
            text += " " + mode
        if low is not None:
            text += f" from {value_text(low)} to {value_text(high)}"
        return self._stage(text + (f" as {as_}" if as_ else ""))

    def session(
        self,
        *keys: Item,
        gap: int | float | Col,
        max: int | float | Col | None = None,
        as_: str | None = None,
    ) -> Pipe:
        text = "session "
        if keys:
            text += _items(keys, {}) + " "
        text += "gap " + value_text(gap)
        if max is not None:
            text += " max " + value_text(max)
        if as_:
            text += " as " + as_
        return self._stage(text)

    def on(self, viewer: TraceViewer) -> TraceViewer:
        """The query applied to ``viewer`` with the bound parameters."""
        return viewer.duql(self.raw(), **self._params)

    def _viewer(self) -> TraceViewer:
        f = self._file
        if isinstance(f, Col):
            bound = self._params.get(f.raw()[1:])
            f = bound if isinstance(bound, str) else None
        if f is None:
            raise ValueError(
                "this query reads no trace file (no source(path) or bound "
                "source(param(...))); run it with q.on(viewer)"
            )
        return TraceViewer(f)

    def collect(self) -> DataFrame:
        """Runs the query on its source file."""
        return self.on(self._viewer()).collect()

    def stream(self, batch_size: int = 65536) -> Iterator[DataFrame]:
        return self.on(self._viewer()).stream(batch_size)

    def count(self) -> int:
        """The number of rows the query gives."""
        got = self.agg(__rows=fn.count()).collect().to_dict()
        n = got["__rows"][0]
        return n if isinstance(n, int) else 0

    def first(self) -> dict[str, Scalar | None] | None:
        """The first row as a dict, or None when there is none."""
        got = self.take(1).collect().to_dict()
        if not got or not next(iter(got.values())):
            return None
        return {k: v[0] for k, v in got.items()}

    def explain(self, analyze: bool = False) -> str:
        """The plan, one step per line; nothing is scanned. With ``analyze=True``
        the plan also runs to its end, its rows dropped, and the text adds the
        rows and time of each stage, the filters the source really took and the
        counters the plugins added."""
        prefix = "explain analyze\n" if analyze else ""
        return self._viewer().explain_duql(prefix + self.raw(), **self._params)


class Source:
    """The member text of a duql source, in the order added. Every method
    returns a new Source."""

    __slots__ = ("_members",)

    def __init__(self) -> None:
        self._members: tuple[str, ...] = ()

    def _add(self, text: str) -> Source:
        s = Source()
        s._members = (*self._members, text)
        return s

    def rowset(self, name: str, pipeline: Pipe) -> Source:
        """``name = pipeline``. Raises ValueError when the pipeline starts
        with a source."""
        body = pipeline.inline()
        if pipeline._from:
            raise ValueError("a source row set cannot start with from")
        return self._add(f"{name} = {body}")

    def define(self, name: str, body: Pipe | Value, params: Sequence[str] = ()) -> Source:
        """``def name(params) = body``; a ``Pipe`` body defines a pipeline
        macro."""
        return self._add(Pipe().define(name, body, params).raw()[:-2])

    def flag(self, name: str, value: bool) -> Source:
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


def source(*files: str | Col) -> Pipe:
    """A query over trace files: ``from "a.pfw.gz", "b.pfw.gz"``, or over the
    file bound to a parameter: ``source(param("f"))``."""
    if not files:
        raise ValueError("source() needs a file")
    p = Pipe()
    p._from = "from " + ", ".join(f.raw() if isinstance(f, Col) else duql_string(f) for f in files)
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
    those on ``$DFTRACER_UTILS_DUQL_PATH``.

    Raises:
        DFTUtilsIOError: If a file cannot be read.
        DFTUtilsValueError: If a file does not parse, or defines a macro that
            an already loaded file defines.
    """
    duql_load_path(path)
