# SPDX-License-Identifier: MIT
"""The ``LazyFrame`` Python wrapper: a deferred query over a native batch.

``LazyFrame`` subclasses the native ``LazyFrame``. Builder methods record ops
and return a new ``LazyFrame``; nothing runs until :meth:`collect`, which
materializes a :class:`~dftracer.utils.DataFrame`. Start one with
``df.lazy()`` or ``lazy(df)``. A ``filter``/``with_column`` expression is a
:mod:`~dftracer.utils.expr` ``Expr`` (``col("dur") > 100``); its column
references resolve by position against the frame's schema at that point, so a
``filter``/``with_column`` after a data-dependent op (``pivot`` / ``to_dummies``
/ ``describe``, whose schema is known only after running) is rejected.
"""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Callable,
    Generic,
    Iterator,
    Literal,
    Mapping,
    NamedTuple,
    Sequence,
    TypeVar,
    Union,
    cast,
    overload,
)

from . import dftracer_utils_ext as _ext
from ._aliases import install as _install_aliases
from ._expr_ops import lower_column_ops
from ._group_transforms import GroupTransforms
from ._series_index import default_name
from ._window_spec import norm_window_spec
from .agg import (
    PANDAS_AGG_NAMES,
    AggForm,
    AggKeyword,
    UdafAgg,
    flatten_agg_specs,
    has_agg_leaf,
    select_aggs,
    split_aggs,
)
from .dataframe import DataFrame, JoinHow, WindowSpec, as_names, single_index
from .enums import CAST_DTYPES, rule_to_units
from .expr import (
    Agg,
    AggLeaf,
    Col,
    Expr,
    Named,
    SelectItem,
    as_expr,
    check_quantile,
    col,
    collect_columns,
    emit_ast,
    has_column_op,
    resolve_selectors,
)
from .groupby import GroupKey, GroupResample, GroupRolling, group_quantile, trace_group
from .runtime import TaskHandle
from .series import Series

if TYPE_CHECKING:
    from typing_extensions import Concatenate, ParamSpec

    from ._loc import LazyILoc, LazyLoc, Resampler
    from .enums import DType
    from .runtime import Runtime
    from .trace.viewer import TraceViewer

    _P = ParamSpec("_P")
    _R = TypeVar("_R")

Value = Union[int, float]


def _col_is_not_null(name: str) -> Expr:
    return col(name).is_not_null()


_CastName = Literal["int64", "uint64", "float64", "bool", "string", "binary"]
_CAST_NAMES: tuple[_CastName, ...] = tuple(cast(_CastName, name) for name in CAST_DTYPES)


def _cast_name(dtype: str | DType) -> _CastName:
    by_type: dict[DType, _CastName] = {CAST_DTYPES[name]: name for name in _CAST_NAMES}
    if isinstance(dtype, str):
        for name in _CAST_NAMES:
            if name == dtype:
                return name
    elif dtype in by_type:
        return by_type[dtype]
    raise TypeError(f"cast: a plan casts to {list(_CAST_NAMES)}, not {dtype!r}")


class SinkSummary(NamedTuple):
    """What a :meth:`LazyFrame.sink` run wrote: ``rows`` rows in ``batches``
    batches."""

    rows: int
    batches: int


def _as_plan(other: object) -> LazyFrame:
    """``other`` as a plan: a trace viewer joins and concatenates by its plan."""
    if isinstance(other, _ext.TraceViewer):
        return other.lazy()
    if isinstance(other, LazyFrame):
        return other
    raise TypeError(f"expected a LazyFrame, got {type(other).__name__}")


