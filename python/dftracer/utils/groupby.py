# SPDX-License-Identifier: MIT
"""``GroupBy``: the group-by over a native ``DataFrame`` and the windows,
resamples and traces that run per group."""

from __future__ import annotations

import functools
from typing import (
    TYPE_CHECKING,
    Callable,
    List,
    Optional,
    Sequence,
    Tuple,
    TypeVar,
    Union,
    cast,
    overload,
)

from . import dftracer_utils_ext as _ext
from ._aliases import install as _install_aliases
from ._apply import trace_errors, uses_identity_test, warn_fallback
from ._group_labels import (
    apply_pieces,
    group_frame,
    labelled_frame,
    labelled_series,
    pandas_named,
    top,
)
from ._group_labels import take as labelled_take
from ._group_transforms import GroupTransforms
from ._series_index import default_name, to_frame, with_levels
from ._series_index import positions as row_positions
from .agg import (
    PANDAS_AGG_NAMES,
    AggForm,
    AggKeyword,
    flatten_agg_specs,
    has_agg_leaf,
    reduce_specs,
    split_aggs,
    to_agg,
)
from .dataframe import DataFrame, single_index
from .enums import DType, rule_to_units
from .expr import Agg, AggLeaf, Col, Expr, check_quantile, collect_columns
from .series import Series

if TYPE_CHECKING:
    from .dftracer_utils_ext import Scalar
    from .lazyframe import LazyFrame, LazyGroupBy

__all__ = ["GroupBy", "SeriesGroupBy"]

_T = TypeVar("_T")

#: The value of a group key: one scalar, or a tuple of them for several keys.
GroupKey = Union["Scalar", None, Tuple[Union["Scalar", None], ...]]


_CMP_GT = 0  # DFTU_CMP_GT, dataframe/c_api.h
_CMP_LT = 2  # DFTU_CMP_LT


