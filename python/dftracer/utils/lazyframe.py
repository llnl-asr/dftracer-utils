"""The ``LazyFrame`` Python wrapper: a deferred query over a native batch.

``LazyFrame`` wraps the native ``_LazyFrame`` handle. Builder methods record ops
and return a new ``LazyFrame``; nothing runs until :meth:`collect`, which
materializes a :class:`~dftracer.utils.DataFrame`. Start one with
``df.lazy()`` or ``lazy(df)``. A ``filter``/``with_column`` expression is a
:mod:`~dftracer.utils.columnar` ``Expr`` (``col("dur") > 100``); its column
references resolve by position against the frame's schema at that point, so a
``filter``/``with_column`` after a data-dependent op (``pivot`` / ``to_dummies``
/ ``describe``, whose schema is known only after running) is rejected.
"""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    Generic,
    Iterator,
    List,
    Literal,
    Mapping,
    Optional,
    Sequence,
    Tuple,
    TypeVar,
    Union,
)

from .columnar import Expr, _as_expr, _Col, _emit_ast, col
from .dataframe import DataFrame, JoinHow, WindowSpec, _names, _norm_window_spec
from .series import Series, _unwrap, _wrap

if TYPE_CHECKING:
    from . import dftracer_utils_ext as _ext
    from .columnar import Agg, _GroupResample, _GroupRolling
    from .enums import DType
    from .indexing import LazyILoc, LazyLoc, Resampler
    from .runtime import Runtime

Value = Union[int, float]


