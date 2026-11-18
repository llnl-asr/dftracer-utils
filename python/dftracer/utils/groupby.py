# SPDX-License-Identifier: MIT
"""``GroupBy``: the group-by over a native ``DataFrame`` and the windows,
resamples and traces that run per group."""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    List,
    Optional,
    Sequence,
    Tuple,
    Union,
    cast,
)

from . import dftracer_utils_ext as _ext
from ._aliases import install as _install_aliases
from ._apply import trace_errors, uses_identity_test, warn_fallback
from .agg import (
    PANDAS_AGG_NAMES,
    agg_specs_from_pandas_forms,
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
    from .lazyframe import LazyFrame, LazyGroupBy

__all__ = ["GroupBy"]


class GroupBy:
    """A lazy group-by over a native ``DataFrame`` (zero or more key columns;
    none makes the whole frame one group). ``.agg(...)`` takes aggregate
    expressions (``col("x").sum()``, ``count()``), ``"op:column"`` strings, or
    the pandas forms (a name, a list of names, a ``{column: name}`` dict,
    ``out=("column", name)`` keywords); ``.sum()`` and its family broadcast one
    aggregate over every eligible non-key column."""

    def __init__(
        self,
        batch: "DataFrame",
        keys: "Union[str, Sequence[str], None]" = None,
    ) -> None:
        self._batch = batch
        self._keys: List[str] = (
            [] if keys is None else [keys] if isinstance(keys, str) else list(keys)
        )

    @property
    def keys(self) -> List[str]:
        return list(self._keys)

    def __getitem__(self, columns: "Union[str, Sequence[str]]") -> "GroupBy":
        """``groupby(k)["v"]`` / ``groupby(k)[["v", "w"]]``: the same groups
        over only those value columns (the pandas column selection). With
        one column, a list of aggregate names labels the outputs by name
        alone, as pandas does."""
        frame = self._batch
        picked = [columns] if isinstance(columns, str) else list(columns)
        for c in picked:
            if c not in frame:
                raise KeyError(f"groupby: no column named {c!r}")
        out = GroupBy(
            frame.select(self._keys + [c for c in picked if c not in self._keys]), self._keys
        )
        out._single = picked[0] if len(picked) == 1 else None
        out._sort = self._sort
        out._rows = self._rows
        return out

    _single: "Optional[str]" = None
    _rows: "Optional[Series]" = None
    _sort: bool = False

    def _sorted(self, out: DataFrame) -> DataFrame:
        if self._sort and self._keys:
            return out.sort_by_multi(self._keys)
        return out

    def agg(self, *specs: object, **named: object) -> DataFrame:
        frame = self._batch
        flat: "List[Union[str, Agg]]" = []
        for spec in specs:
            if isinstance(spec, Agg) or (isinstance(spec, str) and ":" in spec):
                flat.append(spec)
            else:
                flat.extend(agg_specs_from_pandas_forms(frame, self._keys, spec, {}))
        if named:
            flat.extend(agg_specs_from_pandas_forms(frame, self._keys, None, named))
        if not flat:
            raise TypeError("agg() needs at least one aggregate")
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
            legacy = ["count" if a.value is None else f"{a.op}:{a.value.name}" for a in aggs]
            out = _ext.DataFrame.group_by(native, *self._keys, *legacy)
            engine_names = ["count" if a.value is None else f"{a.op}_{a.value.name}" for a in aggs]
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

    def size(self) -> DataFrame:
        """The row count per group, as one Int64 column ``size``."""
        return self.agg(Agg("count", None, "size"))

    # The group-wise transforms (cumsum, shift, rank, head, ...): the engine's
    # GroupBy::transform (dftu_dataframe_group_transform), one value per input
    # row in input order; the lazy group-by builds the same plan.
    def _transform(
        self, kind: str, n: int = 0, method: str = "average", ascending: bool = True
    ) -> DataFrame:
        return _ext.DataFrame._group_transform(
            self._batch, self._keys, kind, int(n), method, bool(ascending)
        )

    def cumsum(self) -> DataFrame:
        return self._transform("cumsum")

    def cummax(self) -> DataFrame:
        return self._transform("cummax")

    def cummin(self) -> DataFrame:
        return self._transform("cummin")

    def cumcount(self) -> DataFrame:
        return self._transform("cumcount")

    def shift(self, periods: int = 1) -> DataFrame:
        return self._transform("shift", periods)

    def diff(self) -> DataFrame:
        return self._transform("diff")

    def pct_change(self) -> DataFrame:
        return self._transform("pct_change")

    def rank(self, method: str = "average", ascending: bool = True) -> DataFrame:
        return self._transform("rank", 0, method, ascending)

    def ngroup(self) -> DataFrame:
        return self._transform("ngroup")

    def head(self, n: int = 5) -> DataFrame:
        return self._transform("head", n)

    def tail(self, n: int = 5) -> DataFrame:
        return self._transform("tail", n)

    def nth(self, n: "Union[int, Sequence[int]]") -> DataFrame:
        """The ``n``-th row of each group (0-based; negative from the end), or
        of each position in a list, in input order."""
        if isinstance(n, int):
            return self._transform("nth", n)
        return self.take(n)

    def cumprod(self) -> DataFrame:
        """Each numeric column's running product within its group (Float64)."""
        return self._transform("cumprod")

    def prod(self) -> DataFrame:
        return self.reduce("prod")

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

    def rolling(self, window: int) -> "GroupRolling":
        """A trailing window of ``window`` rows within each group (pandas
        ``GroupBy.rolling``): ``.sum() .mean() .min() .max()`` are one engine
        transform each, null until the window holds ``window`` present values;
        ``.var() .std() .median() .quantile(q)`` run the Series rolling kernel
        over the group-sorted frame. One value per input row, in input
        order."""
        return GroupRolling(self, int(window))

    def expanding(self) -> "_GroupSeriesWindow":
        """The expanding window within each group (pandas
        ``GroupBy.expanding``): ``.sum() .mean() .min() .max() .count() .var()
        .std()``, the Series kernel per group, one value per input row in
        input order."""
        return _GroupSeriesWindow(self, lambda s: s.expanding())

    def ewm(
        self,
        alpha: Optional[float] = None,
        *,
        span: Optional[float] = None,
        com: Optional[float] = None,
        halflife: Optional[float] = None,
    ) -> "_GroupSeriesWindow":
        """The exponentially weighted window within each group (pandas
        ``GroupBy.ewm``): ``.mean() .std()``, the Series kernel per group."""
        a = _ext._ewm_alpha(alpha, span=span, com=com, halflife=halflife)
        return _GroupSeriesWindow(self, lambda s: s.ewm(a))

    def corr(self) -> DataFrame:
        """Per group, the Pearson correlation of every numeric column pair
        (pandas ``GroupBy.corr``, long form): the keys, ``a``, ``b`` and
        ``value``, one ``corr`` aggregate per pair."""
        return self._pairwise("corr")

    def cov(self) -> DataFrame:
        """Per group, the sample covariance of every numeric column pair
        (pandas ``GroupBy.cov``, long form as :meth:`corr`)."""
        return self._pairwise("covar_samp")

    def _pairwise(self, agg: str) -> DataFrame:
        cols = self._value_columns()
        specs = [Agg(agg, Col(b), f"__dftu_{a}__{b}__", by=Col(a)) for a in cols for b in cols]
        wide = self.agg(*specs)
        parts: List[DataFrame] = []
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
        the end), in input order (pandas ``GroupBy.take``)."""
        frame = self._frame()
        row = "__dftu_group_row__"
        indexed = frame.with_row_index(row)
        picked = [GroupBy(indexed, self._keys).nth(int(i)) for i in indices]
        if not picked:
            return frame.head(0)
        out = picked[0].concat(*picked[1:]) if len(picked) > 1 else picked[0]
        return out.unique(row).sort_by(row).select(*frame.columns)

    def sample(self, n: int, seed: int = 0, *, random_state: Optional[int] = None) -> DataFrame:
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
        return picked.sort_by(row).select(*frame.columns)

    def resample(
        self, rule: "Union[int, str]", on: Optional[str] = None, unit: str = "us"
    ) -> "GroupResample":
        """Tumbling time windows of ``rule`` within each group (pandas
        ``GroupBy.resample``): the time column (the index column or ``on``)
        floored to its bucket joins the keys, and the aggregate family
        (``.sum()``, ``.agg(...)``, ``.size()``, ...) is one ``group_by`` over
        keys + bucket, rows by key then time."""
        frame = self._frame()
        time = on if on is not None else single_index(frame._index, "resample")
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

    def _value_columns(self, agg: str = "sum") -> List[str]:
        return [s.split(":")[1] for s in reduce_specs(self._batch, agg, self._keys)]

    @staticmethod
    def _col(frame: DataFrame, name: str) -> Series:
        return _ext.DataFrame.__getitem__(frame, name)

    @property
    def ngroups(self) -> int:
        return len(self.size())

    def get_group(self, key: object) -> DataFrame:
        """The rows of one group, by its key value (a tuple for several
        keys), in input order."""
        if not self._keys:
            raise ValueError("get_group: the group-by has no keys")
        out = self._frame().set_index(self._keys).loc[key, :]
        assert isinstance(out, DataFrame)
        return out

    @property
    def groups(self) -> Dict[object, List[int]]:
        """``{key: [row positions]}`` (``indices`` likewise)."""
        frame = self._frame()
        if not self._keys:
            return {(): list(range(len(frame)))}
        if len(frame) == 0:
            return {}
        row = "_row"
        while row in frame.columns:
            row = "_" + row
        marked = (
            frame.with_row_index(row) if self._rows is None else frame.with_column(row, self._rows)
        )
        order = cast("List[int]", marked.sort_values(self._keys)[row].to_list())
        sizes = self.size().sort_by_multi(self._keys)
        labels = [
            sizes[k].astype(DType.BOOL).to_list()
            if frame[k].dtype == DType.BOOL
            else sizes[k].to_list()
            for k in self._keys
        ]
        counts = cast("List[int]", sizes["size"].to_list())
        out: Dict[object, List[int]] = {}
        start = 0
        for g, n in enumerate(counts):
            label = labels[0][g] if len(labels) == 1 else tuple(c[g] for c in labels)
            out.setdefault(label, []).extend(order[start : start + n])
            start += n
        return dict(sorted(((k, sorted(v)) for k, v in out.items()), key=lambda kv: kv[1][0]))

    indices = groups

    def pipe(self, func: Callable[..., object], *args: object, **kwargs: object) -> object:
        return func(self, *args, **kwargs)

    def nunique(self) -> DataFrame:
        """Distinct present values per group and non-key column, exact: a
        group-by over the keys plus the column, then a count per key."""
        frame = self._frame()
        out: Optional[DataFrame] = None
        for c in [n for n in frame.columns if n not in self._keys]:
            present = frame.filter(self._col(frame, c).notna())
            pairs = GroupBy(present, self._keys + [c]).size()
            counts = GroupBy(pairs, self._keys).agg(Agg("count", None, c))
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
        index = getattr(frame, "_index", None)
        if index is not None and len(index) == 1:
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
                out = f"{c}_{'count' if name == 'count_valid' else name}"
                specs.append(Agg(name, Col(c), out))
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

    def value_counts(self, ascending: bool = False) -> DataFrame:
        """Distinct rows of the non-key columns within each group with their
        count, most frequent first."""
        frame = self._frame()
        cols = [c for c in frame.columns if c not in self._keys]
        for c in cols:  # pandas drops a row with a null value (dropna=True)
            frame = frame.filter(self._col(frame, c).notna())
        out = GroupBy(frame, self._keys + cols).size().rename({"size": "count"})
        return out.sort_values(
            self._keys + ["count"], ascending=[True] * len(self._keys) + [ascending]
        )

    def transform(self, func: str) -> DataFrame:
        """A group aggregate (``"mean"``, ``"sum"``, ...) broadcast back to
        every row of its group, one column per numeric non-key column, rows
        in input order."""
        frame = self._frame()
        agg = PANDAS_AGG_NAMES.get(func, func)
        cols = self._value_columns(agg)
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
            return out.select(*cols)
        joined = indexed.join(grouped, on=self._keys).sort_values(row)
        for c in cols:
            joined = joined.with_column(c, self._col(joined, f"__dftu_t_{c}__"))
        return joined.select(*cols)

    # -- user functions: traced into the engine, else run per group -----------
    def apply(self, func: "Callable[[Any], object]") -> "Union[DataFrame, Series]":
        """``func`` over each group (pandas ``GroupBy.apply``). It is first
        run once on a symbolic group whose columns are expressions, so a body
        like ``lambda g: g["v"].sum() / g["n"].sum()`` lowers to one group-by
        (a frame of the keys and ``value``), and ``lambda g: g["v"] -
        g["v"].mean()`` to a group-by joined back on the rows (a Series in
        input order). A body the trace cannot follow runs in Python per group
        with a warning: a scalar result per group gives the keys + ``value``
        frame, a Series or frame result the concatenation."""
        traced = trace_group(func, self._frame().columns)
        if traced is not None:
            return self._lower_apply(traced)
        return self._apply_python(func)

    def filter(self, func: "Callable[[Any], object]") -> DataFrame:
        """The rows of the groups for which ``func`` holds (pandas
        ``GroupBy.filter``): ``lambda g: g["v"].sum() > 10`` lowers to one
        group-by and a join; a body the trace cannot follow runs in Python per
        group with a warning. Rows come back in input order."""
        traced = trace_group(func, self._frame().columns, "GroupBy.filter")
        if traced is not None:
            return self._lower_filter(traced)
        return self._filter_python(func)

    def _group_values(self, expr: Expr) -> "Tuple[DataFrame, Expr]":
        """The per-group frame holding every aggregate leaf of ``expr`` (keys
        plus ``__dftu_agg_<i>__`` columns) and the expression rewritten over
        it."""
        rewritten, aggs = split_aggs(expr)
        if not aggs:
            raise TypeError("the group function must reduce (sum, mean, ...) at least once")
        return self.agg(*aggs), rewritten

    def _lower_apply(self, traced: object) -> "Union[DataFrame, Series]":
        frame = self._frame()
        if isinstance(traced, Agg):
            return self.agg(traced.alias("value"))
        if not isinstance(traced, Expr):
            raise TypeError(
                f"the group function returned {type(traced).__name__}; expected an "
                "aggregate or an expression over the group"
            )
        if not has_agg_leaf(traced):
            # A row expression with no aggregate: one value per row.
            return traced.apply(frame)
        grouped, rewritten = self._group_values(traced)
        row_cols = [c for c in collect_columns(traced) if c in frame]
        if not row_cols:
            value = rewritten.apply(grouped)
            return grouped.with_column("value", value).select(*(self._keys + ["value"]))
        # Row leaves too: the aggregates broadcast back onto the rows.
        row = "__dftu_row__"
        indexed = frame.with_row_index(row)
        joined = indexed.join(grouped, on=self._keys).sort_values(row) if self._keys else indexed
        if not self._keys:
            for name in grouped.columns:
                v = self._col(grouped, name)[0]
                assert isinstance(v, (int, float))
                joined = joined.with_column(name, self._col(frame, row_cols[0]).full_like(v))
        return rewritten.apply(joined)

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
            return frame if len(keep) else frame.head(0)
        row = "__dftu_row__"
        indexed = frame.with_row_index(row)
        out = indexed.join(keep.select(*self._keys), on=self._keys).sort_values(row)
        return out.select(*frame.columns)

    def _groups_in_order(self) -> "List[Tuple[object, List[int]]]":
        return list(self.groups.items())

    def _apply_python(self, func: "Callable[[Any], object]") -> "Union[DataFrame, Series]":
        frame = self._frame()
        results: List[Tuple[object, object]] = []
        for label, rows in self._groups_in_order():
            results.append((label, func(frame.take(rows))))
        if not results:
            return frame.head(0)
        first = results[0][1]
        if isinstance(first, DataFrame):
            out = results[0][1]
            assert isinstance(out, DataFrame)
            for _, r in results[1:]:
                assert isinstance(r, DataFrame)
                out = out.concat(r)
            return out
        if isinstance(first, Series):
            values: List[object] = []
            for _, r in results:
                assert isinstance(r, Series)
                values.extend(r.to_list())
            return Series.from_list(values)
        keys: Dict[str, List[object]] = {k: [] for k in self._keys}
        for label, _ in results:
            parts = label if isinstance(label, tuple) else (label,)
            for k, v in zip(self._keys, parts):
                keys[k].append(v)
        return DataFrame({**keys, "value": [r for _, r in results]})

    def _filter_python(self, func: "Callable[[Any], object]") -> DataFrame:
        frame = self._frame()
        keep: List[int] = []
        for _, rows in self._groups_in_order():
            if func(frame.take(rows)):
                keep.extend(rows)
        return frame.take(sorted(keep))


class GroupRolling:
    """``GroupBy.rolling(window)``: the pandas window object over a group-by,
    eager or lazy."""

    __slots__ = ("_grouped", "_window")

    def __init__(self, grouped: "Union[GroupBy, LazyGroupBy]", window: int) -> None:
        if window < 1:
            raise ValueError("rolling: the window must be at least 1")
        self._grouped = grouped
        self._window = window

    def sum(self) -> "Union[DataFrame, LazyFrame]":
        return self._grouped._transform("rolling_sum", self._window)

    def mean(self) -> "Union[DataFrame, LazyFrame]":
        return self._grouped._transform("rolling_mean", self._window)

    def min(self) -> "Union[DataFrame, LazyFrame]":
        return self._grouped._transform("rolling_min", self._window)

    def max(self) -> "Union[DataFrame, LazyFrame]":
        return self._grouped._transform("rolling_max", self._window)

    def var(self) -> DataFrame:
        return self._series_kernel(lambda s: s.rolling_var(self._window))

    def std(self) -> DataFrame:
        return self._series_kernel(lambda s: s.rolling_std(self._window))

    def median(self) -> DataFrame:
        return self._series_kernel(lambda s: s.rolling_median(self._window))

    def quantile(self, q: float) -> DataFrame:
        return self._series_kernel(lambda s: s.rolling_quantile(self._window, q))

    def _series_kernel(self, kernel: "Callable[[Series], Series]") -> DataFrame:
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
        return out.sort_by(row).select(*cols)


class _GroupSeriesWindow:
    """``GroupBy.expanding()`` / ``ewm(alpha)``: the Series window per group
    (a stateful scan, so each group runs on its own rows), the values back
    in input order."""

    __slots__ = ("_grouped", "_window")

    def __init__(self, grouped: GroupBy, window: "Callable[[Series], object]") -> None:
        self._grouped = grouped
        self._window = window

    def _reduce(self, name: str) -> DataFrame:
        g = self._grouped
        frame = g._frame()
        cols = g._value_columns()
        row = "__dftu_group_row__"
        parts: List[DataFrame] = []
        for _, rows in g._groups_in_order():
            part = frame.take(rows).with_column(row, Series.from_list(rows))
            for c in cols:
                part = part.with_column(c, getattr(self._window(g._col(part, c)), name)())
            parts.append(part.select(row, *cols))
        if not parts:
            return frame.select(*cols)
        out = parts[0].concat(*parts[1:]) if len(parts) > 1 else parts[0]
        return out.sort_by(row).select(*cols)

    def sum(self) -> DataFrame:
        return self._reduce("sum")

    def mean(self) -> DataFrame:
        return self._reduce("mean")

    def min(self) -> DataFrame:
        return self._reduce("min")

    def max(self) -> DataFrame:
        return self._reduce("max")

    def count(self) -> DataFrame:
        return self._reduce("count")

    def var(self) -> DataFrame:
        return self._reduce("var")

    def std(self) -> DataFrame:
        return self._reduce("std")


class GroupResample:
    """``GroupBy.resample(rule)``: the group-by over keys + time bucket, every
    result sorted by key then bucket and indexed by them (the pandas
    ``(key, time)`` index)."""

    __slots__ = ("_grouped", "_order")

    def __init__(self, grouped: "Union[GroupBy, LazyGroupBy]", order: List[str]) -> None:
        self._grouped = grouped
        self._order = order

    def __getattr__(self, name: str) -> object:
        attr = getattr(self._grouped, name)
        if not callable(attr):
            return attr

        def call(*args: object, **kwargs: object) -> object:
            out = attr(*args, **kwargs)
            names = out.columns if hasattr(out, "columns") else None
            if names is not None and all(c in names for c in self._order):
                out = out.sort_by_multi(self._order)
                out._index = list(self._order)
            return out

        return call


class _MissingColumn(KeyError):
    """A column the group function reads that the frame lacks: an error in
    the function, not a body the trace cannot follow."""


class _GroupTrace:
    """The symbolic group a traced group function runs on: a column read is
    the column expression, so reductions become aggregates."""

    __slots__ = ("_columns",)

    def __init__(self, columns: Sequence[str]) -> None:
        self._columns = list(columns)

    def __getitem__(self, name: str) -> Expr:
        if name not in self._columns:
            raise _MissingColumn(f"no column named {name!r}")
        return Col(name)

    def __getattr__(self, name: str) -> Expr:
        if name.startswith("_") or name not in self._columns:
            raise AttributeError(name)
        return Col(name)

    @property
    def columns(self) -> List[str]:
        return list(self._columns)

    def size(self) -> Agg:
        return Agg("count", None)

    def __len__(self) -> int:
        raise TypeError("len(group) is a Python value; use group.size() for the aggregate")


def trace_group(
    func: "Callable[[Any], object]", columns: Sequence[str], what: str = "GroupBy.apply"
) -> Optional[object]:
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
    if not isinstance(traced, (Agg, Expr)):
        warn_fallback(
            what,
            func,
            f"returned {type(traced).__name__}, not an aggregate or expression",
        )
        return None
    return traced


def group_quantile(
    plan: LazyFrame,
    keys: List[str],
    columns: List[str],
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