class GroupBy(GroupTransforms[DataFrame]):
    """A lazy group-by over a native ``DataFrame`` (zero or more key columns;
    none makes the whole frame one group). ``.agg(...)`` takes aggregate
    expressions (``col("x").sum()``, ``count()``), ``"op:column"`` strings, or
    the pandas forms (a name, a list of names, a ``{column: name}`` dict,
    ``out=("column", name)`` keywords); ``.sum()`` and its family broadcast one
    aggregate over every eligible non-key column."""

    def __init__(
        self,
        batch: DataFrame,
        keys: str | Sequence[str] | None = None,
    ) -> None:
        self._batch = batch
        self._keys: list[str] = (
            [] if keys is None else [keys] if isinstance(keys, str) else list(keys)
        )

    @property
    def keys(self) -> list[str]:
        return list(self._keys)

    @overload
    def __getitem__(self, columns: str) -> SeriesGroupBy: ...
    @overload
    def __getitem__(self, columns: Sequence[str]) -> GroupBy: ...
    def __getitem__(self, columns: str | Sequence[str]) -> GroupBy:
        """``groupby(k)["v"]`` / ``groupby(k)[["v", "w"]]``: the same groups
        over only those value columns (the pandas column selection). With
        one column, a list of aggregate names labels the outputs by name
        alone, as pandas does."""
        frame = self._batch
        picked = [columns] if isinstance(columns, str) else list(columns)
        for c in picked:
            if c not in frame:
                raise KeyError(f"groupby: no column named {c!r}")
        kind = SeriesGroupBy if isinstance(columns, str) else GroupBy
        out = kind(
            frame.select(self._keys + [c for c in picked if c not in self._keys]), self._keys
        )
        out._single = picked[0] if len(picked) == 1 else None
        out._sort = self._sort
        out._as_index = self._as_index
        out._origin = self._origin
        out._rows = self._rows
        out._total = self._total
        return out

    _single: Optional[str] = None
    _rows: Optional[Series] = None
    _total: Optional[int] = None
    _sort: bool = False
    _as_index: Optional[bool] = None
    _origin: Optional[DataFrame] = None

    def _sorted(self, out: DataFrame) -> DataFrame:
        if self._sort and self._keys:
            out = out.sort_by_multi(self._keys)
        if self._as_index is None or not self._keys:
            return out
        if self._as_index:
            if all(k in out for k in self._keys):
                out = _ext.DataFrame.with_index(out, self._keys)
        elif out.index_names():
            out = out.reset_index()
        return out

    def agg(self, *specs: AggForm, **named: AggKeyword) -> DataFrame:
        flat = flatten_agg_specs(self._batch, self._keys, specs, named)
        native = self._batch
        aggs = [to_agg(s) for s in flat]
        # Plain aggregates over plain columns take the engine's group_by
        # directly; anything with an expression goes through the
        # expression group-by.
        plain = bool(self._keys) and all(
            type(a) is Agg
            and a.by is None
            and a.param == 0.0
            and (a.value is None or type(a.value) is Col)
            for a in aggs
        )
        if plain:
            legacy = [
                "count" if a.value is None else f"{a.op}:{cast(Col, a.value).name}" for a in aggs
            ]
            out = _ext.DataFrame.group_by(native, *self._keys, *legacy)
            engine_names = [
                "count" if a.value is None else f"{a.op}_{cast(Col, a.value).name}" for a in aggs
            ]
            if engine_names != [a.out for a in aggs]:
                out = DataFrame(
                    {
                        **{k: out[k] for k in self._keys},
                        **{a.out: out[name] for a, name in zip(aggs, engine_names)},
                    }
                )
        else:
            names = list(native.column_names)
            out = _ext.DataFrame._group_agg_expr(native, self._keys, [a._spec(names) for a in aggs])
        if self._single is not None and any(isinstance(sp, (list, tuple)) for sp in specs):
            prefix = f"{self._single}_"
            out = out.rename({c: c[len(prefix) :] for c in out.columns if c.startswith(prefix)})
        return self._sorted(out)

    def reduce(self, agg: str) -> DataFrame:
        """One engine aggregate by name over every eligible non-key column,
        each under its own name (the ``.sum()`` family's engine)."""
        native = self._batch
        if not self._keys:
            return _ext.DataFrame.reduce(native, agg)
        return self._sorted(
            _ext.DataFrame.group_by(
                native, *self._keys, *_ext.DataFrame._reduce_specs(native, agg, self._keys)
            )
        )

    # The pandas ``groupby(...).sum()`` family.
    def sum(self) -> DataFrame:
        return self.reduce("sum")

    def mean(self) -> DataFrame:
        return self.reduce("mean")

    def min(self) -> DataFrame:
        return self.reduce("min")

    def max(self) -> DataFrame:
        return self.reduce("max")

    def count(self) -> DataFrame:
        return self.reduce("count_valid")

    def var(self) -> DataFrame:
        return self.reduce("var")

    def std(self) -> DataFrame:
        return self.reduce("std")

    def skew(self) -> DataFrame:
        """The pandas unbiased skewness per group and column (NaN below three
        values and for a constant column, as pandas 3)."""
        return self.reduce("skew_unbiased")

    def kurt(self) -> DataFrame:
        """The pandas unbiased excess kurtosis per group and column (NaN below
        four values and for a constant column, as pandas 3)."""
        return self.reduce("kurt_unbiased")

    def first(self) -> DataFrame:
        return self.reduce("first")

    def last(self) -> DataFrame:
        return self.reduce("last")

    def size(self) -> DataFrame | Series:
        """The row count per group, as one Int64 column ``size``. A pandas
        ``groupby`` that keeps the keys as the index (``as_index=True``) gives
        the counts as an unnamed Series under that index, so ``.unstack()`` reads
        it; the Polars ``group_by`` and ``as_index=False`` give the frame."""
        out = self._size_frame()
        if self._as_index and self._keys:
            return out["size"].rename(None)
        return out

    def _size_frame(self) -> DataFrame:
        return self.agg(Agg("count", None, "size"))

    # The group-wise transforms (cumsum, shift, rank, head, ...): the engine's
    # GroupBy::transform (dftu_dataframe_group_transform), one value per input
    # row in input order; the lazy group-by builds the same plan.
    def _transform(
        self,
        kind: str,
        n: int = 0,
        method: str = "average",
        ascending: bool = True,
        fill: int | float | str | bool | None = None,
    ) -> DataFrame:
        spread = kind not in ("head", "tail", "nth") and not kind.startswith("rolling_")
        if fill is not None:
            self._check_fill(fill)
        if spread and self._all_dropped():
            names = (
                [kind]
                if kind in ("cumcount", "ngroup")
                else [c for c in self._batch._names() if c not in self._keys]
            )
            return self._labelled(self._missing_frame(names, fill), None, True, fill)
        # The engine's row positions ride along as a column: the rows that
        # survive (head, tail, nth) get their own index labels back, and a
        # shift's vacated rows are the ones where the position is null.
        pos = "__dftu_pos__"
        picks = kind in ("head", "tail", "nth")
        carry = self._origin is not None or fill is not None or picks
        out = _ext.DataFrame._group_transform(
            _ext.DataFrame.with_row_index(self._batch, pos) if carry else self._batch,
            self._keys,
            kind,
            int(n),
            method,
            bool(ascending),
        )
        positions = self._col(out, pos) if picks else None
        if fill is not None:
            kept = self._col(out, pos).is_null().logical_not()
            for c in out._names():
                if c != pos:
                    out = out.with_column(c, self._filled(self._col(out, c), kept, fill))
        if carry:
            out = _ext.DataFrame.select(out, *[c for c in out._names() if c != pos])
        out = self._retype(out, kind, fill is not None)
        if self._origin is None and not picks:
            return self._spread(out, fill) if spread else out
        return self._labelled(out, positions, spread, fill)

    @staticmethod
    def _filled(column: Series, keep: Series, fill: int | float | str | bool) -> Series:
        """``column`` where ``keep`` holds, else ``fill``."""
        if isinstance(fill, (str, bool)):
            return column.where(keep, Series.from_list([fill] * len(column)))
        return column.where(keep, fill)

    def _check_fill(self, fill: int | float | str | bool) -> None:
        """``TypeError`` for a fill value the column cannot hold: text takes
        text, a bool column a bool, a number column a number (an integer column
        with nulls an integer). pandas gives an object column for the other
        pairs; this engine has no such column."""
        for name in self._batch._names():
            if name in self._keys:
                continue
            column = self._col(self._batch, name)
            kind = column.dtype
            if kind in (DType.STRING, DType.LARGE_STRING):
                fits = isinstance(fill, str)
            elif kind == DType.BOOL:
                fits = isinstance(fill, bool)
            elif kind.is_numeric:
                fits = isinstance(fill, (int, float)) and not isinstance(fill, bool)
                if fits and kind.is_integer and column.null_count > 0:
                    fits = isinstance(fill, int)
            else:
                fits = False
            if not fits:
                raise TypeError(
                    f"shift: fill_value {fill!r} does not fit the {kind.name.lower()} "
                    f"column {name!r}"
                )

    def _retype(self, out: DataFrame, kind: str, filled: bool) -> DataFrame:
        """The dtype pandas gives a transform of an integer column: shift,
        diff and the rolling windows are float64 (a shift with a fill value
        keeps the integers). The running product of an integer column is
        float64, where pandas keeps int64 (and wraps past its range); it is
        exact up to 2**53."""
        floats = (kind in ("shift", "diff") and not filled) or kind.startswith("rolling_")
        if not floats:
            return out
        ints = [n for n in out._names() if self._col(out, n).dtype.is_integer]
        for name in ints:
            if name in self._keys or name not in self._batch:
                continue
            # A column with nulls is pandas' nullable Int64, which a shift
            # and a diff keep; a plain integer column becomes float64.
            if kind in ("shift", "diff") and self._col(self._batch, name).null_count > 0:
                continue
            out = out.with_column(name, self._col(out, name).astype("float64"))
        return out

    def shift(
        self, periods: int = 1, fill_value: int | float | str | bool | None = None
    ) -> DataFrame:
        """Each column shifted by ``periods`` rows within its group (a negative
        ``periods`` looks ahead); the vacated rows are null, or ``fill_value``
        (which keeps an integer column integer). A fill the column cannot hold
        is a ``TypeError``: text takes text, a bool column a bool, a number
        column a number. pandas gives an object column for the other pairs."""
        return self._transform("shift", periods, fill=fill_value)

    def _spread(self, out: DataFrame, fill: int | float | str | bool | None = None) -> DataFrame:
        """``out``, one row per row the pandas ``groupby`` kept, on every row
        of the grouped frame: a row whose key was dropped (a null or NaN key
        with ``dropna``) is null, as pandas gives NaN there."""
        rows, total = self._rows, self._total
        if (
            rows is None
            or total is None
            or len(rows) == total
            or len(out) == total
            or not pandas_named(self)
        ):
            return out
        slots = DataFrame({"__dftu_at__": row_positions(total)})
        kept = DataFrame({"__dftu_at__": rows, "__dftu_to__": row_positions(len(rows))})
        joined = slots.join(kept, on="__dftu_at__", how="left").sort_by("__dftu_at__")
        to = self._col(joined, "__dftu_to__")
        out = out.gather(to.fill_null(-1))
        if fill is not None:
            present = to.is_null().logical_not()
            for name in out._names():
                out = out.with_column(name, self._filled(self._col(out, name), present, fill))
            return out
        # An integer result with missing rows is float64 with NaN, as pandas.
        for name in out._names():
            nullable = name in self._batch and self._col(self._batch, name).null_count > 0
            if self._col(out, name).dtype.is_integer and not nullable:
                out = out.with_column(name, self._col(out, name).astype("float64"))
        return out

    def _all_dropped(self) -> bool:
        """Whether the pandas ``groupby`` dropped every row (every key null or
        NaN): its transforms are then all missing, on every row."""
        return pandas_named(self) and bool(self._total) and len(self._batch) == 0

    def _missing_frame(
        self, names: Sequence[str], fill: int | float | str | bool | None = None
    ) -> DataFrame:
        """A float64 column per name, missing on every row of the grouped frame
        (``fill`` on every row, when given)."""
        total = self._total or 0
        if fill is not None:
            return DataFrame({n: Series.from_list([fill] * total) for n in names})
        return DataFrame({n: Series.from_list([None] * total).astype("float64") for n in names})

    def _labelled(
        self,
        out: DataFrame,
        positions: Series | Sequence[int] | None,
        spread: bool = False,
        fill: int | float | str | bool | None = None,
    ) -> DataFrame:
        """``out`` with the index the grouped frame had: its index columns,
        taken at ``positions`` (rows of the grouped frame, or all its rows in
        order when ``None``), come back as the index, as pandas keeps the
        labels of the rows a group method returns."""
        origin = self._origin
        rows_all = self._rows
        if spread:
            out = self._spread(out, fill)
            positions = None
            if self._total is not None:
                rows_all = None
        rows: Series | Sequence[int] | None = positions
        if rows_all is not None:
            rows = rows_all if positions is None else rows_all.take(positions)
        names = _ext.DataFrame.index_names(origin) if origin is not None else []
        if not names:
            # A frame with no index labels its rows by position: a subset of
            # the rows keeps the positions it had.
            if rows is None or not pandas_named(self):
                return out
            level = rows if isinstance(rows, Series) else Series.from_list(list(rows))
            out = _ext.DataFrame.with_column(out, default_name(0), level)
            return _ext.DataFrame.with_index(out, [default_name(0)])
        assert origin is not None
        for name in names:
            column = _ext.DataFrame.__getitem__(origin, name)
            out = _ext.DataFrame.with_column(
                out, name, column if rows is None else column.take(rows)
            )
        return _ext.DataFrame.with_index(out, names)

    def _labelled_series(self, out: Series) -> Series:
        """``out``, one value per row of the grouped frame in input order, under
        the labels that frame had (the Polars ``group_by`` adds no level of
        keys)."""
        origin = self._origin
        names = _ext.DataFrame.index_names(origin) if origin is not None else []
        if origin is None or not names:
            return out
        rows = self._rows
        levels = []
        for name in names:
            column = _ext.DataFrame.__getitem__(origin, name)
            levels.append(column if rows is None else column.gather(rows))
        if len(levels[0]) != len(out):
            return out
        return with_levels(out, names, levels)

    def median(self) -> DataFrame:
        """The exact median of each numeric column per group (pandas
        ``GroupBy.median``): the plan of :func:`lazyframe.group_quantile`
        over this frame, collected."""
        return self.quantile(0.5)

    def quantile(self, q: float = 0.5, interpolation: str = "linear") -> DataFrame:
        """The exact ``q`` quantile of each numeric column per group; the
        interpolation (``linear``, ``nearest``, ``lower``, ``higher`` or
        ``midpoint``) is linear by default (pandas ``GroupBy.quantile``)."""
        frame = self._frame()
        columns = self._value_columns()
        if frame.num_rows == 0:
            check_quantile(q, interpolation)
            picked = frame[self._keys + columns]
            return picked.astype({c: "float64" for c in columns})
        out = group_quantile(frame.lazy(), self._keys, columns, float(q), interpolation).collect()
        return self._sorted(out)

    def ffill(self) -> DataFrame:
        """Each non-key column's nulls filled from the nearest present value
        before them within the group (pandas ``GroupBy.ffill``); ``bfill``
        from after."""
        return self._transform("ffill")

    def bfill(self) -> DataFrame:
        return self._transform("bfill")

    def rolling(self, window: int) -> GroupRolling:
        """A trailing window of ``window`` rows within each group (pandas
        ``GroupBy.rolling``): ``.sum() .mean() .min() .max()`` are one engine
        transform each, null until the window holds ``window`` present values;
        ``.var() .std() .median() .quantile(q)`` run the Series rolling kernel
        over the group-sorted frame. One value per input row, in input
        order."""
        return GroupRolling(self, int(window))

    def expanding(self) -> _GroupSeriesWindow:
        """The expanding window within each group (pandas
        ``GroupBy.expanding``): ``.sum() .mean() .min() .max() .count() .var()
        .std()``, the Series kernel per group, one value per input row in
        input order."""
        return _GroupSeriesWindow(self, lambda s: s.expanding())

    def ewm(
        self,
        alpha: float | None = None,
        *,
        span: float | None = None,
        com: float | None = None,
        halflife: float | None = None,
    ) -> _GroupSeriesWindow:
        """The exponentially weighted window within each group (pandas
        ``GroupBy.ewm``): ``.mean() .std()``, the Series kernel per group."""
        a = _ext._ewm_alpha(alpha, span=span, com=com, halflife=halflife)
        return _GroupSeriesWindow(self, lambda s: s.ewm(a))

    def corr(self) -> DataFrame:
        """Per group, the Pearson correlation of every numeric column pair
        (pandas ``GroupBy.corr``, long form): the keys, ``a``, ``b`` and
        ``value``, one ``corr`` aggregate per pair."""
        return self._pairwise("corr")

    def _one_column(self, what: str) -> str:
        if self._single is None:
            raise TypeError(f"{what}: select one column, groupby(keys)['column'].{what}(...)")
        return self._single

    def nlargest(self, n: int = 5, keep: str = "first") -> Series:
        """Per group, the ``n`` largest present values of the selected column
        (pandas ``SeriesGroupBy.nlargest``): a Series named like the column,
        each group in key order and descending, under the labels the pandas
        group-by gives: the group keys, then the label the row had (its index,
        or its position when the frame has none); ``as_index=False`` leaves the
        keys out. A null or NaN ranks last, so it only appears in a group with
        fewer than ``n`` numbers. A tie keeps the earlier row first; ``keep``
        takes only ``"first"``, because pandas orders ``keep="last"`` ties by a
        rule that depends on the group size."""
        return self._top(n, keep, True)

    def nsmallest(self, n: int = 5, keep: str = "first") -> Series:
        """As :meth:`nlargest` for the smallest values, ascending."""
        return self._top(n, keep, False)

    def _top(self, n: int, keep: str, largest: bool) -> Series:
        name = "nlargest" if largest else "nsmallest"
        col = self._one_column(name)
        if keep != "first":
            raise NotImplementedError(f"{name}: keep={keep!r}; only 'first'")
        return top(self, col, n, largest)

    def unique(self) -> DataFrame:
        """Per group, the distinct values of the selected column in first-seen
        order (pandas ``SeriesGroupBy.unique``): the keys, in key order, and a
        List column of the column's own type. A null and a NaN each appear once,
        as pandas."""
        col = self._one_column("unique")
        frame = self._frame().select(*self._keys, col)
        distinct = frame.unique([*self._keys, col])
        ordered = distinct.sort_by_multi(self._keys, nulls_last=True) if self._keys else distinct
        counts = GroupBy(ordered, self._keys).agg(Agg("count", None, "__dftu_n__"))
        counts = counts.sort_by_multi(self._keys, nulls_last=True) if self._keys else counts
        sizes = self._col(counts, "__dftu_n__")
        lists = _ext._series_list_of(self._col(ordered, col), cast("Series[int]", sizes.cumsum()))
        return counts.select(*self._keys).with_column(col, lists)

    @property
    def is_monotonic_increasing(self) -> DataFrame:
        """Per group, whether the selected column never decreases (pandas
        ``SeriesGroupBy.is_monotonic_increasing``): the keys and a Bool column
        in key order. A null or NaN in a group makes it False, equal neighbours
        are allowed, and a one-row group is True."""
        return self._monotonic(True)

    @property
    def is_monotonic_decreasing(self) -> DataFrame:
        """As :attr:`is_monotonic_increasing` for a column that never increases."""
        return self._monotonic(False)

    def _monotonic(self, increasing: bool) -> DataFrame:
        what = "is_monotonic_increasing" if increasing else "is_monotonic_decreasing"
        col = self._one_column(what)
        frame = self._frame()
        value = self._col(frame, col)
        if value.dtype in (DType.FLOAT32, DType.FLOAT64):
            value = value.where(value.is_nan().logical_not())
        helper = "__dftu_mono_v__"
        framed = frame.select(*self._keys).with_column(helper, value)
        previous = self._col(GroupBy(framed, self._keys).shift(1), helper)
        step = value.compare_series(previous, _CMP_LT if increasing else _CMP_GT)
        framed = framed.with_column("__dftu_mono_bad__", step.astype("int64"))
        out = GroupBy(framed, self._keys).agg(
            Agg("count", None, "__dftu_rows__"),
            Agg("count_valid", Col(helper), "__dftu_present__"),
            Agg("max", Col("__dftu_mono_bad__"), "__dftu_bad__"),
        )
        rows, present, bad = (self._col(out, f"__dftu_{p}__") for p in ("rows", "present", "bad"))
        ok = (rows - present).eq(0).logical(0, bad.fill_null(0).eq(0))
        result = out.select(*self._keys).with_column(col, ok)
        return result.sort_by_multi(self._keys) if self._keys else result

    def corrwith(
        self,
        other: DataFrame | Series,
        drop: bool = False,
        method: str = "pearson",
        numeric_only: bool = False,
    ) -> DataFrame:
        """Per group, the Pearson correlation of each numeric column with the
        column of ``other`` of the same name, or with ``other`` when it is a
        Series (pandas ``GroupBy.corrwith``; wide, the keys then one Float64
        column per column). Rows pair by position and ``other`` must have as many
        rows as this frame, where pandas pairs by row label. A column ``other``
        lacks is a NaN column, or is dropped with ``drop=True``; a group with
        fewer than two pairs or a constant side is NaN. It is one ``corr``
        aggregate per column, so it streams and spills like any group-by."""
        if method != "pearson":
            raise NotImplementedError(f"corrwith: method {method!r}; only 'pearson'")
        frame = self._frame()
        if len(other) != len(frame):
            raise ValueError(
                f"corrwith: other has {len(other)} rows and the frame {len(frame)}; "
                "rows pair by position"
            )
        value_cols = self._value_columns()
        names = [c for c in frame._names() if c not in self._keys]
        if numeric_only:
            names = [c for c in names if c in value_cols]
        if isinstance(other, Series):
            pairs = {c: other for c in value_cols}
        else:
            have = set(other.columns)
            pairs = {c: self._col(other, c) for c in value_cols if c in have}
        if not pairs:
            raise ValueError("corrwith: no column in common with other")
        # The engine's corr is 0 for a group with fewer than two pairs or a
        # constant side; pandas gives NaN, so the pair count and both variances
        # (over the rows where both sides are present) come along.
        specs = []
        for c, s in pairs.items():
            y_name, xm, ym = f"__dftu_with_{c}__", f"__dftu_x_{c}__", f"__dftu_y_{c}__"
            frame = frame.with_column(y_name, s)
            x = self._col(frame, c).astype("float64")
            y = self._col(frame, y_name).astype("float64")
            frame = frame.with_column(xm, x + (y - y)).with_column(ym, y + (x - x))
            specs += [
                Agg("corr", Col(xm), c, by=Col(ym)),
                Agg("var", Col(xm), f"__dftu_vx_{c}__"),
                Agg("var", Col(ym), f"__dftu_vy_{c}__"),
                Agg("count_valid", Col(xm), f"__dftu_n_{c}__"),
            ]
        out = self._sorted(GroupBy(frame, self._keys).agg(*specs))
        nan = Series.from_list([float("nan")] * len(out))
        for c in pairs:
            n, vx, vy = (self._col(out, f"__dftu_{p}_{c}__") for p in ("n", "vx", "vy"))
            ok = n.ge(2).logical(0, vx.gt(0)).logical(0, vy.gt(0))
            out = out.with_column(c, self._col(out, c).where(ok, nan))
        out = out.select(*self._keys, *pairs)
        if not drop:
            rows = len(out)
            for c in names:
                if c not in pairs:
                    out = out.with_column(c, Series.from_list([float("nan")] * rows))
            out = out.select(*self._keys, *names)
        return out

    def cov(self) -> DataFrame:
        """Per group, the sample covariance of every numeric column pair
        (pandas ``GroupBy.cov``, long form as :meth:`corr`)."""
        return self._pairwise("covar_samp")

    def _pairwise(self, agg: str) -> DataFrame:
        cols = self._value_columns()
        specs = [Agg(agg, Col(b), f"__dftu_{a}__{b}__", by=Col(a)) for a in cols for b in cols]
        wide = self.agg(*specs)
        parts: list[DataFrame] = []
        for a in cols:
            for b in cols:
                one = wide.select(*self._keys, f"__dftu_{a}__{b}__").rename(
                    {f"__dftu_{a}__{b}__": "value"}
                )
                n = len(one)
                one = one.with_columns(a=Series.from_list([a] * n), b=Series.from_list([b] * n))
                parts.append(one.select(*self._keys, "a", "b", "value"))
        out = parts[0].concat(*parts[1:]) if len(parts) > 1 else parts[0]
        return out.sort_by_multi(self._keys + ["a", "b"]) if self._keys else out

    def take(self, indices: Sequence[int]) -> DataFrame:
        """The rows at ``indices`` within each group (0-based; negative from
        the end), without the key columns, under the group keys (with
        ``as_index``) and the labels the rows had (pandas ``GroupBy.take``: group
        order, then the order of ``indices``; ``IndexError`` when a group is too
        small). The Polars ``group_by`` gives the rows in input order, key columns
        included, under the labels they had."""
        return labelled_take(self, indices) if pandas_named(self) else self._take_rows(indices)

    def _take_rows(self, indices: Sequence[int]) -> DataFrame:
        frame = self._frame()
        row = "__dftu_group_row__"
        indexed = frame.with_row_index(row)
        picked = [GroupBy(indexed, self._keys).nth(int(i)) for i in indices]
        if not picked:
            return frame.head(0)
        out = picked[0].concat(*picked[1:]) if len(picked) > 1 else picked[0]
        out = out.unique(row).sort_by(row)
        return self._labelled(out.select(*frame._names()), self._col(out, row))

    def sample(self, n: int, seed: int = 0, *, random_state: int | None = None) -> DataFrame:
        """A deterministic sample of ``n`` rows per group (fewer for a smaller
        group), in input order: the rows whose salted row hash ranks lowest
        within the group (pandas ``GroupBy.sample``; ``random_state`` is the
        pandas spelling of ``seed``)."""
        if random_state is not None:
            seed = random_state
        frame = self._frame()
        row, key = "__dftu_group_row__", "__dftu_hash__"
        indexed = frame.with_row_index(row)
        hashed = indexed.with_column(key, (Col(row) + int(seed)).mix64().apply(indexed))
        ordered = hashed.sort_by_multi(self._keys + [key])
        picked = GroupBy(ordered, self._keys).head(int(n))
        picked = picked.sort_by(row)
        return self._labelled(picked.select(*frame._names()), self._col(picked, row))

    def resample(self, rule: int | str, on: str | None = None, unit: str = "us") -> GroupResample:
        """Tumbling time windows of ``rule`` within each group (pandas
        ``GroupBy.resample``): the time column (the index column or ``on``)
        floored to its bucket joins the keys, and the aggregate family
        (``.sum()``, ``.agg(...)``, ``.size()``, ...) is one ``group_by`` over
        keys + bucket, rows by key then time."""
        frame = self._frame()
        time = on if on is not None else single_index(frame.index_names(), "resample")
        if time not in frame:
            raise KeyError(f"resample: no column named {time!r}")
        if time in self._keys:
            raise ValueError(f"resample: {time!r} is a group key")
        series = self._col(frame, time)
        dt = series.dt if unit == "us" else series.dt(unit)
        bucket = dt.floor(rule_to_units(rule, dt._unit))
        grouped = GroupBy(frame.with_column(time, bucket), self._keys + [time])
        return GroupResample(grouped, self._keys + [time])

    # -- the rest of the pandas GroupBy surface, as compositions of group_by --
    def _frame(self) -> DataFrame:
        return self._batch

    def _value_columns(self, agg: str = "sum") -> list[str]:
        return [s.split(":")[1] for s in reduce_specs(self._batch, agg, self._keys)]

    @staticmethod
    def _col(frame: DataFrame, name: str) -> Series:
        return _ext.DataFrame.__getitem__(frame, name)

    @property
    def ngroups(self) -> int:
        return len(self._size_frame())

    def get_group(self, key: GroupKey) -> DataFrame:
        """The rows of one group, by its key value (a tuple for several
        keys), in input order."""
        if not self._keys:
            raise ValueError("get_group: the group-by has no keys")
        rows = self._group_rows(key)
        if rows is None:
            out = self._frame().set_index(self._keys).loc[[key], :]
            assert isinstance(out, DataFrame)
            return out.reset_index()
        return self._labelled(self._frame().gather(rows), rows)

    def _group_rows(self, key: GroupKey) -> list[int] | None:
        """The rows (of the kept frame) of the group named ``key``; a missing
        key (null or NaN) names the missing group. ``None`` when no group has
        that key."""

        def missing(x: object) -> bool:
            return x is None or (isinstance(x, float) and x != x)

        if isinstance(key, tuple) and len(self._keys) == 1 and len(key) == 1 and missing(key[0]):
            raise KeyError(key)
        parts = key if isinstance(key, tuple) else (key,)
        for label, rows in self._groups_in_order():
            have = label if isinstance(label, tuple) else (label,)
            if len(have) == len(parts) and all(
                (missing(a) and missing(b)) or a == b for a, b in zip(have, parts)
            ):
                return rows
        return None

    @property
    def groups(self) -> dict[GroupKey, list[int]]:
        """``{key: [row positions]}`` (``indices`` likewise)."""
        frame = self._frame()
        if not self._keys:
            return {(): list(range(len(frame)))}
        if len(frame) == 0:
            return {}
        row = "_row"
        while row in frame._names():
            row = "_" + row
        marked = (
            frame.with_row_index(row) if self._rows is None else frame.with_column(row, self._rows)
        )
        order = cast("List[int]", marked._sort_values(self._keys)[row].to_list())
        sizes = self._size_frame().sort_by_multi(self._keys)
        labels = [
            sizes[k].astype(DType.BOOL).to_list()
            if frame[k].dtype == DType.BOOL
            else sizes[k].to_list()
            for k in self._keys
        ]
        counts = cast("List[int]", sizes["size"].to_list())
        out: dict[GroupKey, list[int]] = {}
        start = 0
        for g, n in enumerate(counts):
            label = labels[0][g] if len(labels) == 1 else tuple(c[g] for c in labels)
            out.setdefault(label, []).extend(order[start : start + n])
            start += n
        return dict(sorted(((k, sorted(v)) for k, v in out.items()), key=lambda kv: kv[1][0]))

    indices = groups

    def pipe(self, func: Callable[..., _T], *args: object, **kwargs: object) -> _T:
        return func(self, *args, **kwargs)

    def nunique(self) -> DataFrame:
        """Distinct present values per group and non-key column, exact: a
        group-by over the keys plus the column, then a count per key."""
        frame = self._frame()
        out: DataFrame | None = None
        for c in [n for n in frame._names() if n not in self._keys]:
            present = frame.filter(self._col(frame, c).notna())
            pairs = GroupBy(present, self._keys + [c])._size_frame()
            counts = GroupBy(pairs, self._keys).agg(Agg("count", None, c))
            every = GroupBy(frame, self._keys)._size_frame().select(*self._keys)
            if len(counts) < len(every):
                # A group whose values are all missing has no pair: zero values.
                counts = every.join(counts, on=self._keys, how="left", nulls_equal=True)
                counts = counts.with_column(c, self._col(counts, c).fillna(0))
            out = counts if out is None else out.join(counts, on=self._keys)
        if out is None:
            raise ValueError("nunique: no columns besides the keys")
        return self._sorted(out)

    def sem(self) -> DataFrame:
        """Standard error of the mean per group: ``std / sqrt(count)``."""
        cols = self._value_columns("std")
        specs = [Agg("std", Col(c), c) for c in cols]
        specs += [Agg("count_valid", Col(c), f"__dftu_n_{c}__") for c in cols]
        out = self.agg(*specs)
        for c in cols:
            n = self._col(out, f"__dftu_n_{c}__").astype("float64").sqrt()
            out = out.with_column(c, self._col(out, c) / n)
        return out.select(*(self._keys + cols))

    def any(self) -> DataFrame:
        """Per group and numeric column: whether any value is non-zero
        (``all`` likewise)."""
        return self._truth("max")

    def all(self) -> DataFrame:
        return self._truth("min")

    def _truth(self, agg: str) -> DataFrame:
        frame = self._frame()
        cols = self._value_columns("sum")
        for c in cols:
            nonzero = self._col(frame, c).astype("float64").ne(0).astype("int64")
            frame = frame.with_column(c, nonzero)
        out = GroupBy(frame, self._keys).reduce(agg)
        for c in cols:
            out = out.with_column(c, self._col(out, c).ne(0))
        return self._sorted(out)

    def idxmax(self) -> DataFrame:
        """Per group and numeric column: the row position of the maximum
        (``idxmin`` likewise); the index label when one index column is set."""
        return self._arg("argmax")

    def idxmin(self) -> DataFrame:
        return self._arg("argmin")

    def _arg(self, op: str) -> DataFrame:
        frame = self._frame()
        label = "__dftu_pos__"
        index = frame.index_names()
        if len(index) == 1:
            label = index[0]
        else:
            frame = frame.with_row_index(label)
        cols = self._value_columns("sum")
        specs = [Agg(op, Col(label), c, by=Col(c)) for c in cols]
        out = GroupBy(frame, self._keys).agg(*specs)
        # The arg aggregates report the label's repr (a string); a numeric
        # label is parsed back to its type.
        dtype = self._col(frame, label).dtype
        if dtype != DType.STRING:
            for c in cols:
                out = out.with_column(c, self._col(out, c).astype(dtype))
        return self._sorted(out)

    def ohlc(self) -> DataFrame:
        """Per group and numeric column: ``<c>_open`` / ``_high`` / ``_low`` /
        ``_close`` (first, max, min, last)."""
        specs = []
        for c in self._value_columns("sum"):
            specs += [
                Agg("first", Col(c), f"{c}_open"),
                Agg("max", Col(c), f"{c}_high"),
                Agg("min", Col(c), f"{c}_low"),
                Agg("last", Col(c), f"{c}_close"),
            ]
        return self.agg(*specs)

    def describe(self) -> DataFrame:
        """Per group and numeric column: ``<c>_count`` / ``_mean`` / ``_std``
        / ``_min`` / ``_25%`` / ``_50%`` / ``_75%`` / ``_max`` (the quantiles
        exact, through :meth:`quantile`)."""
        specs = []
        cols = self._value_columns("sum")
        for c in cols:
            for name in ("count_valid", "mean", "std", "min", "max"):
                label = f"{c}_{'count' if name == 'count_valid' else name}"
                specs.append(Agg(name, Col(c), label))
        out = self.agg(*specs)
        for q in (0.25, 0.5, 0.75):
            quant = self.quantile(q)
            names = {c: f"{c}_{int(q * 100)}%" for c in cols}
            if self._keys:
                out = out.join(quant.rename(names), on=self._keys)
            else:
                out = out.with_columns(**{names[c]: self._col(quant, c) for c in cols})
        order = [*self._keys]
        for c in cols:
            order += [
                f"{c}_{s}" for s in ("count", "mean", "std", "min", "25%", "50%", "75%", "max")
            ]
        return self._sorted(out.select(*order))

    def value_counts(self, ascending: bool = False) -> DataFrame | Series:
        """Distinct rows of the non-key columns within each group with their
        count, most frequent first. The pandas ``groupby`` gives the counts as a
        Series named ``count`` under the keys and the values; ``group_by`` and
        ``as_index=False`` give a frame with the keys and values as columns."""
        frame = self._frame()
        cols = [c for c in frame._names() if c not in self._keys]
        for c in cols:  # pandas drops a row with a null value (dropna=True)
            frame = frame.filter(self._col(frame, c).notna())
            if pandas_named(self) and self._col(frame, c).dtype.is_float:
                frame = frame.filter(self._col(frame, c).is_nan().logical_not())
        if self._sort or not pandas_named(self):
            out = GroupBy(frame, self._keys + cols)._size_frame().rename({"size": "count"})
            out = out._sort_values(
                self._keys + ["count"], ascending=[True] * len(self._keys) + [ascending]
            )
        else:
            # Pandas orders the combinations by count, ties in order of first appearance.
            row, first = "__dftu_row__", "__dftu_first__"
            indexed = frame.with_row_index(row)
            out = GroupBy(indexed, self._keys + cols).agg(
                Agg("count", None, "count"), Agg("min", Col(row), first)
            )
            out = out._sort_values(["count", first], ascending=[ascending, True])
            out = out.select(*self._keys, *cols, "count")
        if self._as_index is True:
            return out.set_index(self._keys + cols)["count"]
        return out

    def transform(self, func: str) -> DataFrame:
        """A group aggregate (``"mean"``, ``"sum"``, ...) broadcast back to
        every row of its group, one column per numeric non-key column, rows
        in input order."""
        frame = self._frame()
        agg = PANDAS_AGG_NAMES.get(func, func)
        cols = self._value_columns(agg)
        if self._all_dropped():
            return self._labelled(self._missing_frame(cols), None, True)
        row = "__dftu_row__"
        indexed = frame.with_row_index(row)
        specs = [Agg(agg, Col(c), f"__dftu_t_{c}__") for c in cols]
        grouped = GroupBy(indexed, self._keys).agg(*specs)
        if not self._keys:
            out = indexed
            for c in cols:
                v = self._col(grouped, f"__dftu_t_{c}__")[0]
                assert isinstance(v, (int, float))
                broadcast = self._col(frame, c).full_like(v)
                out = out.with_column(c, broadcast)
            return self._labelled(out.select(*cols), None)
        joined = indexed.join(grouped, on=self._keys, nulls_equal=True)._sort_values(row)
        for c in cols:
            joined = joined.with_column(c, self._col(joined, f"__dftu_t_{c}__"))
        return self._labelled(joined.select(*cols), self._col(joined, row), spread=True)

    # -- user functions: traced into the engine, else run per group -----------
    def apply(self, func: Callable[[DataFrame], object]) -> DataFrame | Series:
        """``func`` over each group (pandas ``GroupBy.apply``). It is first
        run once on a symbolic group whose columns are expressions, so a body
        like ``lambda g: g["v"].sum() / g["n"].sum()`` lowers to one group-by
        (a frame of the keys and ``value``), and ``lambda g: g["v"] -
        g["v"].mean()`` to a group-by joined back on the rows (a Series in
        input order). A body the trace cannot follow runs in Python per group
        with a warning: a scalar result per group gives the keys + ``value``
        frame, a Series or frame result the concatenation."""
        traced = trace_group(func, self._group_columns())
        if traced is not None:
            return self._lower_apply(traced)
        return self._apply_python(func)

    def _group_columns(self) -> list[str]:
        """The columns a group function sees: all of them, but for the pandas
        ``groupby`` the key columns are left out, as pandas 3 does (pandas 2
        passes them with a warning)."""
        names = self._frame()._names()
        return [c for c in names if c not in self._keys] if pandas_named(self) else names

    def filter(self, func: Callable[[DataFrame], object]) -> DataFrame:
        """The rows of the groups for which ``func`` holds (pandas
        ``GroupBy.filter``): ``lambda g: g["v"].sum() > 10`` lowers to one
        group-by and a join; a body the trace cannot follow runs in Python per
        group with a warning. Rows come back in input order."""
        traced = trace_group(func, self._frame()._names(), "GroupBy.filter")
        if traced is not None:
            return self._lower_filter(traced)
        return self._filter_python(func)

    def _group_values(self, expr: Expr) -> tuple[DataFrame, Expr]:
        """The per-group frame holding every aggregate leaf of ``expr`` (keys
        plus ``__dftu_agg_<i>__`` columns) and the expression rewritten over
        it."""
        rewritten, aggs = split_aggs(expr)
        if not aggs:
            raise TypeError("the group function must reduce (sum, mean, ...) at least once")
        return self.agg(*aggs), rewritten

    def _lower_apply(self, traced: object) -> DataFrame | Series:
        frame = self._frame()
        if isinstance(traced, _AggSet):
            return self.agg(*traced.aggs)
        if isinstance(traced, Agg):
            return self._scalars(self.agg(traced.alias("value")))
        if not isinstance(traced, Expr):
            raise TypeError(
                f"the group function returned {type(traced).__name__}; expected an "
                "aggregate or an expression over the group"
            )
        if not has_agg_leaf(traced):
            # A row expression with no aggregate: one value per row.
            return self._rows_series(traced.apply(frame), traced)
        grouped, rewritten = self._group_values(traced)
        row_cols = [c for c in collect_columns(traced) if c in frame]
        if not row_cols:
            value = rewritten.apply(grouped)
            return self._scalars(
                grouped.with_column("value", value).select(*(self._keys + ["value"]))
            )
        # Row leaves too: the aggregates broadcast back onto the rows.
        row = "__dftu_row__"
        indexed = frame.with_row_index(row)
        joined = (
            indexed.join(grouped, on=self._keys, nulls_equal=True)._sort_values(row)
            if self._keys
            else indexed
        )
        if not self._keys:
            for name in grouped.columns:
                v = self._col(grouped, name)[0]
                assert isinstance(v, (int, float))
                joined = joined.with_column(name, self._col(frame, row_cols[0]).full_like(v))
        return self._rows_series(rewritten.apply(joined), traced)

    def _scalars(self, out: DataFrame) -> DataFrame | Series:
        """One value for each group (``out`` holds the keys and ``value``): for
        the pandas ``groupby`` a Series with no name under the keys, as pandas 3
        gives; else the frame of keys and ``value``."""
        if not (pandas_named(self) and self._as_index is True):
            return out
        if not out.index_names():
            out = out.set_index(self._keys)
        return out["value"].rename(None)

    def _rows_series(self, out: Series, expr: Expr) -> Series:
        """``out``, one value for each row of the grouped frame in input order,
        as the Series ``apply`` returns: for the pandas ``groupby`` in group
        order under the labels the rows had (and the group keys, with
        ``as_index``), named like the one column ``expr`` reads, else in input
        order under the labels."""
        if not pandas_named(self):
            return self._labelled_series(out)
        read = {c for c in collect_columns(expr) if c in self._frame()}
        return labelled_series(self, out.rename(read.pop()) if len(read) == 1 else out)

    def _lower_filter(self, traced: object) -> DataFrame:
        frame = self._frame()
        if isinstance(traced, Agg):
            traced = AggLeaf(traced) != 0
        if not isinstance(traced, Expr) or not has_agg_leaf(traced):
            raise TypeError("filter: the group function must return a condition over aggregates")
        if [c for c in collect_columns(traced) if c in frame]:
            raise TypeError("filter: the group function must reduce every column it reads")
        grouped, rewritten = self._group_values(traced)
        keep = grouped.filter(rewritten.apply(grouped))
        if not self._keys:
            return self._labelled(frame if len(keep) else frame.head(0), None if len(keep) else [])
        row = "__dftu_row__"
        indexed = frame.with_row_index(row)
        out = indexed.join(keep.select(*self._keys), on=self._keys, nulls_equal=True)._sort_values(
            row
        )
        return self._labelled(out.select(*frame._names()), self._col(out, row))

    def _groups_in_order(self) -> list[tuple[object, list[int]]]:
        groups: list[tuple[object, list[int]]] = list(self.groups.items())
        if self._rows is not None:
            # `groups` names rows of the frame the group-by was made on; the
            # callers index the frame with the rows the group-by kept.
            local = {at: i for i, at in enumerate(self._rows.to_list())}
            groups = [(k, [local[r] for r in rows]) for k, rows in groups]
        if pandas_named(self) and self._sort:
            # Key order, a null key last, as the engine sorts them.
            def order(item: tuple[object, list[int]]) -> tuple[object, ...]:
                parts = item[0] if isinstance(item[0], tuple) else (item[0],)
                return tuple((p is None, p) if p is not None else (True, 0) for p in parts)

            groups.sort(key=order)
        return groups

    def _apply_python(self, func: Callable[[DataFrame], object]) -> DataFrame | Series:
        frame = self._frame()
        by_key = pandas_named(self)
        results: list[tuple[object, object]] = []
        for label, rows in self._groups_in_order():
            group = group_frame(self, rows) if by_key else self._labelled(frame.gather(rows), rows)
            results.append((label, func(group)))
        if not results:
            return frame.head(0)
        first = results[0][1]
        if by_key and isinstance(first, (DataFrame, Series)):
            return apply_pieces(self, results)
        if isinstance(first, DataFrame):
            out = results[0][1]
            assert isinstance(out, DataFrame)
            for _, r in results[1:]:
                assert isinstance(r, DataFrame)
                out = out.concat(r)
            return out
        if isinstance(first, Series):
            # The pieces keep the labels their group frame gave them.
            pieces: list[DataFrame] = []
            for _, r in results:
                assert isinstance(r, Series)
                pieces.append(to_frame(r, "value"))
            return pieces[0].concat(*pieces[1:])["value"].rename(None)
        keys: dict[str, list[object]] = {k: [] for k in self._keys}
        for label, _ in results:
            parts = label if isinstance(label, tuple) else (label,)
            for k, v in zip(self._keys, parts):
                keys[k].append(v)
        return self._scalars(DataFrame({**keys, "value": [r for _, r in results]}))

    def _filter_python(self, func: Callable[[DataFrame], object]) -> DataFrame:
        frame = self._frame()
        keep: list[int] = []
        for _, rows in self._groups_in_order():
            if func(frame.gather(rows)):
                keep.extend(rows)
        kept = sorted(keep)
        return self._labelled(frame.gather(kept), kept)


