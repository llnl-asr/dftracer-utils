"""A partitioned DataFrame over Dask futures, or in memory with no client.

``DaskFrame`` holds an ordered list of :class:`~dftracer.utils.DataFrame`
partitions. With a Dask ``client`` each partition is a ``Future`` held on a
worker; with ``client=None`` the partitions are in-memory frames and every
method runs the same functions in a loop, so both modes give equal results.

It covers the patterns a partitioned analysis needs: ``map_partitions`` (with an
optional ``overlap`` of rows from the previous and the next partition), a tree
``reduce``, ``group_by().agg()`` for aggregates that combine (and, with
``exact=True``, for median, quantile, distinct count and set union), ``shuffle``
by key (one or two stages; after it any per-group operation is exact), and
``join`` with a broadcast or a shuffle strategy.
"""

from __future__ import annotations

import inspect
import math
from typing import (
    TYPE_CHECKING,
    Callable,
    Dict,
    List,
    Optional,
    Sequence,
    Tuple,
    TypeVar,
    Union,
    cast,
)

from .columnar import Agg, GroupBy, col
from .dataframe import DataFrame
from .series import Series

if TYPE_CHECKING:
    from dask.distributed import Client, Future
    from pandas import DataFrame as PandasFrame
    from typing_extensions import ParamSpec

    _P = ParamSpec("_P")

__all__ = ["DaskFrame"]

Keys = Union[str, Sequence[str]]
# One ``agg`` value: an ``Agg``, a ``(column, op)`` pair or ``(column, "quantile", q)``.
AggSpec = Union[Agg, Tuple[str, str], Tuple[str, str, float]]
# A plain value ``map_partitions`` hands to every call of ``fn`` after the partition.
Extra = Union[str, int, float, bool, None, DataFrame, Sequence[str]]
_R = TypeVar("_R")
_T = TypeVar("_T")

# Aggregates that combine from per-partition partials, and the partial columns
# each needs: n = non-null count, s = sum, ss = sum of squares, m = mean and
# m2 = the sum of squared deviations from the mean (what var and std merge from).
_NEEDS: Dict[str, Tuple[str, ...]] = {
    "count": ("n",),
    "sum": ("s",),
    "min": ("mn",),
    "max": ("mx",),
    "sumsq": ("ss",),
    "mean": ("n", "s"),
    "var": ("n", "m", "m2"),
    "std": ("n", "m", "m2"),
}
# The engine's Agg op name for each aggregate a caller can write as an Agg.
_AGG_OP = {
    "count_valid": "count",
    "sum": "sum",
    "min": "min",
    "max": "max",
    "sumsq": "sumsq",
    "mean": "mean",
    "var": "var",
    "std": "std",
}


# Aggregates that need a whole group in one partition: exact only after a shuffle by the keys.
_HOLISTIC = ("median", "quantile", "nunique", "set_union")
# The combining aggregates computed straight on a whole group (the exact=True path).
_DIRECT: Dict[str, Callable[[str], Agg]] = {
    "count": lambda c: col(c).count(),
    "sum": lambda c: col(c).sum(),
    "min": lambda c: col(c).min(),
    "max": lambda c: col(c).max(),
    "sumsq": lambda c: col(c).sumsq(),
    "mean": lambda c: col(c).mean(),
    "var": lambda c: col(c).var(),
    "std": lambda c: col(c).std(),
}
# Shuffle by hash into pieces: above this many splits (inputs * outputs) use two stages.
_TREE_THRESHOLD = 1024


def _keys(keys: Keys) -> List[str]:
    return [keys] if isinstance(keys, str) else list(keys)


def _s(frame: DataFrame, name: str) -> Series:
    """One column of ``frame`` as a ``Series`` (``frame[name]`` is typed ``Series | DataFrame``)."""
    return cast(Series, frame[name])


def _concat(frames: Sequence[DataFrame]) -> DataFrame:
    return frames[0].concat(*frames[1:]) if len(frames) > 1 else frames[0]


def _pick(parts: Sequence[DataFrame], i: int) -> DataFrame:
    return parts[i]


def _split(frame: DataFrame, keys: List[str], n: int) -> List[DataFrame]:
    return frame.hash_partition(keys, n)


def _tail(frame: DataFrame, k: int) -> DataFrame:
    return frame.tail(k)


def _head(frame: DataFrame, k: int) -> DataFrame:
    return frame.head(k)