class LazyFrame(_ext.LazyFrame):
    """A deferred query over a native batch; :meth:`collect` runs it."""

    __slots__ = ("_rt", "_open")
    _open: bool

    # The native type has no constructor of its own: its factories build this
    # class directly (_bind_types), so a call here wraps a plan that a native
    # call built, on ``runtime``.
    def __new__(cls, native: _ext.LazyFrame, runtime: Runtime | None = None) -> LazyFrame:
        if cls is not LazyFrame:
            raise TypeError(
                "LazyFrame cannot be subclassed: the native calls build the class bound by "
                "_bind_types, so an instance of a subclass would come back as a LazyFrame"
            )
        out = cast("LazyFrame", _ext.LazyFrame.share(native))
        out._runtime = runtime
        return out

    def __init__(self, native: _ext.LazyFrame, runtime: Runtime | None = None) -> None:
        pass

    @property
    def _runtime(self) -> Runtime | None:
        return getattr(self, "_rt", None)

    @_runtime.setter
    def _runtime(self, runtime: Runtime | None) -> None:
        self._rt = runtime

    def _new(self, out: LazyFrame) -> LazyFrame:
        """A plan built from this one, on this plan's runtime."""
        out._runtime = self._runtime
        return out

    # -- the index: named columns, or the row position -------------------------
    def _index_names(self) -> list[str]:
        from ._loc import _ROW

        return self.index_names() or [_ROW]

    def set_index(self, names: str | Sequence[str], *, append: bool = False) -> LazyFrame:
        """Name the column(s) ``loc`` / ``resample`` read labels from; the
        columns stay in place. Several names: ``loc[(a, b)]`` matches every
        level, ``loc[a]`` the first. With ``append`` they become levels after
        the current ones (row positions come first when there is no index)."""
        keys = [names] if isinstance(names, str) else list(names)
        if not keys:
            raise ValueError("set_index: at least one column")
        resolve = self._resolve("set_index")
        for name in keys:
            resolve(name)
        if not append:
            return self._new(_ext.LazyFrame.with_index(self, keys))
        current = self.index_names()
        if current:
            return self._new(_ext.LazyFrame.with_index(self, [*current, *keys]))
        first = default_name(0)
        return self._new(
            _ext.LazyFrame.with_index(_ext.LazyFrame.with_row_index(self, first), [first, *keys])
        )

    def reset_index(self, drop: bool = False) -> LazyFrame:
        """Forget the index columns (kept as plain columns unless ``drop``);
        with no index set, prepend the row positions as ``index``."""
        names = self.index_names()
        if not names:
            return self if drop else self._new(_ext.LazyFrame.with_row_index(self, "index"))
        if drop:
            keep = [c for c in self.columns if c not in names]
            return self._new(_ext.LazyFrame.select(self, keep))
        return self._new(_ext.LazyFrame.with_index(self, []))

    @property
    def iloc(self) -> LazyILoc:
        """Rows by a non-negative position or unit-step slice (``slice`` plan
        steps), a position list (``take``) or a Bool mask; columns by
        position. Assignment appends ``with_column`` steps."""
        from ._loc import LazyILoc

        return LazyILoc(self)

    @property
    def loc(self) -> LazyLoc:
        """Rows by index label (a value, an inclusive slice, a list, an
        expression or a Bool mask), columns by name; assignment appends
        ``with_column`` steps (``lf.loc[col("a") > 1, "c"] = 0``)."""
        from ._loc import LazyLoc

        return LazyLoc(self)

    def resample(self, rule: int | str, on: str | None = None, unit: str = "us") -> Resampler:
        """:meth:`DataFrame.resample` as a plan: tumbling windows of ``rule``
        over the index column or ``on``, each aggregate call one streaming
        ``group_by_dynamic`` over the time-sorted plan."""
        from ._loc import Resampler

        time = on if on is not None else single_index(self.index_names(), "resample")
        self._resolve("resample")(time)
        return Resampler(self, time, rule_to_units(rule, unit))

    @property
    def columns(self) -> list[str]:
        """Output column names without running, or ``[]`` when the plan ends in
        a data-dependent op (``pivot`` / ``to_dummies`` / ``describe``)."""
        return _ext.LazyFrame.schema(self)

    @property
    def schema(self) -> dict[str, DType]:
        """Output column names and dtypes without running; a dtype the plan
        cannot know before it runs is ``DType.UNKNOWN``. Empty where
        :attr:`columns` is."""
        from .enums import DType

        return {
            name: DType(code) for name, code, _unit, _zone in _ext.LazyFrame.output_schema(self)
        }

    @property
    def dtypes(self) -> list[DType]:
        """The dtypes of :attr:`schema`, in column order."""
        return list(self.schema.values())

    @property
    def width(self) -> int:
        """The number of output columns (see :attr:`columns`)."""
        return len(self.columns)

    def collect_schema(self) -> dict[str, DType]:
        """:attr:`schema` (polars ``collect_schema``)."""
        return self.schema

    def lazy(self) -> LazyFrame:
        """This plan itself."""
        return self

    def clone(self) -> LazyFrame:
        """This plan itself (plans are immutable)."""
        return self

    def clear(self, n: int = 0) -> LazyFrame:
        """The plan with its columns and no rows (polars ``clear``; ``n > 0``,
        null rows, is not taken)."""
        if n != 0:
            raise ValueError("clear: only n=0 is taken")
        return self.head(0)

    def first(self) -> LazyFrame:
        """The first row as a one-row plan."""
        return self.head(1)

    def last(self) -> LazyFrame:
        """The last row as a one-row plan."""
        return self.tail(1)

    def pipe(
        self,
        function: Callable[Concatenate[LazyFrame, _P], _R],
        *args: _P.args,
        **kwargs: _P.kwargs,
    ) -> _R:
        """``function(self, *args, **kwargs)``."""
        return function(self, *args, **kwargs)

    def _resolve(self, op: str) -> Callable[[str], int]:
        names = _ext.LazyFrame.schema(self)
        if not names:
            raise ValueError(
                op + "(): the frame's schema is data-dependent here (after a "
                "pivot/to_dummies/describe), so an expression's columns cannot "
                "be resolved by name; apply it before that op"
            )
        index = {name: i for i, name in enumerate(names)}
        return index.__getitem__

    def _lowered(self, expr: Expr, op: str) -> tuple[LazyFrame, Expr, list[str]]:
        # A column op (cum_sum, rolling_*, sort, .dt.*, over, ...) reads the
        # whole column: it becomes its own plan step over a temporary column,
        # and the expression continues over that column.
        if not has_column_op(expr):
            return self, expr, []
        return lower_column_ops(self, expr)

    def filter(self, predicate: Expr) -> LazyFrame:
        """Keep rows where ``predicate`` holds."""
        plan, expr, temps = self._lowered(as_expr(predicate), "filter")
        ast: list[tuple[object, ...]] = []
        emit_ast(expr, plan._resolve("filter"), ast)
        out = plan._new(_ext.LazyFrame.filter(plan, ast))
        return out.drop(temps) if temps else out

    def with_column(self, name: str, expr: Expr) -> LazyFrame:
        """Add or replace column ``name`` with ``expr``."""
        plan, expr, temps = self._lowered(as_expr(expr), "with_column")
        ast: list[tuple[object, ...]] = []
        emit_ast(expr, plan._resolve("with_column"), ast)
        out = plan._new(_ext.LazyFrame.with_column(plan, name, ast))
        return out.drop(temps) if temps else out

    def with_columns(self, *named: Named, **columns: Expr) -> LazyFrame:
        """Add or replace several columns from expressions (the polars
        spelling of repeated :meth:`with_column`), positionally as
        ``expr.alias(name)`` or as keywords."""

        out = self
        for item in named:
            if not isinstance(item, Named):
                raise TypeError("with_columns: a positional item must be expr.alias(name)")
            out = out.with_column(item.name, item.expr)
        for name, expr in columns.items():
            out = out.with_column(name, expr)
        return out

    def select(self, *items: SelectItem) -> LazyFrame:
        """Project to ``items``: column names, bare column expressions, or
        named expressions (``(col("a") * 2).alias("b")``), in that order; a
        set of aggregate expressions gives the one-row ``group_by().agg``
        plan, each unaliased aggregate of a column named after that column
        (the polars ``select``)."""

        if items and all(isinstance(i, Agg) for i in items):
            return LazyGroupBy(self, []).agg(*select_aggs(items))
        pairs = resolve_selectors(items)
        out = self
        for name, expr in pairs:
            if expr is not None:
                out = out.with_column(name, expr)
        return out._new(_ext.LazyFrame.select(out, [name for name, _ in pairs]))

    def _schema_complete(self) -> bool:
        """Whether the schema lists every column the plan can produce. A trace
        scan adds columns per batch, so a name it does not list may still be
        a column."""
        return not getattr(self, "_open", False)

    def drop(self, *names: str | Sequence[str]) -> LazyFrame:
        """Every column except ``names``, including the columns a trace scan
        adds per batch. A name no column carries is a ``KeyError`` when the
        schema is complete and ignored otherwise."""
        dropped: list[str] = []
        for n in names:
            dropped.extend([n] if isinstance(n, str) else list(n))
        if self._schema_complete():
            schema = self._resolve("drop")
            for n in dropped:
                schema(n)
        return self._new(_ext.LazyFrame.drop(self, dropped))

    def rename(self, names: Sequence[str] | Mapping[str, str]) -> LazyFrame:
        """Rename columns: a ``{old: new}`` mapping (pandas / polars), which
        also reaches the columns a trace scan adds per batch, or a full
        positional name list."""
        if isinstance(names, Mapping):
            if self._schema_complete():
                current = _ext.LazyFrame.schema(self)
                missing = [k for k in names if k not in current]
                if missing:
                    raise KeyError(f"rename: no column named {missing[0]!r}")
            return self._new(_ext.LazyFrame.rename_columns(self, list(names), list(names.values())))
        return self._new(_ext.LazyFrame.rename(self, list(names)))

    def slice(self, offset: int, length: int) -> LazyFrame:
        """The row window ``[offset, offset + length)``."""
        return self._new(_ext.LazyFrame.slice(self, offset, length))

    def head(self, n: int = 5) -> LazyFrame:
        """The first ``n`` rows."""
        return self._new(_ext.LazyFrame.head(self, n))

    def tail(self, n: int = 5) -> LazyFrame:
        """The last ``n`` rows."""
        return self._new(_ext.LazyFrame.tail(self, n))

    def drop_nulls(self) -> LazyFrame:
        """Drop rows holding any null."""
        return self._new(_ext.LazyFrame.drop_nulls(self))

    def fill_null(self, value: Value) -> LazyFrame:
        """Fill nulls in every column with ``value``."""
        return self._new(_ext.LazyFrame.fill_null(self, value))

    def top_k(self, k: int, *, by: str | Sequence[str], reverse: bool = False) -> LazyFrame:
        """The ``k`` rows with the largest ``by`` (the smallest with
        ``reverse=True``), best first; a null never ranks (polars ``top_k``)."""
        keys = [by] if isinstance(by, str) else list(by)
        present = _col_is_not_null(keys[0])
        for key in keys[1:]:
            present = present & _col_is_not_null(key)
        return self.filter(present).sort(keys, descending=not reverse).head(k)

    def bottom_k(self, k: int, *, by: str | Sequence[str], reverse: bool = False) -> LazyFrame:
        """The ``k`` rows with the smallest ``by``, smallest first (polars
        ``bottom_k``): :meth:`top_k` with ``reverse`` flipped."""
        return self.top_k(k, by=by, reverse=not reverse)

    def shift(self, n: int = 1, *, fill_value: Value | None = None) -> LazyFrame:
        """Every column shifted down by ``n`` rows (up for a negative ``n``);
        the vacated rows are null, or ``fill_value`` (polars ``shift``)."""
        out = self
        for name in self.columns:
            shifted = col(name).shift(n)
            if fill_value is not None:
                shifted = shifted.fill_null(fill_value)
            out = out.with_column(name, shifted)
        return out

    def cast(
        self,
        dtypes: Mapping[str, str | DType] | str | DType,
        *,
        strict: bool = True,
    ) -> LazyFrame:
        """Cast the named columns (a mapping) or every column (one dtype) to
        ``int64``, ``uint64``, ``float64``, ``bool``, ``string`` or ``binary``
        (polars ``cast``). A value that cannot be cast fails when the plan
        runs; ``strict=False`` is not taken."""
        if not strict:
            raise ValueError("cast: only strict=True is taken")
        mapping = (
            dict(dtypes) if isinstance(dtypes, Mapping) else {name: dtypes for name in self.columns}
        )
        out = self
        for name, target in mapping.items():
            out = out.with_column(name, col(name).cast(_cast_name(target)))
        return out

    def gather_every(self, n: int, offset: int = 0) -> LazyFrame:
        """Every ``n``-th row from ``offset`` (polars ``gather_every``)."""
        if n < 1:
            raise ValueError("gather_every: n must be at least 1")
        index = "__dftu_row__"
        step = (col(index) - offset) / n
        keep = (col(index) >= offset) & (step == step.floor())
        return self.with_row_index(index).filter(keep).drop(index)

    def remove(self, predicate: Expr) -> LazyFrame:
        """The rows where ``predicate`` is not true: false and null rows stay
        (polars ``remove``), where :meth:`filter` of the negation drops the
        null ones."""
        predicate = as_expr(predicate)
        return self.filter((~predicate) | predicate.is_null())

    def with_row_index(self, name: str = "index") -> LazyFrame:
        """Prepend an Int64 row-index column."""
        return self._new(_ext.LazyFrame.with_row_index(self, name))

    def null_count(self) -> LazyFrame:
        """A one-row frame of each column's null count."""
        return self._new(_ext.LazyFrame.null_count(self))

    def explode(self, column: str) -> LazyFrame:
        """Expand a List ``column``: each element becomes its own row."""
        return self._new(_ext.LazyFrame.explode(self, column))

    def unnest(self, column: str, keep_empty: bool = False) -> LazyFrame:
        """UNNEST the List ``column`` per morsel, as :meth:`DataFrame.unnest`:
        an empty or null list drops the row unless ``keep_empty``, and a
        ``list<struct>`` flattens into one column per field."""
        return self._new(_ext.LazyFrame.unnest(self, column, bool(keep_empty)))

    def compare_agg(self, variant: LazyFrame, on: int | str | Sequence[str] = 1) -> LazyFrame:
        """:meth:`DataFrame.compare_agg` over the two collected plans. ``on`` is
        an int count of the leading key columns, or the key column name(s)
        (which must lead both plans' schemas)."""
        if isinstance(on, bool):
            raise TypeError("compare_agg: 'on' must be a key name/list or an int count")
        n_key = on if isinstance(on, int) else len(as_names(on))
        if n_key < 1:
            raise ValueError("compare_agg: 'on' must name at least one key column")
        return self._new(_ext.LazyFrame.compare_agg(self, _as_plan(variant), n_key))

    def window(
        self,
        partition_by: Sequence[str] | None = None,
        order_by: Sequence[str] | None = None,
        specs: Sequence[WindowSpec] | None = None,
    ) -> LazyFrame:
        """:meth:`DataFrame.window` over the collected plan (a pipeline
        breaker); ``specs`` take the same tuples."""
        norm = [norm_window_spec(s) for s in (specs or [])]
        return self._new(
            _ext.LazyFrame.window(self, as_names(partition_by), as_names(order_by), norm)
        )

    def gap_fill(
        self,
        partition_by: Sequence[str] | None,
        time: str,
        bucket: int,
        values: str | Sequence[str],
        mode: Literal["none", "locf", "linear"] = "none",
        start: int | None = None,
        end: int | None = None,
    ) -> LazyFrame:
        """:meth:`DataFrame.gap_fill` over the collected plan."""
        if (start is None) != (end is None):
            raise ValueError("gap_fill: pass both start and end, or neither")
        return self._new(
            _ext.LazyFrame.gap_fill(
                self, as_names(partition_by), time, int(bucket), as_names(values), mode, start, end
            )
        )

    def asof(
        self,
        other: LazyFrame,
        on: str,
        by: str | Sequence[str] | None = None,
        direction: Literal["backward", "forward", "nearest"] = "backward",
        tolerance: float | None = None,
    ) -> LazyFrame:
        """:meth:`DataFrame.asof` over the two plans. Both sides are sorted by
        ``(by, on)`` with the spilling sort and merged under the plan's budget
        (:meth:`memory_budget`, else the automatic one), holding only the right
        rows of the current ``by`` group window, so neither plan is collected.
        Rows come out ordered by ``(by, on)``, as the eager join returns them."""
        return self._new(
            _ext.LazyFrame.asof(self, _as_plan(other), on, as_names(by), direction, tolerance)
        )

    def interval(
        self,
        other: LazyFrame,
        point: str,
        lo: str,
        hi: str,
        by: str | Sequence[str] | None = None,
        outer: bool = False,
    ) -> LazyFrame:
        """:meth:`DataFrame.interval` over the two plans. Both sides are sorted
        and merged under the plan's budget, holding the ranges open at the
        current point and leaving in morsels within the budget."""
        return self._new(
            _ext.LazyFrame.interval(self, _as_plan(other), point, lo, hi, as_names(by), bool(outer))
        )

    def unpivot(self, id_vars: list[str], value_vars: list[str]) -> LazyFrame:
        """Reshape wide to long."""
        return self._new(_ext.LazyFrame.unpivot(self, list(id_vars), list(value_vars)))

    def melt(self, id_vars: list[str], value_vars: list[str]) -> LazyFrame:
        """Alias of :meth:`unpivot`."""
        return self._new(_ext.LazyFrame.melt(self, list(id_vars), list(value_vars)))

    def topk(self, name: str, k: int, largest: bool = True) -> LazyFrame:
        """The ``k`` rows with the largest (or smallest) ``name`` values."""
        return self._new(_ext.LazyFrame.topk(self, name, k, largest))

    def group_by(
        self, key: str | Sequence[str] | None = None, *aggs: str, dropna: bool = True
    ) -> LazyFrame | LazyGroupBy:
        """Group by ``key`` (one column, several for a composite key, or
        ``None`` / ``[]`` for the whole plan as one group). With
        ``"op[:column[:out]]"`` specs the group-by step is appended now;
        without, a :class:`LazyGroupBy` takes the aggregates
        (``.agg(...)``, ``.sum()``, ...). A row whose key is null is
        filtered out first, as pandas; ``dropna=False`` keeps the null keys
        as their own group."""
        keys = as_names(key)
        if not aggs:
            return LazyGroupBy(self, keys, dropna=dropna)
        out = self._new(_ext.LazyFrame.group_by(self, keys, list(aggs)))
        return _drop_null_key_groups(out, keys) if dropna else out

    def reduce(self, agg: str) -> LazyFrame:
        """:meth:`DataFrame.reduce` as a streaming one-group group-by: ``agg``
        over every eligible column, one row. The grouped form is
        ``group_by(keys).reduce(agg)``."""
        return self._new(_ext.LazyFrame.reduce(self, agg))

    def agg(
        self, func: AggForm | None = None, axis: int | str = 0, **named: AggKeyword
    ) -> LazyFrame:
        """The pandas ``agg`` forms over the whole plan, one row (see
        :meth:`DataFrame.agg`); ``axis=1`` is refused."""
        if axis not in (0, "index"):
            raise NotImplementedError("agg(axis=1): no per-row reduction across columns")
        return LazyGroupBy(self, []).agg(*([] if func is None else [func]), **named)

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
        return self.reduce("skew_unbiased")

    def kurt(self) -> LazyFrame:
        return self.reduce("kurt_unbiased")

    def sort_by(self, name: str, descending: bool = False, nulls_last: bool = True) -> LazyFrame:
        """Sort by one column (external merge sort, bounded memory)."""
        return self._new(_ext.LazyFrame.sort_by(self, name, descending, nulls_last))

    def gather(self, indices: Sequence[int]) -> LazyFrame:
        """Keep the rows at ``indices`` (any order, repeats allowed); ``take``
        is the pandas name. Not streaming: the whole frame is resident before
        the indices apply."""
        return self._new(_ext.LazyFrame.take(self, list(indices)))

    def filter_mask(self, mask: Series) -> LazyFrame:
        """Keep rows where the precomputed Bool ``mask`` is true, aligned by
        position to this frame's rows."""
        return self._new(_ext.LazyFrame.filter_mask(self, mask))

    def reverse(self) -> LazyFrame:
        """Rows in reverse order. Not streaming."""
        return self._new(_ext.LazyFrame.reverse(self))

    def sort_by_multi(
        self,
        names: Sequence[str],
        descending: bool | Sequence[bool] = False,
        nulls_last: bool | Sequence[bool] = True,
    ) -> LazyFrame:
        """Stable lexicographic sort by several key columns. A single flag
        broadcasts; a sequence maps one flag per name."""
        return self._new(_ext.LazyFrame.sort_by_multi(self, list(names), descending, nulls_last))

    def sort(
        self,
        by: str | Sequence[str],
        *,
        descending: bool | Sequence[bool] = False,
        nulls_last: bool = False,
    ) -> LazyFrame:
        """Sort by one column (external merge sort) or lexicographically by
        several, over :meth:`sort_by` / :meth:`sort_by_multi`. The null-keyed
        rows come first, or last with ``nulls_last=True`` (the polars keywords
        and default): a hidden 0 / 1 presence column sorts ahead of each key."""
        names = [by] if isinstance(by, str) else list(by)
        if isinstance(by, str) and isinstance(descending, bool) and isinstance(nulls_last, bool):
            return self._new(_ext.LazyFrame.sort_by(self, by, descending, nulls_last))
        return self._new(_ext.LazyFrame.sort_by_multi(self, names, descending, nulls_last))

    def sort_values(
        self,
        by: str | Sequence[str],
        ascending: bool | Sequence[bool] = True,
        na_position: Literal["first", "last"] = "last",
    ) -> LazyFrame:
        """The pandas ``sort_values``: :meth:`sort` with ``ascending`` and
        ``na_position`` (nulls last by default, unlike :meth:`sort`)."""
        if na_position not in ("first", "last"):
            raise ValueError("na_position must be 'first' or 'last'")
        if isinstance(ascending, bool):
            return self.sort(by, descending=not ascending, nulls_last=na_position == "last")
        return self.sort(
            by, descending=[not a for a in ascending], nulls_last=na_position == "last"
        )

    def join(
        self,
        other: LazyFrame,
        on: str | Sequence[str] | None = None,
        how: JoinHow = "inner",
        left_on: str | Sequence[str] | None = None,
        right_on: str | Sequence[str] | None = None,
        suffix: str = "_right",
        nulls_equal: bool = False,
    ) -> LazyFrame:
        """Hash join with another ``LazyFrame``, as :meth:`DataFrame.join`
        (``nulls_equal`` included).
        ``other`` is collected in full when this plan runs (the build side);
        this plan streams through it morsel by morsel. No filter or projection
        is moved across the join."""
        if not isinstance(other, (LazyFrame, _ext.TraceViewer)):
            raise TypeError(f"join expects a LazyFrame, got {type(other).__name__}")
        return self._new(
            _ext.LazyFrame.join(
                self,
                _as_plan(other),
                on=on,
                how=how,
                left_on=left_on,
                right_on=right_on,
                suffix=suffix,
                nulls_equal=nulls_equal,
            )
        )

    def concat(self, *others: LazyFrame) -> LazyFrame:
        """Every row of this plan, then every row of each of ``others`` in
        turn (UNION ALL). All plans must share a schema. Streams each side in
        turn with no state; no filter or projection is moved across it."""
        return self._new(_ext.LazyFrame.concat(self, *[_as_plan(o) for o in others]))

    def union(self, *others: LazyFrame) -> LazyFrame:
        """:meth:`concat` then :meth:`unique` (SQL UNION)."""
        return self.concat(*others).unique()

    def unique(self, subset: str | Sequence[str] | None = None) -> LazyFrame:
        """Distinct rows, first occurrence, in original order, keyed on every
        column or on ``subset`` (``drop_duplicates`` is the pandas name)."""
        return self._new(_ext.LazyFrame.unique(self, subset))

    def sample(self, n: int, seed: int = 0) -> LazyFrame:
        """A deterministic ``n``-row sample."""
        return self._new(_ext.LazyFrame.sample(self, n, seed))

    def head_by(self, keys: str | Sequence[str], n: int) -> LazyFrame:
        """For each distinct ``keys`` tuple, the first ``n`` rows with that
        key, in input order. A null key is its own key; equal numbers are one
        key. Streams, holding one counter per distinct key."""
        return self._new(_ext.LazyFrame.head_by(self, keys, n))

    def is_duplicated(self) -> LazyFrame:
        """One Bool column: true where the whole row is duplicated."""
        return self._new(_ext.LazyFrame.is_duplicated(self))

    def is_unique(self) -> LazyFrame:
        """One Bool column: true where the whole row is unique."""
        return self._new(_ext.LazyFrame.is_unique(self))

    def group_by_dynamic(
        self,
        time_col: str,
        every: int,
        period: int | None = None,
        aggs: Sequence[str | Agg | tuple[str, str, list[str]]] | None = None,
        origin: int = 0,
        origin_min: bool = False,
    ) -> LazyFrame:
        """Tumbling/sliding time-window aggregation over an ascending Int64
        time column. ``period`` defaults to ``every`` (tumbling)."""
        return self._new(
            _ext.LazyFrame.group_by_dynamic(
                self, time_col, every, period, dynamic_aggs(self, aggs), origin, origin_min
            )
        )

    def pivot(self, index: str, on: str, values: str, agg: str = "first") -> LazyFrame:
        """Reshape long to wide (schema is data-dependent until collected)."""
        return self._new(_ext.LazyFrame.pivot(self, index, on, values, agg))

    def to_dummies(self, column: str) -> LazyFrame:
        """One-hot encode ``column`` (schema is data-dependent)."""
        return self._new(_ext.LazyFrame.to_dummies(self, column))

    def describe(self) -> LazyFrame:
        """Per-column summary statistics (schema is data-dependent)."""
        return self._new(_ext.LazyFrame.describe(self))

    def memory_budget(self, nbytes: int) -> LazyFrame:
        """Out-of-core spill budget for the pipeline breakers (bytes). 0 means
        auto (~1/3 of memory); a huge value disables spilling."""
        return self._new(_ext.LazyFrame.memory_budget(self, nbytes))

    def auto_spill(self) -> LazyFrame:
        """Set the spill budget to ~1/3 of available memory."""
        return self._new(_ext.LazyFrame.auto_spill(self))

    def collect(self, batch_size: int = 0) -> DataFrame:
        """Run the pipeline and materialize a :class:`DataFrame`. ``batch_size``
        is the chunk a streaming source is pulled in; ``0`` (the default) is
        auto, and runs a resident source whole-column, as the eager path."""
        return _ext.LazyFrame.collect(self, batch_size, self._runtime)

    def collect_async(self, batch_size: int = 0) -> TaskHandle[DataFrame]:
        """Start :meth:`collect` on the native pool and return at once.

        The native plan is copied into the task, so later changes to the sources
        do not reach it. ``get()`` returns the :class:`DataFrame` ``collect`` would,
        or raises its error, and releases the GIL while it waits.
        """
        native = _ext.LazyFrame.collect_async(self, batch_size, self._runtime)
        return TaskHandle(
            native=native,
            name=native.name,
            task_id=native.task_id,
            finish=lambda out: out,
        )

    def sink(
        self,
        name: str,
        **options: str,
    ) -> SinkSummary:
        """Write the pipeline's rows to the sink a plugin registered as ``name``
        and return the totals.

        Every option is a string, e.g. ``lf.sink("csv.write", path="out.csv")``; a
        value that is not a ``str`` raises ``TypeError``. The plan runs on the
        whole pipeline, so this call does the work of :meth:`collect` with no
        frame kept in memory. An unknown name raises with the registered names.
        """
        for key, value in options.items():
            if not isinstance(value, str):
                raise TypeError(f"sink option {key!r} must be a str, not {type(value).__name__}")
        rows, batches = _ext.LazyFrame.sink(self, name, dict(options), 0, self._runtime)
        return SinkSummary(rows=int(rows), batches=int(batches))

    def stream(self, batch_size: int = 0) -> Iterator[DataFrame]:
        """Run the pipeline and yield its rows as :class:`DataFrame` chunks of
        about ``batch_size`` rows (``0`` is auto), holding a bounded amount in
        memory. Each chunk carries its own schema. Starts on the first pull."""
        return (chunk for chunk in _ext.LazyFrame.stream(self, batch_size, self._runtime))

    def __arrow_c_stream__(self, requested_schema: object | None = None) -> object:
        """Stream the query through the Arrow C Stream Interface: one batch per
        ``get_next``, run on the native pool, so ``pa.RecordBatchReader.from_stream``
        and ``pl.DataFrame`` read it without a full collect."""
        return _ext.LazyFrame.__arrow_c_stream__(self, requested_schema, self._runtime)

    def __copy__(self) -> LazyFrame:
        """A new handle on the same plan, with its runtime and schema
        openness."""
        out = _ext.LazyFrame.share(self)
        out._runtime = self._runtime
        out._open = getattr(self, "_open", False)
        return out

    def __deepcopy__(self, memo: object) -> LazyFrame:
        """A plan is an immutable value, so a deep copy is a shallow one."""
        return self.__copy__()

    def __getitem__(self, key: object) -> BoundColumn | LazyFrame:
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
            plan = _ext.LazyFrame.explain(self)
        except Exception:  # noqa: BLE001 - repr must never raise
            return "<LazyFrame>"
        return "<LazyFrame>\n" + plan

    # BEGIN GENERATED aliases from _aliases.py by scripts/gen_pyi.py
    if TYPE_CHECKING:
        aggregate = agg
        drop_duplicates = unique
        take = gather
        limit = head
        select_seq = select
        with_columns_seq = with_columns
        pass
    # END GENERATED aliases