_SERIES_FORM = (
    "sum",
    "mean",
    "min",
    "max",
    "count",
    "first",
    "last",
    "median",
    "prod",
    "sem",
    "skew",
    "kurt",
    "var",
    "std",
    "nunique",
    "any",
    "all",
    "idxmax",
    "idxmin",
    "quantile",
    "cumsum",
    "cummax",
    "cummin",
    "cumprod",
    "cumcount",
    "ngroup",
    "shift",
    "diff",
    "pct_change",
    "rank",
    "head",
    "tail",
    "nth",
    "ffill",
    "bfill",
)


class SeriesGroupBy(GroupBy):
    """``groupby(keys)["column"]``: the pandas ``SeriesGroupBy``. The
    reductions and transforms of :class:`GroupBy` give the one value column as
    a :class:`Series` (named by the column, under the group keys or the row
    labels), where ``groupby(keys)[["column"]]`` gives a frame. With
    ``as_index=False`` the keys are columns, so the result is a frame."""

    def _single_series(self, out: DataFrame) -> DataFrame | Series:
        columns = out.columns
        return _ext.DataFrame.__getitem__(out, columns[0]) if len(columns) == 1 else out

    def agg(self, *specs: AggForm, **named: AggKeyword) -> DataFrame | Series:
        """As :meth:`GroupBy.agg`; one aggregate name or function is a Series,
        a list of names, a mapping or keywords a frame."""
        out = super().agg(*specs, **named)
        if len(specs) == 1 and not named and isinstance(specs[0], str):
            return self._single_series(out)
        return out

    if TYPE_CHECKING:

        def sum(self) -> Series: ...
        def mean(self) -> Series: ...
        def min(self) -> Series: ...
        def max(self) -> Series: ...
        def count(self) -> Series: ...
        def var(self) -> Series: ...
        def std(self) -> Series: ...
        def skew(self) -> Series: ...
        def kurt(self) -> Series: ...
        def first(self) -> Series: ...
        def last(self) -> Series: ...
        def median(self) -> Series: ...
        def prod(self) -> Series: ...
        def sem(self) -> Series: ...
        def nunique(self) -> Series: ...
        def any(self) -> Series: ...
        def all(self) -> Series: ...
        def idxmax(self) -> Series: ...
        def idxmin(self) -> Series: ...
        def quantile(self, q: float = 0.5, interpolation: str = "linear") -> Series: ...
        def cumsum(self) -> Series: ...
        def cummax(self) -> Series: ...
        def cummin(self) -> Series: ...
        def cumprod(self) -> Series: ...
        def cumcount(self) -> Series: ...
        def ngroup(self) -> Series: ...
        def shift(
            self, periods: int = 1, fill_value: int | float | str | bool | None = None
        ) -> Series: ...
        def diff(self) -> Series: ...
        def pct_change(self) -> Series: ...
        def rank(self, method: str = "average", ascending: bool = True) -> Series: ...
        def head(self, n: int = 5) -> Series: ...
        def tail(self, n: int = 5) -> Series: ...
        def nth(self, n: int | Sequence[int]) -> Series: ...
        def ffill(self) -> Series: ...
        def bfill(self) -> Series: ...