def _with_overlap(
    fn: Callable[[DataFrame], DataFrame],
    prev: Optional[DataFrame],
    frame: DataFrame,
    nxt: Optional[DataFrame],
) -> DataFrame:
    """``fn`` over ``prev + frame + nxt``, keeping only the rows that belong to ``frame``."""
    before = prev if prev is not None and len(prev) else None
    after = nxt if nxt is not None and len(nxt) else None
    if before is None and after is None:
        return fn(frame)
    lead = 0 if before is None else len(before)
    out = fn(_concat([x for x in (before, frame, after) if x is not None]))
    return cast(DataFrame, out.iloc[lead : lead + len(frame)])


def _join(left: DataFrame, right: DataFrame, on: Keys, how: str, kw: Dict[str, bool]) -> DataFrame:
    return left.join(right, on=on, how=how, **kw)  # ty: ignore[invalid-argument-type]


def _aggregate(frame: DataFrame, keys: List[str], aggs: Dict[str, Agg]) -> DataFrame:
    return cast(GroupBy, frame.group_by(keys)).agg(**aggs)


def _refs(frame: DataFrame, keys: List[str], columns: List[str]) -> DataFrame:
    """One partition's per-group reference values: the minimum of each column."""
    return _aggregate(frame, keys, {f"__ref_{c}": col(c).min() for c in columns})


def _min_fold(parts: Sequence[DataFrame], keys: List[str]) -> DataFrame:
    merged = _concat(parts)
    return _aggregate(merged, keys, {n: col(n).min() for n in merged.columns if n not in keys})


def _partial(
    frame: DataFrame,
    keys: List[str],
    plan: Dict[str, Tuple[str, ...]],
    refs: Optional[DataFrame] = None,
) -> DataFrame:
    """One partition's partial aggregates, one row per group.

    ``refs`` holds a reference value per group and column. The mean and the sum of squared
    deviations are taken of ``x - ref``: a variance does not change with a shift, and the
    shifted means are small, so they merge without losing the digits a mean of 1e9 or 1e12
    next to a spread of 1 would."""
    moment_cols = [c for c, needs in plan.items() if "m2" in needs] if refs is not None else []
    src = frame
    if moment_cols:
        src = frame.join(cast(DataFrame, refs), on=keys, how="inner")
        src = src.with_columns(
            **{f"__d_{c}": _s(src, c) - _s(src, f"__ref_{c}") for c in moment_cols}
        )
    aggs: Dict[str, Agg] = {}
    for c, needs in plan.items():
        x = f"__d_{c}" if c in moment_cols else c
        for kind in needs:
            aggs[f"__{kind}_{c}"] = {
                "n": col(c).count(),
                "s": col(c).sum(),
                "ss": col(c).sumsq(),
                "mn": col(c).min(),
                "mx": col(c).max(),
                "m": col(x).mean(),
                "m2": col(x).var(),  # the sample variance for now; M2 below
            }[kind]
    out = _aggregate(src, keys, aggs)
    for c, needs in plan.items():
        if "m2" in needs:
            # M2 = var * (n - 1), 0 for a group of fewer than two values; the mean of
            # an empty group is null, and weighs 0 in the merge.
            n = _s(out, f"__n_{c}")
            out = out.with_columns(
                **{
                    f"__m2_{c}": (_s(out, f"__m2_{c}") * (n - 1)).fillna(0.0),
                    f"__m_{c}": _s(out, f"__m_{c}").fillna(0.0),
                }
            )
    return out