def dynamic_aggs(
    frame: LazyFrame | DataFrame,
    aggs: Sequence[str | Agg | tuple[str, str, list[str]]] | None,
) -> list[str | tuple[str, str, list[str]]]:
    """The ``aggs`` of ``group_by_dynamic``: spec strings and UDAF spec tuples
    stay as they are and an ``Agg`` becomes its spec."""

    out: list[str | tuple[str, str, list[str]]] = []
    for item in aggs or []:
        if isinstance(item, Agg):
            out.extend(agg_spec_strings(frame, [], [item], {}))
        elif isinstance(item, (str, tuple)):
            out.append(item)
        else:
            raise TypeError(f"aggs takes str or Agg items, got {type(item).__name__}")
    return out


def agg_spec_strings(
    frame: LazyFrame | DataFrame,
    keys: Sequence[str],
    specs: Sequence[AggForm],
    named: Mapping[str, AggKeyword],
) -> list[str | tuple[str, str, list[str]]]:
    """The ``op:column:out`` spec strings the streaming group-by and
    ``group_by_dynamic`` take, from ``Agg`` objects, spec strings and the
    pandas ``agg`` forms (a name, a list, a dict, ``out=("column", name)``). A
    UDAF ``Agg`` becomes the tuple ``(udaf_name, out, [column, ...])``."""

    flat = flatten_agg_specs(frame, keys, specs, named)
    strings: list[str | tuple[str, str, list[str]]] = []
    for spec in flat:
        if isinstance(spec, str):
            strings.append(spec)
            continue
        if not isinstance(spec, Agg):
            raise TypeError(f"agg() takes Agg or str specs, got {type(spec).__name__}")
        if isinstance(spec, UdafAgg):
            if not all(isinstance(a, Col) for a in spec.args):
                raise ValueError(
                    f"lazy group_by: the udaf {spec.name!r} takes bare columns; the "
                    "streaming group-by aggregates columns by name"
                )
            strings.append((spec.name, spec.out, [a.name for a in spec.args if isinstance(a, Col)]))
            continue
        if spec.by is not None and not isinstance(spec.by, Col):
            raise ValueError(
                f"lazy group_by: aggregate {spec.out!r} has a computed second column; "
                "the streaming group-by aggregates columns by name"
            )
        tail = ""
        if spec.op == "quantile":
            code = check_quantile(spec.param, spec.interpolation)
            tail = f"::{float(spec.param)!r}:{code}"
        elif spec.by is not None or spec.param not in (None, 0.0):
            by = spec.by.name if isinstance(spec.by, Col) else ""
            tail = f":{by}"
            if spec.param not in (None, 0.0):
                tail += f":{float(spec.param)!r}"
        if spec.value is None:
            strings.append(
                f"{spec.op}::{spec.out}{tail}" if spec.out != spec.op or tail else spec.op
            )
        elif isinstance(spec.value, Col):
            strings.append(f"{spec.op}:{spec.value.name}:{spec.out}{tail}")
        else:
            raise ValueError(
                f"lazy group_by: aggregate {spec.out!r} has a computed value; the "
                "streaming group-by aggregates columns by name"
            )
    return strings