def _series_form(name: str) -> Callable[..., object]:
    method = getattr(GroupBy, name)

    @functools.wraps(method)
    def call(self: SeriesGroupBy, *args: object, **kwargs: object) -> object:
        out = method(self, *args, **kwargs)
        return self._single_series(out) if isinstance(out, DataFrame) else out

    return call


for _name in _SERIES_FORM:
    setattr(SeriesGroupBy, _name, _series_form(_name))


def _windowed(grouped: object, out: DataFrame) -> DataFrame | Series:
    """A window result of the pandas ``groupby``: the rows in group order under
    the keys and the row labels (``(key, row)``), a Series for ``["column"]``.
    The Polars ``group_by`` keeps the input order and no labels."""
    if not isinstance(grouped, GroupBy) or not pandas_named(grouped):
        return out
    labelled = labelled_frame(grouped, out)
    return grouped._single_series(labelled) if isinstance(grouped, SeriesGroupBy) else labelled


class GroupRolling:
    """``GroupBy.rolling(window)``: the pandas window object over a group-by,
    eager or lazy."""

    __slots__ = ("_grouped", "_window")

    def __init__(self, grouped: GroupBy | LazyGroupBy, window: int) -> None:
        if window < 1:
            raise ValueError("rolling: the window must be at least 1")
        self._grouped = grouped
        self._window = window

    def sum(self) -> DataFrame | Series | LazyFrame:
        return self._roll("rolling_sum")

    def mean(self) -> DataFrame | Series | LazyFrame:
        return self._roll("rolling_mean")

    def min(self) -> DataFrame | Series | LazyFrame:
        return self._roll("rolling_min")

    def max(self) -> DataFrame | Series | LazyFrame:
        return self._roll("rolling_max")

    def var(self) -> DataFrame | Series:
        return self._series_kernel(lambda s: s.rolling_var(self._window))

    def std(self) -> DataFrame | Series:
        return self._series_kernel(lambda s: s.rolling_std(self._window))

    def median(self) -> DataFrame | Series:
        return self._series_kernel(lambda s: s.rolling_median(self._window))

    def quantile(self, q: float) -> DataFrame | Series:
        return self._series_kernel(lambda s: s.rolling_quantile(self._window, q))

    def _roll(self, kind: str) -> DataFrame | Series | LazyFrame:
        out = self._grouped._transform(kind, self._window)
        return _windowed(self._grouped, out) if isinstance(out, DataFrame) else out

    def _series_kernel(self, kernel: Callable[[Series], Series]) -> DataFrame | Series:
        # The Series rolling kernel over the frame sorted by group (stable, so
        # each group keeps its input order), then the rows back in input order.
        if not isinstance(self._grouped, GroupBy):
            raise TypeError("rolling: var / std / median / quantile need an eager group-by")
        g = self._grouped
        frame = g._frame()
        cols = g._value_columns()
        row = "__dftu_group_row__"
        ordered = frame.with_row_index(row).sort_by_multi(g._keys + [row])
        # The engine's rolling sum is null exactly where the window is short
        # of present values, so it masks the kernel's value.
        mask = GroupBy(ordered, g._keys)._transform("rolling_sum", self._window)
        out = ordered
        for c in cols:
            m = g._col(mask, c)
            out = out.with_column(c, kernel(g._col(ordered, c)) + (m - m))
        return _windowed(g, out.sort_by(row).select(*cols))