class LazyFrame:
    """A deferred query over a native batch; :meth:`collect` runs it."""

    __slots__ = ("_native", "_index", "_runtime")

    def __init__(self, native: "_ext._LazyFrame", runtime: "Optional[Runtime]" = None) -> None:
        self._native = native
        self._index: Optional[List[str]] = None
        self._runtime = runtime

    def _new(self, native: "_ext._LazyFrame") -> "LazyFrame":
        """Wrap a native plan built from this one, on this plan's runtime."""
        return LazyFrame(native, self._runtime)

    def _like(self, native: "_ext._LazyFrame") -> "LazyFrame":
        """:meth:`_new`, also carrying this plan's index column names when the
        result's schema still holds every one of them."""
        out = self._new(native)
        if self._index is not None:
            names = out.columns
            if all(name in names for name in self._index):
                out._index = self._index
        return out

    # -- the index: named columns, or the row position -------------------------
    def _index_names(self) -> List[str]:
        from .indexing import _ROW

        return self._index if self._index is not None else [_ROW]

    def set_index(self, names: Union[str, Sequence[str]]) -> "LazyFrame":
        """Name the column(s) ``loc`` / ``resample`` read labels from; the
        columns stay in place. Several names: ``loc[(a, b)]`` matches every
        level, ``loc[a]`` the first."""
        keys = [names] if isinstance(names, str) else list(names)
        if not keys:
            raise ValueError("set_index: at least one column")
        resolve = self._resolve("set_index")
        for name in keys:
            resolve(name)
        out = self._new(self._native)
        out._index = keys
        return out

    def reset_index(self, drop: bool = False) -> "LazyFrame":
        """Forget the index columns (kept as plain columns unless ``drop``);
        with no index set, prepend the row positions as ``index``."""
        if self._index is None:
            return self if drop else self._like(self._native.with_row_index("index"))
        if drop:
            keep = [c for c in self.columns if c not in self._index]
            return self._new(self._native.select(keep))
        return self._new(self._native)

    @property
    def iloc(self) -> "LazyILoc":
        """Rows by a non-negative position or unit-step slice (``slice`` plan
        steps), a position list (``take``) or a Bool mask; columns by
        position. Assignment appends ``with_column`` steps."""
        from .indexing import LazyILoc

        return LazyILoc(self)

    @property
    def loc(self) -> "LazyLoc":
        """Rows by index label (a value, an inclusive slice, a list, an
        expression or a Bool mask), columns by name; assignment appends
        ``with_column`` steps (``lf.loc[col("a") > 1, "c"] = 0``)."""
        from .indexing import LazyLoc

        return LazyLoc(self)

    def resample(
        self, rule: Union[int, str], on: Optional[str] = None, unit: str = "us"
    ) -> "Resampler":
        """:meth:`DataFrame.resample` as a plan: tumbling windows of ``rule``
        over the index column or ``on``, each aggregate call one streaming
        ``group_by_dynamic`` over the time-sorted plan."""
        from .dataframe import _single_index
        from .indexing import Resampler, rule_to_units

        time = on if on is not None else _single_index(self._index, "resample")
        self._resolve("resample")(time)
        return Resampler(self, time, rule_to_units(rule, unit))

    @property
    def columns(self) -> List[str]:
        """Output column names without running, or ``[]`` when the plan ends in
        a data-dependent op (``pivot`` / ``to_dummies`` / ``describe``)."""
        return self._native.schema()

    @property
    def schema(self) -> "Dict[str, DType]":
        """Output column names and dtypes without running; a dtype the plan
        cannot know before it runs is ``DType.UNKNOWN``. Empty where
        :attr:`columns` is."""
        from .enums import DType

        return {name: DType(code) for name, code in self._native.output_schema()}

    def explain(self) -> str:
        """The optimized plan as text, for introspection and tests."""
        return self._native.explain()

    def _resolve(self, op: str) -> Callable[[str], int]:
        names = self._native.schema()
        if not names:
            raise ValueError(
                op + "(): the frame's schema is data-dependent here (after a "
                "pivot/to_dummies/describe), so an expression's columns cannot "
                "be resolved by name; apply it before that op"
            )
        index = {name: i for i, name in enumerate(names)}
        return index.__getitem__

    def _lowered(self, expr: Expr, op: str) -> "Tuple[LazyFrame, Expr, List[str]]":
        # A column op (cum_sum, rolling_*, sort, .dt.*, over, ...) reads the
        # whole column: it becomes its own plan step over a temporary column,
        # and the expression continues over that column.
        from ._expr_ops import has_column_op, lower_column_ops

        if not has_column_op(expr):
            return self, expr, []
        return lower_column_ops(self, expr)

    def filter(self, predicate: Expr) -> "LazyFrame":
        """Keep rows where ``predicate`` holds."""
        plan, expr, temps = self._lowered(_as_expr(predicate), "filter")
        ast: List[tuple] = []
        _emit_ast(expr, plan._resolve("filter"), ast)
        out = plan._like(plan._native.filter(ast))
        return out.drop(temps) if temps else out

    def with_column(self, name: str, expr: Expr) -> "LazyFrame":
        """Add or replace column ``name`` with ``expr``."""
        plan, expr, temps = self._lowered(_as_expr(expr), "with_column")
        ast: List[tuple] = []
        _emit_ast(expr, plan._resolve("with_column"), ast)
        out = plan._like(plan._native.with_column(name, ast))
        return out.drop(temps) if temps else out

    def with_columns(self, *named: object, **columns: Expr) -> "LazyFrame":
        """Add or replace several columns from expressions (the polars
        spelling of repeated :meth:`with_column`), positionally as
        ``expr.alias(name)`` or as keywords."""
        from .columnar import Named

        out = self
        for item in named:
            if not isinstance(item, Named):
                raise TypeError("with_columns: a positional item must be expr.alias(name)")
            out = out.with_column(item.name, item.expr)
        for name, expr in columns.items():
            out = out.with_column(name, expr)
        return out

    def select(self, *items: object) -> "LazyFrame":
        """Project to ``items``: column names, bare column expressions, or
        named expressions (``(col("a") * 2).alias("b")``), in that order; a
        set of aggregate expressions gives the one-row ``group_by().agg``
        plan, each unaliased aggregate of a column named after that column
        (the polars ``select``)."""
        from .columnar import Agg, resolve_selectors, select_aggs

        if items and all(isinstance(i, Agg) for i in items):
            return LazyGroupBy(self, []).agg(*select_aggs(items))
        pairs = resolve_selectors(items)
        out = self
        for name, expr in pairs:
            if expr is not None:
                out = out.with_column(name, expr)
        return out._new(out._native.select([name for name, _ in pairs]))

    def drop(self, *names: Union[str, Sequence[str]]) -> "LazyFrame":
        """Every column except ``names``."""
        dropped: set = set()
        for n in names:
            dropped.update([n] if isinstance(n, str) else list(n))
        schema = self._resolve("drop")
        for n in dropped:
            schema(n)
        return self._new(
            self._native.select([c for c in self._native.schema() if c not in dropped])
        )

    def rename(self, names: Union[Sequence[str], Mapping[str, str]]) -> "LazyFrame":
        """Rename columns: a ``{old: new}`` mapping (pandas / polars), or a full
        positional name list."""
        if isinstance(names, Mapping):
            current = self._native.schema()
            if not current:
                raise ValueError(
                    "rename(): the schema is data-dependent here; pass a full name list"
                )
            missing = [k for k in names if k not in current]
            if missing:
                raise KeyError(f"rename: no column named {missing[0]!r}")
            names = [names.get(c, c) for c in current]
        return self._like(self._native.rename(list(names)))

    def slice(self, offset: int, length: int) -> "LazyFrame":
        """The row window ``[offset, offset + length)``."""
        return self._like(self._native.slice(offset, length))

    def head(self, n: int = 5) -> "LazyFrame":
        """The first ``n`` rows."""
        return self._like(self._native.head(n))

    def limit(self, n: int = 5) -> "LazyFrame":
        """The first ``n`` rows (polars spelling of :meth:`head`)."""
        return self._like(self._native.head(n))

    def tail(self, n: int = 5) -> "LazyFrame":
        """The last ``n`` rows."""
        return self._like(self._native.tail(n))

    def drop_nulls(self) -> "LazyFrame":
        """Drop rows holding any null."""
        return self._like(self._native.drop_nulls())

    def fill_null(self, value: Value) -> "LazyFrame":
        """Fill nulls in every column with ``value``."""
        return self._like(self._native.fill_null(value))

    def with_row_index(self, name: str = "index") -> "LazyFrame":
        """Prepend an Int64 row-index column."""
        return self._like(self._native.with_row_index(name))

    def null_count(self) -> "LazyFrame":
        """A one-row frame of each column's null count."""
        return self._like(self._native.null_count())

    def explode(self, column: str) -> "LazyFrame":
        """Expand a List ``column``: each element becomes its own row."""
        return self._like(self._native.explode(column))

    def unnest(self, column: str, keep_empty: bool = False) -> "LazyFrame":
        """UNNEST the List ``column`` per morsel, as :meth:`DataFrame.unnest`:
        an empty or null list drops the row unless ``keep_empty``, and a
        ``list<struct>`` flattens into one column per field."""
        return self._like(self._native.unnest(column, bool(keep_empty)))

    def compare_agg(
        self, variant: "LazyFrame", on: Union[int, str, Sequence[str]] = 1
    ) -> "LazyFrame":
        """:meth:`DataFrame.compare_agg` over the two collected plans. ``on`` is
        an int count of the leading key columns, or the key column name(s)
        (which must lead both plans' schemas)."""
        if isinstance(on, bool):
            raise TypeError("compare_agg: 'on' must be a key name/list or an int count")
        n_key = on if isinstance(on, int) else len(_names(on))
        if n_key < 1:
            raise ValueError("compare_agg: 'on' must name at least one key column")
        return self._like(self._native.compare_agg(variant._native, n_key))

    def window(
        self,
        partition_by: Optional[Sequence[str]] = None,
        order_by: Optional[Sequence[str]] = None,
        specs: Optional[Sequence[WindowSpec]] = None,
    ) -> "LazyFrame":
        """:meth:`DataFrame.window` over the collected plan (a pipeline
        breaker); ``specs`` take the same tuples."""
        norm = [_norm_window_spec(s) for s in (specs or [])]
        return self._like(self._native.window(_names(partition_by), _names(order_by), norm))

    def gap_fill(
        self,
        partition_by: Optional[Sequence[str]],
        time: str,
        bucket: int,
        values: Union[str, Sequence[str]],
        mode: Literal["none", "locf", "linear"] = "none",
        start: Optional[int] = None,
        end: Optional[int] = None,
    ) -> "LazyFrame":
        """:meth:`DataFrame.gap_fill` over the collected plan."""
        if (start is None) != (end is None):
            raise ValueError("gap_fill: pass both start and end, or neither")
        return self._new(
            self._native.gap_fill(
                _names(partition_by), time, int(bucket), _names(values), mode, start, end
            )
        )

    def asof(
        self,
        other: "LazyFrame",
        on: str,
        by: Optional[Union[str, Sequence[str]]] = None,
        direction: Literal["backward", "forward", "nearest"] = "backward",
        tolerance: Optional[int] = None,
    ) -> "LazyFrame":
        """:meth:`DataFrame.asof` over the two collected plans."""
        return self._like(self._native.asof(other._native, on, _names(by), direction, tolerance))

    def interval(
        self,
        other: "LazyFrame",
        point: str,
        lo: str,
        hi: str,
        by: Optional[Union[str, Sequence[str]]] = None,
        outer: bool = False,
    ) -> "LazyFrame":
        """:meth:`DataFrame.interval` over the two collected plans."""
        return self._new(
            self._native.interval(other._native, point, lo, hi, _names(by), bool(outer))
        )

    def unpivot(self, id_vars: List[str], value_vars: List[str]) -> "LazyFrame":
        """Reshape wide to long."""
        return self._like(self._native.unpivot(list(id_vars), list(value_vars)))

    def melt(self, id_vars: List[str], value_vars: List[str]) -> "LazyFrame":
        """Alias of :meth:`unpivot`."""
        return self._like(self._native.melt(list(id_vars), list(value_vars)))

    def topk(self, name: str, k: int, largest: bool = True) -> "LazyFrame":
        """The ``k`` rows with the largest (or smallest) ``name`` values."""
        return self._like(self._native.topk(name, k, largest))

    def group_by(
        self, key: Union[str, Sequence[str], None] = None, *aggs: str, dropna: bool = True
    ) -> "Union[LazyFrame, LazyGroupBy]":
        """Group by ``key`` (one column, several for a composite key, or
        ``None`` / ``[]`` for the whole plan as one group). With
        ``"op[:column[:out]]"`` specs the group-by step is appended now;
        without, a :class:`LazyGroupBy` takes the aggregates
        (``.agg(...)``, ``.sum()``, ...). A row whose key is null is
        filtered out first, as pandas; ``dropna=False`` keeps the null keys
        as their own group."""
        keys = _names(key)
        if not aggs:
            return LazyGroupBy(self, keys, dropna=dropna)
        out = self._like(self._native.group_by(keys, list(aggs)))
        return _drop_null_key_groups(out, keys) if dropna else out

    def reduce(self, agg: str) -> "LazyFrame":
        """:meth:`DataFrame.reduce` as a streaming one-group group-by: ``agg``
        over every eligible column, one row. The grouped form is
        ``group_by(keys).reduce(agg)``."""
        return self._like(self._native.reduce(agg))

    def agg(self, func: object = None, axis: "Union[int, str]" = 0, **named: object) -> "LazyFrame":
        """The pandas ``agg`` forms over the whole plan, one row (see
        :meth:`DataFrame.agg`); ``axis=1`` is refused."""
        if axis not in (0, "index"):
            raise NotImplementedError("agg(axis=1): no per-row reduction across columns")
        return LazyGroupBy(self, []).agg(*([] if func is None else [func]), **named)

    aggregate = agg

    def sum(self) -> "LazyFrame":
        return self.reduce("sum")

    def mean(self) -> "LazyFrame":
        return self.reduce("mean")

    def min(self) -> "LazyFrame":
        return self.reduce("min")

    def max(self) -> "LazyFrame":
        return self.reduce("max")

    def count(self) -> "LazyFrame":
        return self.reduce("count_valid")

    def var(self) -> "LazyFrame":
        return self.reduce("var")

    def std(self) -> "LazyFrame":
        return self.reduce("std")

    def skew(self) -> "LazyFrame":
        return self.reduce("skew")

    def kurt(self) -> "LazyFrame":
        return self.reduce("kurt")

    def sort_by(self, name: str, descending: bool = False) -> "LazyFrame":
        """Sort by one column (external merge sort, bounded memory)."""
        return self._like(self._native.sort_by(name, descending))

    def take(self, indices: Sequence[int]) -> "LazyFrame":
        """Keep the rows at ``indices`` (any order, repeats allowed). Not
        streaming: the whole frame is resident before the indices apply."""
        return self._like(self._native.take(list(indices)))

    def filter_mask(self, mask: "Series") -> "LazyFrame":
        """Keep rows where the precomputed Bool ``mask`` is true, aligned by
        position to this frame's rows."""
        return self._like(self._native.filter_mask(_unwrap(mask)))

    def reverse(self) -> "LazyFrame":
        """Rows in reverse order. Not streaming."""
        return self._like(self._native.reverse())

    def sort_by_multi(
        self, names: Sequence[str], descending: Union[bool, Sequence[bool]] = False
    ) -> "LazyFrame":
        """Stable lexicographic sort by several key columns (nulls last). A
        single flag broadcasts; a sequence maps one flag per name."""
        return self._like(self._native.sort_by_multi(list(names), descending))

    def sort(
        self,
        by: Union[str, Sequence[str]],
        descending: Union[bool, Sequence[bool]] = False,
        nulls_last: bool = True,
    ) -> "LazyFrame":
        """Sort by one column (external merge sort) or lexicographically by
        several; the polars spelling over :meth:`sort_by` / :meth:`sort_by_multi`.
        ``nulls_last=False`` puts the null-keyed rows first: a hidden 0 / 1
        presence column sorts ahead of each key."""
        names = [by] if isinstance(by, str) else list(by)
        if not nulls_last:
            from .dataframe import _nulls_first_keys

            keys, flags = _nulls_first_keys(names, descending)
            out = self
            for name, key in zip(keys[::2], names):
                out = out.with_column(name, col(key).is_not_null().cast("int64"))
            return out._new(out._native.sort_by_multi(keys, flags).select(self.columns))
        if isinstance(by, str) and isinstance(descending, bool):
            return self._like(self._native.sort_by(by, descending))
        return self._like(self._native.sort_by_multi(names, descending))

    def sort_values(
        self,
        by: Union[str, Sequence[str]],
        ascending: Union[bool, Sequence[bool]] = True,
        na_position: Literal["first", "last"] = "last",
    ) -> "LazyFrame":
        """The pandas spelling of :meth:`sort`."""
        if na_position not in ("first", "last"):
            raise ValueError("na_position must be 'first' or 'last'")
        if isinstance(ascending, bool):
            return self.sort(by, not ascending, nulls_last=na_position == "last")
        return self.sort(by, [not a for a in ascending], nulls_last=na_position == "last")

    def gather(self, indices: Sequence[int]) -> "LazyFrame":
        """The polars spelling of :meth:`take`."""
        return self.take(indices)

    def join(
        self,
        other: "LazyFrame",
        on: Union[str, Sequence[str], None] = None,
        how: JoinHow = "inner",
        left_on: Union[str, Sequence[str], None] = None,
        right_on: Union[str, Sequence[str], None] = None,
        suffix: str = "_right",
    ) -> "LazyFrame":
        """Hash join with another ``LazyFrame``, as :meth:`DataFrame.join`.
        ``other`` is collected in full when this plan runs (the build side);
        this plan streams through it morsel by morsel. No filter or projection
        is moved across the join."""
        if not isinstance(other, LazyFrame):
            raise TypeError(f"join expects a LazyFrame, got {type(other).__name__}")
        return self._new(
            self._native.join(
                other._native,
                on=on,
                how=how,
                left_on=left_on,
                right_on=right_on,
                suffix=suffix,
            )
        )

    def concat(self, *others: "LazyFrame") -> "LazyFrame":
        """Every row of this plan, then every row of each of ``others`` in
        turn (UNION ALL). All plans must share a schema. Streams each side in
        turn with no state; no filter or projection is moved across it."""
        return self._like(self._native.concat(*[o._native for o in others]))

    def union(self, *others: "LazyFrame") -> "LazyFrame":
        """:meth:`concat` then :meth:`unique` (SQL UNION)."""
        return self.concat(*others).unique()

    def unique(self, subset: "str | Sequence[str] | None" = None) -> "LazyFrame":
        """Distinct rows, first occurrence, in original order, keyed on every
        column or on ``subset``."""
        return self._like(self._native.unique(subset))

    def drop_duplicates(self, subset: "str | Sequence[str] | None" = None) -> "LazyFrame":
        """Alias of :meth:`unique`."""
        return self._like(self._native.drop_duplicates(subset))

    def sample(self, n: int, seed: int = 0) -> "LazyFrame":
        """A deterministic ``n``-row sample."""
        return self._like(self._native.sample(n, seed))

    def is_duplicated(self) -> "LazyFrame":
        """One Bool column: true where the whole row is duplicated."""
        return self._like(self._native.is_duplicated())

    def is_unique(self) -> "LazyFrame":
        """One Bool column: true where the whole row is unique."""
        return self._like(self._native.is_unique())

    def group_by_dynamic(
        self,
        time_col: str,
        every: int,
        period: Union[int, None] = None,
        aggs: Union[List[str], None] = None,
        origin: int = 0,
        origin_min: bool = False,
    ) -> "LazyFrame":
        """Tumbling/sliding time-window aggregation over an ascending Int64
        time column. ``period`` defaults to ``every`` (tumbling)."""
        return self._new(
            self._native.group_by_dynamic(
                time_col, every, period, list(aggs or []), origin, origin_min
            )
        )

    def pivot(self, index: str, on: str, values: str, agg: str = "first") -> "LazyFrame":
        """Reshape long to wide (schema is data-dependent until collected)."""
        return self._like(self._native.pivot(index, on, values, agg))

    def to_dummies(self, column: str) -> "LazyFrame":
        """One-hot encode ``column`` (schema is data-dependent)."""
        return self._like(self._native.to_dummies(column))

    def describe(self) -> "LazyFrame":
        """Per-column summary statistics (schema is data-dependent)."""
        return self._like(self._native.describe())

    def memory_budget(self, nbytes: int) -> "LazyFrame":
        """Out-of-core spill budget for the pipeline breakers (bytes). 0 means
        auto (~1/3 of memory); a huge value disables spilling."""
        return self._like(self._native.memory_budget(nbytes))

    def auto_spill(self) -> "LazyFrame":
        """Set the spill budget to ~1/3 of available memory."""
        return self._like(self._native.auto_spill())

    def collect(self, morsel_rows: int = 0) -> DataFrame:
        """Run the pipeline and materialize a :class:`DataFrame`. ``morsel_rows``
        is the chunk a streaming source is pulled in; ``0`` (the default) is
        auto, and runs a resident source whole-column, as the eager path."""
        return self._finish(_wrap(self._native.collect(morsel_rows, self._runtime)))

    def _finish(self, out: DataFrame) -> DataFrame:
        if self._index is not None and all(name in out for name in self._index):
            out._index = self._index
        return out

    def stream(self, morsel_rows: int = 0) -> "Iterator[DataFrame]":
        """Run the pipeline and yield its rows as :class:`DataFrame` chunks of
        about ``morsel_rows`` rows (``0`` is auto), holding a bounded amount in
        memory. Each chunk carries its own schema. Starts on the first pull."""
        return (_wrap(chunk) for chunk in self._native.stream(morsel_rows, self._runtime))

    def __getitem__(self, key: object) -> "Union[BoundColumn, LazyFrame]":
        """Lazy indexing: a name gives a column expression bound to this plan
        (its reductions are :class:`LazyScalar` values), a list of names a
        :meth:`select`, an expression a :meth:`filter`, and a unit-step slice
        with non-negative bounds a :meth:`slice`."""
        if isinstance(key, str):
            if key not in self.columns:
                raise KeyError(key)
            return BoundColumn(key, self)
        if isinstance(key, slice):
            if key.step not in (None, 1):
                raise ValueError("a lazy slice takes no step")
            start = 0 if key.start is None else key.start
            if start < 0 or (key.stop is not None and key.stop < 0):
                raise ValueError("a lazy slice takes non-negative bounds")
            if key.stop is None:
                return self.slice(start, _ALL_ROWS)
            return self.slice(start, max(0, key.stop - start))
        if isinstance(key, Expr):
            return self.filter(key)
        if isinstance(key, (list, tuple)) and all(isinstance(k, str) for k in key):
            return self.select(*key)
        raise TypeError(f"cannot index a LazyFrame with {type(key).__name__}")

    def __repr__(self) -> str:
        try:
            plan = self._native.explain()
        except Exception:  # noqa: BLE001 - repr must never raise
            return "<LazyFrame>"
        return "<LazyFrame>\n" + plan