def _combine(parts: Sequence[DataFrame], keys: List[str]) -> DataFrame:
    """Merge partial frames: counts, sums and sums of squares add; min and max fold;
    (n, mean, M2) merge as within-partition spread plus the spread of the means."""
    merged = _concat(parts)
    names = [n for n in merged.columns if n not in keys]
    moments = [n[4:] for n in names if n.startswith("__m_")]  # the columns with a mean
    aggs: Dict[str, Agg] = {}
    for n in names:
        kind = n[2:].split("_", 1)[0]
        if kind == "m":
            continue  # rebuilt below from the deviations of the means
        aggs[n] = col(n).min() if kind == "mn" else col(n).max() if kind == "mx" else col(n).sum()
    if not moments:
        return _aggregate(merged, keys, aggs)
    # Take every mean relative to its group's smallest mean, so the weighted mean and the
    # between-partition spread are sums of small numbers however large the means are.
    refs = _aggregate(merged, keys, {f"__ref_{c}": col(f"__m_{c}").min() for c in moments})
    merged = merged.join(refs, on=keys, how="inner")
    dev = {}
    for c in moments:
        d = _s(merged, f"__m_{c}") - _s(merged, f"__ref_{c}")
        dev[f"__d_{c}"] = d
        dev[f"__nd_{c}"] = _s(merged, f"__n_{c}") * d
    merged = merged.with_columns(**dev)
    step1 = dict(aggs)
    for c in moments:
        step1[f"__nd_{c}"] = col(f"__nd_{c}").sum()
        step1[f"__ref_{c}"] = col(f"__ref_{c}").min()
    first = _aggregate(merged, keys, step1)
    mean_d = {}
    for c in moments:
        n_total = _s(first, f"__n_{c}")
        mean_d[f"__md_{c}"] = (_s(first, f"__nd_{c}") / n_total).fillna(0.0)
    first = first.with_columns(**mean_d)
    merged = merged.join(first.select(*keys, *mean_d), on=keys, how="inner")
    between = {}
    for c in moments:
        e = _s(merged, f"__d_{c}") - _s(merged, f"__md_{c}")
        between[f"__b_{c}"] = _s(merged, f"__n_{c}") * e * e
    merged = merged.with_columns(**between)
    second = _aggregate(merged, keys, {f"__b_{c}": col(f"__b_{c}").sum() for c in moments})
    out = first.join(second, on=keys, how="inner")
    fin = {}
    for c in moments:
        fin[f"__m_{c}"] = _s(out, f"__ref_{c}") + _s(out, f"__md_{c}")
        fin[f"__m2_{c}"] = _s(out, f"__m2_{c}") + _s(out, f"__b_{c}")
    out = out.with_columns(**fin)
    drop = {f"__nd_{c}" for c in moments} | {f"__ref_{c}" for c in moments}
    drop |= {f"__md_{c}" for c in moments} | {f"__b_{c}" for c in moments}
    return out.select(*[n for n in out.columns if n not in drop])


def _finish(partial: DataFrame, keys: List[str], specs: List[Tuple[str, str, str]]) -> DataFrame:
    """Turn the merged partials into the requested aggregates."""
    out = {}
    for name, c, op in specs:
        n, s, ss = col(f"__n_{c}"), col(f"__s_{c}"), col(f"__ss_{c}")
        if op == "count":
            out[name] = n
        elif op == "sum":
            out[name] = s
        elif op == "min":
            out[name] = col(f"__mn_{c}")
        elif op == "max":
            out[name] = col(f"__mx_{c}")
        elif op == "sumsq":
            out[name] = ss
        elif op == "mean":
            out[name] = s / n
        else:
            # var and std come from the merged (n, mean, M2): M2 / (n - 1), null below two values.
            nn, m2 = _s(partial, f"__n_{c}"), _s(partial, f"__m2_{c}")
            var = (m2 / (nn - 1)).where(nn >= 2, None)
            out[name] = var if op == "var" else var.sqrt()
    return partial.with_columns(**out).select(*keys, *[name for name, _, _ in specs])


def _parse(name: str, spec: AggSpec) -> Tuple[str, str, Optional[float]]:
    """One ``agg`` value as ``(column, op, param)``: an ``Agg`` over one plain column, or a
    ``(column, op)`` pair, or ``(column, "quantile", q)``. A ``col(c).quantile(q)`` Agg is a
    quantile, ``median`` is the 0.5 quantile."""
    if isinstance(spec, Agg):
        column = getattr(spec.value, "name", None)
        op, param = _AGG_OP.get(spec.op, spec.op), None
        if op == "pct":
            op, param = "quantile", spec.param
    else:
        column, op, *rest = spec
        param = rest[0] if rest else None
    if column is None:
        raise ValueError(
            f"agg {name!r}: only an aggregate over one plain column combines from partials"
        )
    if op == "quantile" and param is None:
        raise ValueError(f"agg {name!r}: a quantile needs its level, as (column, 'quantile', q)")
    return column, op, param