class _GroupSeriesWindow:
    """``GroupBy.expanding()`` / ``ewm(alpha)``: the Series window per group
    (a stateful scan, so each group runs on its own rows), the values back
    in input order."""

    __slots__ = ("_grouped", "_window")

    def __init__(self, grouped: GroupBy, window: Callable[[Series], object]) -> None:
        self._grouped = grouped
        self._window = window

    def _reduce(self, name: str) -> DataFrame | Series:
        g = self._grouped
        frame = g._frame()
        cols = g._value_columns()
        row = "__dftu_group_row__"
        parts: list[DataFrame] = []
        for _, rows in g._groups_in_order():
            part = frame.gather(rows).with_column(row, Series.from_list(rows))
            for c in cols:
                part = part.with_column(c, getattr(self._window(g._col(part, c)), name)())
            parts.append(part.select(row, *cols))
        if not parts:
            return _windowed(g, frame.select(*cols))
        out = parts[0].concat(*parts[1:]) if len(parts) > 1 else parts[0]
        out = out.sort_by(row).select(*cols)
        # An integer column's window is float64, as pandas gives it.
        for c in cols:
            if g._col(out, c).dtype.is_integer:
                out = out.with_column(c, g._col(out, c).astype("float64"))
        return _windowed(g, out)

    def sum(self) -> DataFrame | Series:
        return self._reduce("sum")

    def mean(self) -> DataFrame | Series:
        return self._reduce("mean")

    def min(self) -> DataFrame | Series:
        return self._reduce("min")

    def max(self) -> DataFrame | Series:
        return self._reduce("max")

    def count(self) -> DataFrame | Series:
        return self._reduce("count")

    def var(self) -> DataFrame | Series:
        return self._reduce("var")

    def std(self) -> DataFrame | Series:
        return self._reduce("std")