def agg_spec_strings(
    frame: object, keys: Sequence[str], specs: Sequence[object], named: Mapping[str, object]
) -> List[str]:
    """The ``op:column:out`` spec strings the streaming group-by and
    ``group_by_dynamic`` take, from ``Agg`` objects, spec strings and the
    pandas ``agg`` forms (a name, a list, a dict, ``out=("column", name)``)."""
    from .columnar import Agg, _agg_specs_from_pandas_forms, _Col

    flat: "List[object]" = []
    for spec in specs:
        if isinstance(spec, Agg) or (isinstance(spec, str) and ":" in spec):
            flat.append(spec)
        else:
            flat.extend(_agg_specs_from_pandas_forms(frame, list(keys), spec, {}))
    if named:
        flat.extend(_agg_specs_from_pandas_forms(frame, list(keys), None, dict(named)))
    if not flat:
        raise TypeError("agg() needs at least one aggregate")
    strings: List[str] = []
    for spec in flat:
        if isinstance(spec, str):
            strings.append(spec)
            continue
        if not isinstance(spec, Agg):
            raise TypeError(f"agg() takes Agg or str specs, got {type(spec).__name__}")
        if spec.by is not None and not isinstance(spec.by, _Col):
            raise ValueError(
                f"lazy group_by: aggregate {spec.out!r} has a computed second column; "
                "the streaming group-by aggregates columns by name"
            )
        tail = ""
        if spec.by is not None or spec.param not in (None, 0.0):
            by = spec.by.name if isinstance(spec.by, _Col) else ""
            tail = f":{by}"
            if spec.param not in (None, 0.0):
                tail += f":{spec.param}"
        if spec.value is None:
            strings.append(
                f"{spec.op}::{spec.out}{tail}" if spec.out != spec.op or tail else spec.op
            )
        elif isinstance(spec.value, _Col):
            strings.append(f"{spec.op}:{spec.value.name}:{spec.out}{tail}")
        else:
            raise ValueError(
                f"lazy group_by: aggregate {spec.out!r} has a computed value; the "
                "streaming group-by aggregates columns by name"
            )
    return strings