def _exact_part(
    frame: DataFrame, keys: List[str], specs: List[Tuple[str, str, str, Optional[float]]]
) -> DataFrame:
    """Every requested aggregate per group over one whole-group partition."""
    outs: List[DataFrame] = []
    direct = {n: _DIRECT[op](c) for n, c, op, _ in specs if op in _DIRECT}
    if direct:
        outs.append(_aggregate(frame, keys, direct))
    for name, c, op, param in specs:
        if op in ("median", "quantile"):
            g = (
                frame.select(*keys, c)
                .group_by(keys)
                .quantile(0.5 if op == "median" else cast(float, param))
            )
            outs.append(g.with_columns(**{name: _s(g, c)}).select(*keys, name))
        elif op == "nunique":
            outs.append(
                _aggregate(frame.select(*keys, c).drop_duplicates(), keys, {name: col(c).count()})
            )
        elif op == "set_union":
            outs.append(_aggregate(frame, keys, {name: col(c).set_union()}))
    out = outs[0]
    for o in outs[1:]:
        out = out.join(o, on=keys, how="inner")
    return out.select(*keys, *[n for n, _, _, _ in specs])


class _GroupBy:
    def __init__(self, frame: "DaskFrame", keys: List[str]) -> None:
        self._frame = frame
        self._keys = keys

    def agg(self, *, exact: bool = False, **named: AggSpec) -> "DaskFrame":
        """Aggregate per group. Each value is a ``col(c).sum()``-style ``Agg`` or a
        ``(column, name)`` pair with ``name`` one of ``count``, ``sum``, ``min``, ``max``,
        ``sumsq``, ``mean``, ``var``, ``std`` (``var`` and ``std`` are the sample form); the
        result is one partition, computed from per-partition partials.

        An aggregate that does not combine (``median``, ``quantile``, ``nunique``,
        ``set_union``) is refused unless ``exact=True``: then the frame is shuffled by the
        group keys once and every aggregate is computed per group, exactly: ``(column,
        "median")``, ``(column, "quantile", q)`` (or ``col(c).quantile(q)``, which means the
        exact value here), ``(column, "nunique")`` (distinct non-null values) and
        ``(column, "set_union")``. The result then stays hash-partitioned by the keys."""
        self._frame._need_parts("group_by().agg")
        specs: List[Tuple[str, str, str]] = []
        full: List[Tuple[str, str, str, Optional[float]]] = []
        plan: Dict[str, Tuple[str, ...]] = {}
        for name, spec in named.items():
            column, op, param = _parse(name, spec)
            if op not in _NEEDS and op not in _HOLISTIC:
                raise ValueError(
                    f"agg {name!r}: {op} does not combine from partials; shuffle by the group key first"
                )
            if op in _HOLISTIC and not exact:
                raise ValueError(
                    f"agg {name!r}: {op} does not combine from partials; shuffle by the group "
                    "key first (or pass exact=True)"
                )
            full.append((name, column, op, param))
            if op in _NEEDS:
                specs.append((name, column, op))
                plan[column] = tuple(dict.fromkeys(plan.get(column, ()) + _NEEDS[op]))
        f = self._frame
        keys = self._keys
        if exact:
            shuffled = f.shuffle(keys)
            return shuffled._like(
                [shuffled._submit(_exact_part, p, keys, full) for p in shuffled._parts]
            )
        moment_cols = [c for c, needs in plan.items() if "m2" in needs]
        refs = (
            f.reduce(lambda p: _refs(p, keys, moment_cols), lambda ps: _min_fold(ps, keys))
            if moment_cols
            else None
        )
        merged = f.reduce(lambda p: _partial(p, keys, plan, refs), lambda ps: _combine(ps, keys))
        return DaskFrame.from_frames([_finish(merged, keys, specs)], f._client)