class GroupResample:
    """``GroupBy.resample(rule)``: the group-by over keys + time bucket, every
    result sorted by key then bucket and indexed by them (the pandas
    ``(key, time)`` index)."""

    __slots__ = ("_grouped", "_order")

    def __init__(self, grouped: GroupBy | LazyGroupBy, order: list[str]) -> None:
        self._grouped = grouped
        self._order = order

    def __getattr__(self, name: str) -> object:
        attr = getattr(self._grouped, name)
        if not callable(attr):
            return attr

        def call(*args: object, **kwargs: object) -> object:
            out = attr(*args, **kwargs)
            names = out._names() if isinstance(out, DataFrame) else getattr(out, "columns", None)
            if names is not None and all(c in names for c in self._order):
                out = out.sort_by_multi(self._order)
                if isinstance(out, DataFrame):
                    out = _ext.DataFrame.with_index(out, list(self._order))
                else:
                    out = out.set_index(list(self._order))
            return out

        return call


class _MissingColumn(KeyError):
    """A column the group function reads that the frame lacks: an error in
    the function, not a body the trace cannot follow."""


class _AggSet:
    """The aggregates of one group function that reduces several columns, one
    column of the result each."""

    __slots__ = ("aggs",)

    def __init__(self, aggs: list[Agg]) -> None:
        self.aggs = aggs