def group_quantile(plan: LazyFrame, keys: List[str], columns: List[str], q: float) -> LazyFrame:
    """The exact ``q`` quantile of each of ``columns`` per group, as plan
    steps (pandas ``GroupBy.quantile`` with linear interpolation, the
    default): per column, the present values ranked within the group by a
    window, the two rows bracketing ``q * (n - 1)`` kept, then one group-by
    interpolating between them. Columns join on the keys; the result is
    sorted by the keys, one Float64 column per input column."""
    from .columnar import Agg, _Col

    if not 0.0 <= q <= 1.0:
        raise ValueError(f"quantile: q must be in [0, 1], not {q!r}")
    if not columns:
        raise ValueError("quantile: no numeric column to reduce")
    all_key = "__dftu_all__"
    by = keys or [all_key]
    # A never-null constant key (is_null is present on every row).
    source = (
        plan if keys else plan.with_column(all_key, _Col(columns[0]).is_null().cast("int64") * 0)
    )
    rn, n, pos, lo = "__dftu_rn__", "__dftu_n__", "__dftu_pos__", "__dftu_lo__"
    # Every group, so one with no present value keeps a null row.
    out = source.select(*by).unique()
    for c in columns:
        low, high, frac = f"__dftu_lo_{c}__", f"__dftu_hi_{c}__", f"__dftu_frac_{c}__"
        present = source.select(*(by + [c])).filter(_Col(c).is_not_null())
        ranked = present.window(
            partition_by=by,
            order_by=[c],
            specs=[("row_number", rn), ("frame_count", c, None, None, n)],
        )
        placed = (
            ranked.with_column(pos, (_Col(n) - 1) * q)
            .with_column(lo, _Col(pos).floor())
            .with_column(frac, _Col(pos) - _Col(lo))
        )
        # The rows at floor(pos) and ceil(pos), 1-based row numbers.
        bracket = placed.filter(
            ((_Col(rn) - _Col(lo) - 1) == 0) | ((_Col(rn) - _Col(pos).ceil() - 1) == 0)
        )
        reduced = LazyGroupBy(bracket, by).agg(
            Agg("min", _Col(c), low), Agg("max", _Col(c), high), Agg("min", _Col(frac), frac)
        )
        value = reduced.with_column(c, _Col(low) + (_Col(high) - _Col(low)) * _Col(frac)).select(
            *(by + [c])
        )
        out = out.join(value, on=by, how="left")
    out = out.sort_by_multi(by)
    return out.select(*(keys + columns)) if keys else out.select(*columns)