class DaskFrame:
    """An ordered list of ``DataFrame`` partitions, on Dask workers or in memory.

    Build one with :meth:`from_frames` or :meth:`from_function`. With
    ``client=None`` everything runs in this process. The frame owns its
    futures: :meth:`close` (or dropping it) releases them.
    """

    # A partition is a ``DataFrame`` in memory or a ``Future`` of one on a worker. Both are typed
    # as the frame they stand for, because Dask resolves a future before it calls a function.
    def __init__(self, parts: List[DataFrame], client: Optional["Client"] = None) -> None:
        self._parts = parts
        self._client = client

    @classmethod
    def from_frames(
        cls, frames: Sequence[DataFrame], client: Optional["Client"] = None
    ) -> "DaskFrame":
        frames = list(frames)
        parts = (
            cast(List[DataFrame], client.scatter(frames, hash=False))
            if client is not None
            else frames
        )
        return cls(parts, client)

    @classmethod
    def from_function(
        cls, fn: Callable[[_T], DataFrame], args: Sequence[_T], client: Optional["Client"] = None
    ) -> "DaskFrame":
        """One partition per element of ``args``: ``fn(arg)`` runs on a worker (or here)."""
        f = cls([], client)
        f._parts = [f._submit(fn, a) for a in args]
        return f

    @property
    def npartitions(self) -> int:
        return len(self._parts)

    def _submit(self, fn: Callable["_P", _R], *args: "_P.args", **kwargs: "_P.kwargs") -> _R:
        if self._client is None:
            return fn(*args, **kwargs)
        return cast(_R, self._client.submit(fn, *args, pure=False, **kwargs))

    def _gather(self, items: List[_R]) -> List[_R]:
        return list(items) if self._client is None else self._client.gather(items)

    def _like(self, parts: List[DataFrame]) -> "DaskFrame":
        return DaskFrame(parts, self._client)

    def close(self) -> None:
        """Release the partitions' futures."""
        if self._client is not None:
            for p in self._parts:
                cast("Future[DataFrame]", p).release()
        self._parts = []

    def _need_parts(self, what: str) -> None:
        if not self._parts:
            raise ValueError(f"DaskFrame.{what}: the frame has no partitions")

    def to_frame(self) -> DataFrame:
        """One frame: the partitions concatenated in order."""
        if not self._parts:
            raise ValueError("DaskFrame has no partitions")
        return _concat(self._gather(self._parts))

    def to_pandas(
        self,
        *,
        arrow: bool = False,
        nullable: bool = False,
        index: "Union[str, Sequence[str], None]" = None,
    ) -> "PandasFrame":
        """:meth:`DataFrame.to_pandas` of the concatenated partitions."""
        return self.to_frame().to_pandas(arrow=arrow, nullable=nullable, index=index)

    def map_partitions(
        self,
        fn: Callable[..., DataFrame],
        *args: Extra,
        overlap: int = 0,
        overlap_next: int = 0,
    ) -> "DaskFrame":
        """``fn(partition, *args)`` on every partition, results in the same order.

        With ``overlap=k``, ``fn`` also sees the last ``k`` rows of the previous
        partition in front of the partition, and with ``overlap_next=m`` the first ``m``
        rows of the next partition after it; the extra output rows are dropped, so
        ``rolling``, ``diff`` and ``shift`` over a window reaching at most ``k`` rows back
        and ``m`` rows ahead equal the single-frame result (a ``lead`` or a centered
        window needs ``overlap_next``). ``fn`` must keep one output row per input row, in
        order.
        """
        self._need_parts("map_partitions")
        if overlap < 0 or overlap_next < 0:
            raise ValueError("overlap and overlap_next must be 0 or more")
        if overlap == 0 and overlap_next == 0:
            return self._like([self._submit(fn, p, *args) for p in self._parts])
        # The overlap comes from the neighbouring partition only, so a window that
        # reaches past a neighbour shorter than the overlap is wrong.
        n = len(self._parts)
        prevs: List[Optional[DataFrame]] = [None] * n
        nexts: List[Optional[DataFrame]] = [None] * n
        if overlap:
            prevs[1:] = [self._submit(_tail, p, overlap) for p in self._parts[:-1]]
        if overlap_next:
            nexts[:-1] = [self._submit(_head, p, overlap_next) for p in self._parts[1:]]

        def bound(frame: DataFrame) -> DataFrame:
            return fn(frame, *args)

        return self._like(
            [
                self._submit(_with_overlap, bound, pv, p, nx)
                for pv, p, nx in zip(prevs, self._parts, nexts)
            ]
        )

    def reduce(
        self,
        partial: Callable[[DataFrame], _R],
        combine: Callable[[List[_R]], _R],
        split_every: int = 8,
    ) -> _R:
        """``partial`` on every partition, then ``combine`` on groups of up to
        ``split_every`` results as a tree. ``combine`` takes a list and must give
        a value it can take again. One partition gives its ``partial`` unchanged."""
        self._need_parts("reduce")
        if split_every < 2:
            raise ValueError("split_every must be at least 2")
        items = [self._submit(partial, p) for p in self._parts]
        while len(items) > 1:
            items = [
                self._submit(combine, items[i : i + split_every])
                for i in range(0, len(items), split_every)
            ]
        return self._gather(items)[0]

    def group_by(self, keys: Keys) -> _GroupBy:
        return _GroupBy(self, _keys(keys))

    def shuffle(
        self, keys: Keys, n: Optional[int] = None, stages: Optional[int] = None
    ) -> "DaskFrame":
        """Hash-partition by ``keys`` into ``n`` partitions (default: as many as now), so all
        rows of a key are in one partition. A key goes to the same partition in every process,
        and in both forms below.

        One stage runs ``M`` splits and ``M * N`` small fetch tasks. Two stages group the rows
        into about ``sqrt(N)`` coarse partitions first and split those, which is about
        ``M * sqrt(N) + N * sqrt(N)`` tasks and moves the data twice. ``stages=None`` picks two
        when ``M * N`` is above 1024 and ``N`` is at least 4, else one; ``stages=1`` or ``2``
        forces it (two needs ``N >= 4``). The rows inside a partition may be in another order.
        """
        self._need_parts("shuffle")
        k = _keys(keys)
        n = n or self.npartitions
        if stages not in (None, 1, 2):
            raise ValueError("stages must be 1 or 2")
        if stages is None:
            stages = 2 if self.npartitions * n > _TREE_THRESHOLD and n >= 4 else 1
        elif stages == 2 and n < 4:
            raise ValueError("a two-stage shuffle needs at least 4 output partitions")
        if stages == 1:
            splits = [self._submit(_split, p, k, n) for p in self._parts]
            pieces = [[self._submit(_pick, sp, j) for sp in splits] for j in range(n)]
            return self._like([self._submit(_concat, pc) for pc in pieces])
        # Stage 1 only groups rows into a coarse partitions; stage 2 recomputes the final
        # assignment with the one-stage hash, so output j holds exactly the rows the one-stage
        # form puts there, whatever a is.
        a = max(2, math.isqrt(n))
        first = [self._submit(_split, p, k, a) for p in self._parts]
        coarse = [
            self._submit(_concat, [self._submit(_pick, sp, i) for sp in first]) for i in range(a)
        ]
        second = [self._submit(_split, c, k, n) for c in coarse]
        return self._like(
            [self._submit(_concat, [self._submit(_pick, sp, j) for sp in second]) for j in range(n)]
        )

    def join(
        self,
        other: Union["DaskFrame", DataFrame],
        on: Keys,
        how: str = "inner",
        strategy: str = "auto",
        nulls_equal: Optional[bool] = None,
    ) -> "DaskFrame":
        """Join with ``other`` on ``on``. ``strategy="broadcast"`` copies ``other`` to every
        partition (``how`` inner or left only: an unmatched row of the broadcast side would
        repeat on every partition); ``"shuffle"`` hashes both sides by ``on`` and joins matching
        partitions, for any ``how`` including ``right`` and ``outer``; ``"auto"`` broadcasts when
        ``other`` is a frame or one partition and ``how`` is inner or left, else shuffles.

        Null keys match as the engine's ``join`` says. ``nulls_equal=True`` or ``False`` is
        passed to it, and raises ``NotImplementedError`` if this build's ``join`` has no such
        option."""
        self._need_parts("join")
        if isinstance(other, DaskFrame):
            other._need_parts("join (the other frame)")
        if strategy not in ("auto", "broadcast", "shuffle"):
            raise ValueError(f"unknown join strategy {strategy!r}")
        kw: Dict[str, bool] = {}
        if nulls_equal is not None:
            if "nulls_equal" not in inspect.signature(DataFrame.join).parameters:
                raise NotImplementedError(
                    "DataFrame.join has no nulls_equal option in this build (change join-null-keys)"
                )
            kw["nulls_equal"] = nulls_equal
        if strategy == "auto":
            small = isinstance(other, DataFrame) or other.npartitions == 1
            strategy = "broadcast" if small and how in ("inner", "left") else "shuffle"
        if strategy == "broadcast":
            if how not in ("inner", "left"):
                raise ValueError(
                    f"a broadcast join cannot do how={how!r}: an unmatched row of the broadcast "
                    "side would repeat on every partition; use strategy='shuffle'"
                )
            right = other if isinstance(other, DataFrame) else other.to_frame()
            ref = self._client.scatter(right, broadcast=True) if self._client is not None else right
            return self._like([self._submit(_join, p, ref, on, how, kw) for p in self._parts])
        right_frame = (
            other if isinstance(other, DaskFrame) else DaskFrame.from_frames([other], self._client)
        )
        n = max(self.npartitions, right_frame.npartitions)
        left, right_s = self.shuffle(on, n), right_frame.shuffle(on, n)
        return self._like(
            [self._submit(_join, a, b, on, how, kw) for a, b in zip(left._parts, right_s._parts)]
        )