class _TraceColumns:
    """``group[["a", "b"]]`` on a traced group: a reduction of it is one
    aggregate for each column, named like the column."""

    __slots__ = ("_names",)

    def __init__(self, names: list[str]) -> None:
        self._names = names

    def __getattr__(self, name: str) -> Callable[[], _AggSet]:
        if name.startswith("_") or name not in _COLUMN_REDUCTIONS:
            raise AttributeError(name)

        def reduce() -> _AggSet:
            return _AggSet([getattr(Col(c), name)().alias(c) for c in self._names])

        return reduce


_COLUMN_REDUCTIONS = frozenset(
    ("sum", "mean", "min", "max", "count", "std", "var", "median", "first", "last")
)


class _GroupTrace:
    """The symbolic group a traced group function runs on: a column read is
    the column expression, so reductions become aggregates."""

    __slots__ = ("_columns",)

    def __init__(self, columns: Sequence[str]) -> None:
        self._columns = list(columns)

    def __getitem__(self, name: str | list[str]) -> Expr | _TraceColumns:
        if isinstance(name, list):
            for c in name:
                if c not in self._columns:
                    raise _MissingColumn(f"no column named {c!r}")
            return _TraceColumns(name)
        if name not in self._columns:
            raise _MissingColumn(f"no column named {name!r}")
        return Col(name)

    def __getattr__(self, name: str) -> Expr:
        if name.startswith("_") or name not in self._columns:
            raise AttributeError(name)
        return Col(name)

    @property
    def columns(self) -> list[str]:
        return list(self._columns)

    def size(self) -> Agg:
        return Agg("count", None)

    def __len__(self) -> int:
        raise TypeError("len(group) is a Python value; use group.size() for the aggregate")