def _drop_null_key_groups(grouped: LazyFrame, keys: Sequence[str]) -> LazyFrame:
    """``grouped`` without the groups whose key is null: a filter over the
    group-by's output (one row per group), not over the input rows."""
    out = grouped
    for k in keys:
        out = out.filter(_Col(k).is_not_null())
    return out


class LazyGroupBy:
    """The two-step lazy group-by: a plan and its key columns (none makes the
    whole plan one group). Every method appends one streaming group-by step.
    ``.agg(...)`` takes ``"op:column[:out]"`` strings, the pandas forms (a
    name, a list of names, a ``{column: name}`` dict, ``out=("column", name)``
    keywords) and bare-column aggregate expressions (``col("x").sum()``); an
    expression with a computed value, a second column or a parameter has no
    string spec and is refused here (collect first, then :meth:`DataFrame.group_by`)."""

    def __init__(self, plan: LazyFrame, keys: Sequence[str], dropna: bool = True) -> None:
        # A reduction groups the whole plan and drops the null-key groups from
        # its (small) output; a row-wise method (a transform, head, filter)
        # reads the rows with a null key filtered out first, as pandas.
        self._source = plan
        self._dropna = dropna
        self._keys: List[str] = list(keys)
        rows = plan
        if dropna:
            for k in self._keys:
                rows = rows.filter(_Col(k).is_not_null())
        self._plan = rows

    def _grouped(self, native: "_ext._LazyFrame") -> LazyFrame:
        out = self._source._new(native)
        return _drop_null_key_groups(out, self._keys) if self._dropna else out

    @property
    def keys(self) -> List[str]:
        return list(self._keys)

    def agg(self, *specs: object, **named: object) -> LazyFrame:
        strings = agg_spec_strings(self._source, self._keys, specs, named)
        return self._grouped(self._source._native.group_by(self._keys, strings))

    aggregate = agg

    def reduce(self, agg: str) -> LazyFrame:
        native = self._source._native
        if not self._keys:
            return self._source._new(native.reduce(agg))
        return self._grouped(native.group_by(self._keys, native.reduce_specs(agg, self._keys)))

    def sum(self) -> LazyFrame:
        return self.reduce("sum")

    def mean(self) -> LazyFrame:
        return self.reduce("mean")

    def min(self) -> LazyFrame:
        return self.reduce("min")

    def max(self) -> LazyFrame:
        return self.reduce("max")

    def count(self) -> LazyFrame:
        return self.reduce("count_valid")

    def var(self) -> LazyFrame:
        return self.reduce("var")

    def std(self) -> LazyFrame:
        return self.reduce("std")

    def skew(self) -> LazyFrame:
        return self.reduce("skew")

    def kurt(self) -> LazyFrame:
        return self.reduce("kurt")

    def first(self) -> LazyFrame:
        return self.reduce("first")

    def last(self) -> LazyFrame:
        return self.reduce("last")

    def size(self) -> LazyFrame:
        return self._grouped(self._source._native.group_by(self._keys, ["count::size"]))

    # ---- transforms: one value per input row, in input order ----------------
    # The engine's LazyGroupBy::transform (dftu_lazyframe_group_transform):
    # the window kernel PARTITION BY the keys ORDER BY a row index the plan
    # prepends, then the sort back to input order. A null value stays null.

    def _transform(
        self, kind: str, n: int = 0, method: str = "average", ascending: bool = True
    ) -> LazyFrame:
        return self._plan._new(
            self._plan._native.group_transform(self._keys, kind, int(n), method, bool(ascending))
        )

    def cumsum(self) -> LazyFrame:
        return self._transform("cumsum")

    def cummax(self) -> LazyFrame:
        return self._transform("cummax")

    def cummin(self) -> LazyFrame:
        return self._transform("cummin")

    def cumcount(self) -> LazyFrame:
        """The 0-based position of each row within its group, as ``cumcount``."""
        return self._transform("cumcount")

    def shift(self, periods: int = 1) -> LazyFrame:
        """Each column shifted by ``periods`` rows within its group (a negative
        ``periods`` looks ahead); the vacated rows are null."""
        return self._transform("shift", periods)

    def diff(self) -> LazyFrame:
        return self._transform("diff")

    def pct_change(self) -> LazyFrame:
        return self._transform("pct_change")

    def rank(self, method: str = "average", ascending: bool = True) -> LazyFrame:
        """Each numeric column's rank within its group by value: ``method`` is
        ``average`` (the pandas default), ``min``, ``max``, ``dense`` or ``first``; a
        null value has a null rank."""
        return self._transform("rank", 0, method, ascending)

    def ngroup(self) -> LazyFrame:
        """The 0-based number of each row's group, groups numbered in sorted
        key order (pandas ``ngroup``)."""
        return self._transform("ngroup")

    def head(self, n: int = 5) -> LazyFrame:
        """The first ``n`` rows of each group, in input order (pandas
        ``head``)."""
        return self._transform("head", n)

    def tail(self, n: int = 5) -> LazyFrame:
        """The last ``n`` rows of each group, in input order."""
        return self._transform("tail", n)

    def nth(self, n: "Union[int, Sequence[int]]") -> LazyFrame:
        """The ``n``-th row of each group (0-based; negative counts from the
        end), or of each position in a list, in input order."""
        if isinstance(n, int):
            return self._transform("nth", n)
        return self.take(n)

    def cumprod(self) -> LazyFrame:
        """Each numeric column's running product within its group (Float64)."""
        return self._transform("cumprod")

    def prod(self) -> LazyFrame:
        return self.reduce("prod")

    product = prod

    def median(self) -> LazyFrame:
        """The exact median of each numeric column per group (pandas
        ``GroupBy.median``), as :func:`group_quantile` plan steps."""
        return self.quantile(0.5)

    def quantile(self, q: float = 0.5) -> LazyFrame:
        """The exact ``q`` quantile of each numeric column per group, linear
        interpolation (pandas ``GroupBy.quantile``)."""
        return group_quantile(self._plan, self._keys, self._value_columns(), float(q))

    def ffill(self) -> LazyFrame:
        """:meth:`GroupBy.ffill` as a plan step; ``bfill`` likewise."""
        return self._transform("ffill")

    def bfill(self) -> LazyFrame:
        return self._transform("bfill")

    def rolling(self, window: int) -> "_GroupRolling":
        """:meth:`GroupBy.rolling` as plan steps: ``.sum() .mean() .min()
        .max()`` (the Series-kernel reductions need an eager group-by)."""
        from .columnar import _GroupRolling

        return _GroupRolling(self, int(window))

    def take(self, indices: Sequence[int]) -> LazyFrame:
        """:meth:`GroupBy.take` as plan steps: the ``nth`` plans of each
        position, concatenated, deduplicated and sorted back to input order."""
        row = "__dftu_group_row__"
        indexed = self._plan.with_row_index(row)
        picked = [LazyGroupBy(indexed, self._keys).nth(int(i)) for i in indices]
        if not picked:
            return self._plan.head(0)
        out = picked[0].concat(*picked[1:]) if len(picked) > 1 else picked[0]
        return out.unique(row).sort_by(row).select(*self._plan.columns)

    def sample(self, n: int, seed: int = 0, *, random_state: Optional[int] = None) -> LazyFrame:
        """:meth:`GroupBy.sample` as plan steps: the ``n`` rows per group
        whose salted row hash (``mix64``) ranks lowest, in input order."""
        if random_state is not None:
            seed = random_state
        row, key = "__dftu_group_row__", "__dftu_hash__"
        indexed = self._plan.with_row_index(row)
        hashed = indexed.with_column(key, (_Col(row) + int(seed)).mix64())
        ordered = hashed.sort_by_multi(self._keys + [key])
        picked = LazyGroupBy(ordered, self._keys).head(int(n))
        return picked.sort_by(row).select(*self._plan.columns)

    def resample(
        self, rule: "Union[int, str]", on: Optional[str] = None, unit: str = "us"
    ) -> "_GroupResample":
        """:meth:`GroupBy.resample` as a plan: the Int64 time column (the
        index column or ``on``, in ``unit``) floored to its bucket joins the
        keys; each aggregate call is one ``group_by`` over keys + bucket,
        rows by key then time."""
        from .columnar import _GroupResample
        from .dataframe import _single_index
        from .indexing import rule_to_units

        time = on if on is not None else _single_index(self._plan._index, "resample")
        self._plan._resolve("resample")(time)
        if time in self._keys:
            raise ValueError(f"resample: {time!r} is a group key")
        every = rule_to_units(rule, unit)
        bucket = ((_Col(time) / every).floor() * every).cast("int64")
        grouped = LazyGroupBy(self._plan.with_column(time, bucket), self._keys + [time])
        return _GroupResample(grouped, self._keys + [time])

    # -- the rest of the pandas GroupBy surface, as streaming compositions ----
    def _value_columns(self, agg: str = "sum") -> List[str]:
        return [s.split(":")[1] for s in self._plan._native.reduce_specs(agg, self._keys)]

    def pipe(self, func: Callable[..., object], *args: object, **kwargs: object) -> object:
        return func(self, *args, **kwargs)

    def get_group(self, key: object) -> LazyFrame:
        """The rows of one group, by its key value (a tuple for several
        keys), in input order."""
        if not self._keys:
            raise ValueError("get_group: the group-by has no keys")
        return self._plan.set_index(self._keys).loc[key, :]

    def nunique(self) -> LazyFrame:
        """Distinct present values per group and non-key column, exact: a
        group-by over the keys plus the column, then a count per key."""
        out: Optional[LazyFrame] = None
        for c in [n for n in self._plan.columns if n not in self._keys]:
            pairs = LazyGroupBy(self._plan.filter(col(c).is_not_null()), self._keys + [c]).size()
            counts = pairs.group_by(self._keys).agg(f"count::{c}")
            out = counts if out is None else out.join(counts, on=self._keys)
        if out is None:
            raise ValueError("nunique: no columns besides the keys")
        return out

    def sem(self) -> LazyFrame:
        """Standard error of the mean per group: ``std / sqrt(count)``."""
        cols = self._value_columns("std")
        specs = [f"std:{c}:{c}" for c in cols] + [f"count_valid:{c}:__dftu_n_{c}__" for c in cols]
        out = self.agg(*specs)
        for c in cols:
            out = out.with_column(c, col(c) / col(f"__dftu_n_{c}__").cast("float64").sqrt())
        return out.select(*(self._keys + cols))

    def any(self) -> LazyFrame:
        """Per group and numeric column: whether any value is non-zero
        (``all`` likewise)."""
        return self._truth("max")

    def all(self) -> LazyFrame:
        return self._truth("min")

    def _truth(self, agg: str) -> LazyFrame:
        cols = self._value_columns("sum")
        plan = self._plan
        for c in cols:
            plan = plan.with_column(c, (col(c).cast("float64") != 0).cast("int64"))
        out = plan.group_by(self._keys).agg(*[f"{agg}:{c}:{c}" for c in cols])
        for c in cols:
            out = out.with_column(c, col(c) != 0)
        return out

    def idxmax(self) -> LazyFrame:
        """Per group and numeric column: the row position of the maximum
        (``idxmin`` likewise); the index label (as its string repr) when one
        index column is set."""
        return self._arg("argmax")

    def idxmin(self) -> LazyFrame:
        return self._arg("argmin")

    def _arg(self, op: str) -> LazyFrame:
        plan = self._plan
        label = "__dftu_pos__"
        if plan._index is not None and len(plan._index) == 1:
            label = plan._index[0]
        else:
            plan = plan.with_row_index(label)
        cols = self._value_columns("sum")
        out = plan.group_by(self._keys).agg(*[f"{op}:{label}:{c}:{c}" for c in cols])
        # The arg aggregates report the label's repr (a string); a row
        # position parses back to Int64 (a set index keeps its own type only
        # when it is numeric, which the collect-time cast decides).
        if plan._index is None or len(plan._index) != 1:
            for c in cols:
                out = out.with_column(c, col(c).cast("int64"))
        return out

    def ohlc(self) -> LazyFrame:
        """Per group and numeric column: ``<c>_open`` / ``_high`` / ``_low`` /
        ``_close`` (first, max, min, last)."""
        specs: List[str] = []
        for c in self._value_columns("sum"):
            specs += [
                f"first:{c}:{c}_open",
                f"max:{c}:{c}_high",
                f"min:{c}:{c}_low",
                f"last:{c}:{c}_close",
            ]
        return self.agg(*specs)

    def describe(self) -> LazyFrame:
        """Per group and numeric column: ``<c>_count`` / ``_mean`` / ``_std``
        / ``_min`` / ``_25%`` / ``_50%`` / ``_75%`` / ``_max`` (the quartiles
        exact, through :meth:`quantile`), keyed groups only."""
        if not self._keys:
            raise ValueError("describe: a plan needs group keys to join the quartiles")
        specs: List[str] = []
        cols = self._value_columns("sum")
        for c in cols:
            specs += [
                f"count_valid:{c}:{c}_count",
                f"mean:{c}:{c}_mean",
                f"std:{c}:{c}_std",
                f"min:{c}:{c}_min",
                f"max:{c}:{c}_max",
            ]
        out = self.agg(*specs)
        for q in (0.25, 0.5, 0.75):
            names = {c: f"{c}_{int(q * 100)}%" for c in cols}
            out = out.join(self.quantile(q).rename(names), on=self._keys)
        order = [*self._keys]
        for c in cols:
            order += [
                f"{c}_{s}" for s in ("count", "mean", "std", "min", "25%", "50%", "75%", "max")
            ]
        return out.select(*order)

    def value_counts(self, ascending: bool = False) -> LazyFrame:
        """Distinct rows of the non-key columns within each group with their
        count, most frequent first."""
        cols = [c for c in self._plan.columns if c not in self._keys]
        plan = self._plan
        for c in cols:
            plan = plan.filter(col(c).is_not_null())
        out = LazyGroupBy(plan, self._keys + cols).size().rename({"size": "count"})
        return out.sort_values(
            self._keys + ["count"], ascending=[True] * len(self._keys) + [ascending]
        )

    def transform(self, func: str) -> LazyFrame:
        """A group aggregate (``"mean"``, ``"sum"``, ...) broadcast back to
        every row of its group, one column per numeric non-key column, rows
        in input order (a join of the group result back on the keys)."""
        from .columnar import _PANDAS_AGG_NAMES

        agg = _PANDAS_AGG_NAMES.get(func, func)
        cols = self._value_columns(agg)
        row = "__dftu_row__"
        indexed = self._plan.with_row_index(row)
        grouped = indexed.group_by(self._keys).agg(*[f"{agg}:{c}:__dftu_t_{c}__" for c in cols])
        if not self._keys:
            raise ValueError("transform: the group-by has no keys; use reduce")
        joined = indexed.join(grouped, on=self._keys).sort_by(row)
        for c in cols:
            joined = joined.with_column(c, col(f"__dftu_t_{c}__"))
        return joined.select(*cols)

    # -- user functions, traced into the plan (no Python tier on a plan) -------
    def apply(self, func: "Callable[[object], object]") -> LazyFrame:
        """:meth:`GroupBy.apply` as plan steps: the function is traced once on
        a symbolic group; an aggregate or an expression over aggregates gives
        the keys + ``value`` plan, a row expression the ``value`` column per
        input row. A body the trace cannot follow is refused (a plan has no
        Python tier)."""
        from .columnar import Agg, _collect_columns, _trace_group, has_agg_leaf, split_aggs

        traced = _trace_group(func, self._plan.columns, "LazyGroupBy.apply")
        if traced is None:
            raise TypeError("apply: the group function could not be traced into the plan")
        if isinstance(traced, Agg):
            return self.agg(traced.alias("value"))
        assert isinstance(traced, Expr)
        if not has_agg_leaf(traced):
            return self._plan.with_column("value", traced).select("value")
        rewritten, aggs = split_aggs(traced)
        row_cols = [c for c in _collect_columns(traced) if c in self._plan.columns]
        if not row_cols:
            grouped = self.agg(*aggs)
            return grouped.with_column("value", rewritten).select(*(self._keys + ["value"]))
        if not self._keys:
            raise TypeError(
                "apply: a row expression over whole-plan aggregates needs keys on a plan"
            )
        row = "__dftu_row__"
        indexed = self._plan.with_row_index(row)
        grouped = LazyGroupBy(indexed, self._keys).agg(*aggs)
        joined = indexed.join(grouped, on=self._keys).sort_by(row)
        return joined.with_column("value", rewritten).select("value")

    def filter(self, func: "Callable[[object], object]") -> LazyFrame:
        """:meth:`GroupBy.filter` as plan steps: the groups whose traced
        condition over aggregates holds, rows in input order."""
        from .columnar import (
            Agg,
            _AggLeaf,
            _collect_columns,
            _trace_group,
            has_agg_leaf,
            split_aggs,
        )

        traced = _trace_group(func, self._plan.columns, "LazyGroupBy.filter")
        if traced is None:
            raise TypeError("filter: the group function could not be traced into the plan")
        if isinstance(traced, Agg):
            traced = _AggLeaf(traced) != 0
        assert isinstance(traced, Expr)
        if not has_agg_leaf(traced) or [
            c for c in _collect_columns(traced) if c in self._plan.columns
        ]:
            raise TypeError("filter: the group function must return a condition over aggregates")
        rewritten, aggs = split_aggs(traced)
        if not self._keys:
            raise TypeError("filter: the group-by has no keys; use LazyFrame.filter")
        keep = self.agg(*aggs).filter(rewritten).select(*self._keys)
        row = "__dftu_row__"
        indexed = self._plan.with_row_index(row)
        schema = self._plan.columns
        return indexed.join(keep, on=self._keys).sort_by(row).select(*schema)