def _drop_null_key_groups(grouped: LazyFrame, keys: Sequence[str]) -> LazyFrame:
    """``grouped`` without the groups whose key is null: a filter over the
    group-by's output (one row per group), not over the input rows."""
    out = grouped
    for k in keys:
        out = out.filter(Col(k).is_not_null())
    return out


class LazyGroupBy(GroupTransforms[LazyFrame]):
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
        self._keys: list[str] = list(keys)
        rows = plan
        if dropna:
            for k in self._keys:
                rows = rows.filter(Col(k).is_not_null())
        self._plan = rows

    def _grouped(self, native: LazyFrame) -> LazyFrame:
        out = self._source._new(native)
        return _drop_null_key_groups(out, self._keys) if self._dropna else out

    @property
    def keys(self) -> list[str]:
        return list(self._keys)

    def agg(self, *specs: AggForm, **named: AggKeyword) -> LazyFrame:
        strings = agg_spec_strings(self._source, self._keys, specs, named)
        return self._grouped(_ext.LazyFrame.group_by(self._source, self._keys, strings))

    def reduce(self, agg: str) -> LazyFrame:
        native = self._source
        if not self._keys:
            return self._source._new(_ext.LazyFrame.reduce(native, agg))
        specs = _ext.LazyFrame._reduce_specs(native, agg, self._keys)
        return self._grouped(_ext.LazyFrame.group_by(native, self._keys, specs))

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
        return self.reduce("skew_unbiased")

    def kurt(self) -> LazyFrame:
        return self.reduce("kurt_unbiased")

    def first(self) -> LazyFrame:
        return self.reduce("first")

    def last(self) -> LazyFrame:
        return self.reduce("last")

    def size(self) -> LazyFrame:
        return self._grouped(_ext.LazyFrame.group_by(self._source, self._keys, ["count::size"]))

    # ---- transforms: one value per input row, in input order ----------------
    # The engine's LazyGroupBy::transform (dftu_lazyframe_group_transform):
    # the window kernel PARTITION BY the keys ORDER BY a row index the plan
    # prepends, then the sort back to input order. A null value stays null.

    def _transform(
        self, kind: str, n: int = 0, method: str = "average", ascending: bool = True
    ) -> LazyFrame:
        return self._plan._new(
            _ext.LazyFrame._group_transform(
                self._plan, self._keys, kind, int(n), method, bool(ascending)
            )
        )

    def median(self) -> LazyFrame:
        """The exact median of each numeric column per group (pandas
        ``GroupBy.median``), as :func:`group_quantile` plan steps."""
        return self.quantile(0.5)

    def quantile(self, q: float = 0.5, interpolation: str = "linear") -> LazyFrame:
        """The exact ``q`` quantile of each numeric column per group; the
        interpolation (``linear``, ``nearest``, ``lower``, ``higher`` or
        ``midpoint``) is linear by default (pandas ``GroupBy.quantile``)."""
        return group_quantile(
            self._plan, self._keys, self._value_columns(), float(q), interpolation
        )

    def ffill(self) -> LazyFrame:
        """:meth:`GroupBy.ffill` as a plan step; ``bfill`` likewise."""
        return self._transform("ffill")

    def bfill(self) -> LazyFrame:
        return self._transform("bfill")

    def rolling(self, window: int) -> GroupRolling:
        """:meth:`GroupBy.rolling` as plan steps: ``.sum() .mean() .min()
        .max()`` (the Series-kernel reductions need an eager group-by)."""

        return GroupRolling(self, int(window))

    def _take_rows(self, indices: Sequence[int]) -> LazyFrame:
        return self.take(indices)

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

    def sample(self, n: int, seed: int = 0, *, random_state: int | None = None) -> LazyFrame:
        """:meth:`GroupBy.sample` as plan steps: the ``n`` rows per group
        whose salted row hash (``mix64``) ranks lowest, in input order."""
        if random_state is not None:
            seed = random_state
        row, key = "__dftu_group_row__", "__dftu_hash__"
        indexed = self._plan.with_row_index(row)
        hashed = indexed.with_column(key, (Col(row) + int(seed)).mix64())
        ordered = hashed.sort_by_multi(self._keys + [key])
        picked = LazyGroupBy(ordered, self._keys).head(int(n))
        return picked.sort_by(row).select(*self._plan.columns)

    def resample(self, rule: int | str, on: str | None = None, unit: str = "us") -> GroupResample:
        """:meth:`GroupBy.resample` as a plan: the Int64 time column (the
        index column or ``on``, in ``unit``) floored to its bucket joins the
        keys; each aggregate call is one ``group_by`` over keys + bucket,
        rows by key then time."""
        time = on if on is not None else single_index(self._plan.index_names(), "resample")
        self._plan._resolve("resample")(time)
        if time in self._keys:
            raise ValueError(f"resample: {time!r} is a group key")
        every = rule_to_units(rule, unit)
        bucket = ((Col(time) / every).floor() * every).cast("int64")
        grouped = LazyGroupBy(self._plan.with_column(time, bucket), self._keys + [time])
        return GroupResample(grouped, self._keys + [time])

    # -- the rest of the pandas GroupBy surface, as streaming compositions ----
    def _value_columns(self, agg: str = "sum") -> list[str]:
        return [s.split(":")[1] for s in _ext.LazyFrame._reduce_specs(self._plan, agg, self._keys)]

    def pipe(
        self,
        func: Callable[Concatenate[LazyGroupBy, _P], _R],
        *args: _P.args,
        **kwargs: _P.kwargs,
    ) -> _R:
        return func(self, *args, **kwargs)

    def get_group(self, key: GroupKey) -> LazyFrame:
        """The rows of one group, by its key value (a tuple for several
        keys), in input order."""
        if not self._keys:
            raise ValueError("get_group: the group-by has no keys")
        return self._plan.set_index(self._keys).loc[key, :]

    def nunique(self) -> LazyFrame:
        """Distinct present values per group and non-key column, exact: a
        group-by over the keys plus the column, then a count per key."""
        out: LazyFrame | None = None
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
        index = plan.index_names()
        if len(index) == 1:
            label = index[0]
        else:
            plan = plan.with_row_index(label)
        cols = self._value_columns("sum")
        out = plan.group_by(self._keys).agg(*[f"{op}:{label}:{c}:{c}" for c in cols])
        # The arg aggregates report the label's repr (a string); a row
        # position parses back to Int64 (a set index keeps its own type only
        # when it is numeric, which the collect-time cast decides).
        if len(index) != 1:
            for c in cols:
                out = out.with_column(c, col(c).cast("int64"))
        return out

    def ohlc(self) -> LazyFrame:
        """Per group and numeric column: ``<c>_open`` / ``_high`` / ``_low`` /
        ``_close`` (first, max, min, last)."""
        specs: list[str] = []
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
        specs: list[str] = []
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

        agg = PANDAS_AGG_NAMES.get(func, func)
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
    def apply(self, func: Callable[[LazyFrame], object]) -> LazyFrame:
        """:meth:`GroupBy.apply` as plan steps: the function is traced once on
        a symbolic group; an aggregate or an expression over aggregates gives
        the keys + ``value`` plan, a row expression the ``value`` column per
        input row. A body the trace cannot follow is refused (a plan has no
        Python tier)."""

        traced = trace_group(func, self._plan.columns, "LazyGroupBy.apply")
        if traced is None:
            raise TypeError("apply: the group function could not be traced into the plan")
        if isinstance(traced, Agg):
            return self.agg(traced.alias("value"))
        assert isinstance(traced, Expr)
        if not has_agg_leaf(traced):
            return self._plan.with_column("value", traced).select("value")
        rewritten, aggs = split_aggs(traced)
        row_cols = [c for c in collect_columns(traced) if c in self._plan.columns]
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

    def filter(self, func: Callable[[LazyFrame], object]) -> LazyFrame:
        """:meth:`GroupBy.filter` as plan steps: the groups whose traced
        condition over aggregates holds, rows in input order."""

        traced = trace_group(func, self._plan.columns, "LazyGroupBy.filter")
        if traced is None:
            raise TypeError("filter: the group function could not be traced into the plan")
        if isinstance(traced, Agg):
            traced = AggLeaf(traced) != 0
        assert isinstance(traced, Expr)
        if not has_agg_leaf(traced) or [
            c for c in collect_columns(traced) if c in self._plan.columns
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


def scan_file(path: str, index_dir: str = "", format: str = "") -> LazyFrame:
    """A plan over the file at ``path``, read by the reader that claims it (the
    one with the highest probe score for the file): a plugin file format, or
    the built-in trace reader (``"jsonl"``) for gzip and JSON-lines files.

    ``format`` names the reader and nothing is probed; an unknown name raises
    ``DFTUtilsValueError`` listing the known ones.

    Nothing is read until :meth:`LazyFrame.collect`, which returns the error of
    a file no reader claims (``no format claims <path>``), of two readers that
    tie, or of the winning reader's own open failure. Use it to read a format
    a plugin added.

    ``index_dir`` is the index directory an :class:`Indexer` built for the file
    (``Indexer([path], index_dir=...)``); the scan skips the chunks that index
    proves hold no matching row. Empty means the index beside the file. A file
    with no index, or one that changed since it was indexed, is read whole.
    """
    return _ext.scan_file(path, index_dir, format)


_ALL_ROWS = (1 << 63) - 1

T = TypeVar("T")


class LazyResult(Generic[T]):
    """A lazy value that is not one frame: the plans it reads and the step
    that turns their frames into the value. :meth:`collect` runs it; pass it
    to :func:`collect_all` to run its plans with others', so plans over the
    same trace share one scan."""

    __slots__ = ("_plans", "_finish")

    def __init__(
        self,
        plans: Sequence[LazyFrame | TraceViewer],
        finish: Callable[[list[DataFrame]], T],
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


def _first_value(frames: list[DataFrame]) -> object:
    frame = frames[0]
    if frame.height == 0:
        return None
    return next(iter(frame.to_dict().values()))[0]


class BoundColumn(Col):
    """A column expression bound to the plan it came from, as ``lf["dur"]``.
    It is an ordinary column expression in a filter or a projection; its
    reductions run over that plan and return a :class:`LazyScalar`."""

    __slots__ = ("_frame",)

    def __init__(self, name: str, frame: LazyFrame) -> None:
        super().__init__(name)
        self._frame = frame

    def _scalar(self, agg: Agg) -> LazyScalar:
        return LazyScalar(self._frame.select(agg.alias("value")))

    def sum(self) -> LazyScalar:
        return self._scalar(Col(self.name).sum())

    def mean(self) -> LazyScalar:
        return self._scalar(Col(self.name).mean())

    def min(self) -> LazyScalar:
        return self._scalar(Col(self.name).min())

    def max(self) -> LazyScalar:
        return self._scalar(Col(self.name).max())

    def var(self) -> LazyScalar:
        return self._scalar(Col(self.name).var())

    def std(self) -> LazyScalar:
        return self._scalar(Col(self.name).std())

    def first(self) -> LazyScalar:
        return self._scalar(Col(self.name).first())

    def last(self) -> LazyScalar:
        return self._scalar(Col(self.name).last())

    def count(self) -> LazyScalar:

        return self._scalar(Agg("count_valid", Col(self.name)))


@overload
def collect_all(roots: Sequence[LazyFrame | TraceViewer]) -> list[DataFrame]: ...
@overload
def collect_all(roots: Sequence[LazyResult[T]]) -> list[T]: ...
@overload
def collect_all(
    roots: Sequence[LazyFrame | TraceViewer | LazyResult[T]],
) -> list[DataFrame | T]: ...
def collect_all(
    roots: Sequence[LazyFrame | TraceViewer | LazyResult[T]],
) -> list[DataFrame] | list[T] | list[DataFrame | T]:
    """Collect every root, one result per root in input order: a
    :class:`DataFrame` for a ``LazyFrame``, the value for a
    :class:`LazyResult`. Plans over the same trace base share one scan; any
    other plan collects as :meth:`LazyFrame.collect` would."""
    plans: list[LazyFrame | TraceViewer] = []
    for root in roots:
        plans.extend(root._plans if isinstance(root, LazyResult) else [root])
    runtime = next((p._runtime for p in plans if p._runtime is not None), None)
    native = [p.lazy() if isinstance(p, _ext.TraceViewer) else p for p in plans]
    frames = _ext.collect_all(native, runtime) if native else []
    out: list[DataFrame | T] = []
    at = 0
    for root in roots:
        if isinstance(root, LazyResult):
            n = len(root._plans)
            out.append(root._finish(frames[at : at + n]))
            at += n
        else:
            out.append(frames[at])
            at += 1
    return out


_install_aliases("LazyFrame", LazyFrame)
_install_aliases("LazyGroupBy", LazyGroupBy)
_ext._bind_types(LazyFrame=LazyFrame)