def trace_group(
    func: Callable[..., object], columns: Sequence[str], what: str = "GroupBy.apply"
) -> object | None:
    """Run ``func`` on a symbolic group; the aggregate / expression it built,
    or None (with the fallback warning logged) when the body cannot be
    followed."""
    if uses_identity_test(func):
        warn_fallback(what, func, "`is` cannot be traced")
        return None
    try:
        traced = func(_GroupTrace(columns))
    except _MissingColumn:
        raise
    except trace_errors() as e:
        warn_fallback(what, func, f"{type(e).__name__}: {e}")
        return None
    if not isinstance(traced, (Agg, Expr, _AggSet)):
        warn_fallback(
            what,
            func,
            f"returned {type(traced).__name__}, not an aggregate or expression",
        )
        return None
    return traced


def group_quantile(
    plan: LazyFrame,
    keys: list[str],
    columns: list[str],
    q: float,
    interpolation: str = "linear",
) -> LazyFrame:
    """The exact ``q`` quantile of each of ``columns`` per group (pandas
    ``GroupBy.quantile``, linear interpolation by default), as one streaming
    group-by of the engine's exact ``quantile`` aggregate. The result is sorted
    by the keys, one Float64 column per input column, named as the input."""
    check_quantile(q, interpolation)
    if not columns:
        raise ValueError("quantile: no numeric column to reduce")
    out = cast("LazyGroupBy", plan.group_by(keys)).agg(
        *[Agg("quantile", Col(c), c, param=float(q), interpolation=interpolation) for c in columns]
    )
    return out.sort_by_multi(keys) if keys else out


_install_aliases("GroupBy", GroupBy)