def lazy(frame: DataFrame) -> LazyFrame:
    """Free-function form of :meth:`DataFrame.lazy`."""
    return frame.lazy()


_ALL_ROWS = (1 << 63) - 1

T = TypeVar("T")


class LazyResult(Generic[T]):
    """A lazy value that is not one frame: the plans it reads and the step
    that turns their frames into the value. :meth:`collect` runs it; pass it
    to :func:`collect_all` to run its plans with others', so plans over the
    same trace share one scan."""

    __slots__ = ("_plans", "_finish")

    def __init__(
        self, plans: Sequence[LazyFrame], finish: "Callable[[List[DataFrame]], T]"
    ) -> None:
        self._plans = list(plans)
        self._finish = finish

    def collect(self) -> T:
        return collect_all([self])[0]

    def __repr__(self) -> str:
        return f"<{type(self).__name__} over {len(self._plans)} plan(s)>"


class LazyScalar(LazyResult[object]):
    """One value from a plan, such as ``tv["dur"].mean()``; :meth:`collect`
    returns the Python value (``None`` for a null)."""

    __slots__ = ()

    def __init__(self, plan: LazyFrame) -> None:
        super().__init__([plan], _first_value)


def _first_value(frames: List[DataFrame]) -> object:
    frame = frames[0]
    if frame.height == 0:
        return None
    return next(iter(frame.to_dict().values()))[0]


class BoundColumn(_Col):
    """A column expression bound to the plan it came from, as ``lf["dur"]``.
    It is an ordinary column expression in a filter or a projection; its
    reductions run over that plan and return a :class:`LazyScalar`."""

    __slots__ = ("_frame",)

    def __init__(self, name: str, frame: LazyFrame) -> None:
        super().__init__(name)
        self._frame = frame

    def _scalar(self, agg: "Agg") -> LazyScalar:
        return LazyScalar(self._frame.select(agg.alias("value")))

    def sum(self) -> LazyScalar:  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._scalar(_Col(self.name).sum())

    def mean(self) -> LazyScalar:  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._scalar(_Col(self.name).mean())

    def min(self) -> LazyScalar:  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._scalar(_Col(self.name).min())

    def max(self) -> LazyScalar:  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._scalar(_Col(self.name).max())

    def var(self) -> LazyScalar:  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._scalar(_Col(self.name).var())

    def std(self) -> LazyScalar:  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._scalar(_Col(self.name).std())

    def first(self) -> LazyScalar:  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._scalar(_Col(self.name).first())

    def last(self) -> LazyScalar:  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._scalar(_Col(self.name).last())

    def count(self) -> LazyScalar:  # type: ignore[override]  # ty: ignore[invalid-method-override]
        from .columnar import Agg

        return self._scalar(Agg("count_valid", _Col(self.name)))


def collect_all(roots: "Sequence[Union[LazyFrame, LazyResult[Any]]]") -> List[Any]:
    """Collect every root, one result per root in input order: a
    :class:`DataFrame` for a ``LazyFrame``, the value for a
    :class:`LazyResult`. Plans over the same trace base share one scan; any
    other plan collects as :meth:`LazyFrame.collect` would."""
    from . import dftracer_utils_ext as _native_ext

    plans: List[LazyFrame] = []
    for root in roots:
        plans.extend(root._plans if isinstance(root, LazyResult) else [root])
    runtime = next((p._runtime for p in plans if p._runtime is not None), None)
    frames = (
        [_wrap(f) for f in _native_ext.collect_all([p._native for p in plans], runtime)]
        if plans
        else []
    )
    out: List[Any] = []
    at = 0
    for root in roots:
        if isinstance(root, LazyResult):
            n = len(root._plans)
            out.append(root._finish(frames[at : at + n]))
            at += n
        else:
            out.append(root._finish(frames[at]))
            at += 1
    return out
