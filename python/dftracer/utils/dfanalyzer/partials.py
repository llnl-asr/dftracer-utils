# SPDX-License-Identifier: MIT
"""Partial view aggregations and their merge."""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    AbstractSet,
    Callable,
    Hashable,
    Iterable,
    Optional,
    Sequence,
    Union,
)

import numpy as np
import pandas as pd
import pyarrow as pa

if TYPE_CHECKING:
    import dask.dataframe as dd  # ty: ignore[import-not-found]


def _partial_agg_columns(full_cols, sum_cols, min_cols, max_cols, set_cols_items, dtype_of):
    """Metric column -> dtype map for a partial view aggregation, in the one
    order every partial has: per full column its sum, count, min, max, m2,
    mean_hi and mean_lo, then the sum, min, max and set columns. The data
    (`partial_arrow_view_groupby`), the empty partition and the dask meta all
    follow it. `dtype_of(col)` resolves a metric column's dtype from the
    caller's source (a live frame or the dask meta)."""
    cols = {}
    for c in full_cols:
        cols[f"{c}_sum"] = dtype_of(c)
        cols[f"{c}_count"] = pd.ArrowDtype(pa.int64())
        cols[f"{c}_min"] = dtype_of(c)
        cols[f"{c}_max"] = dtype_of(c)
        cols[f"{c}_m2"] = pd.ArrowDtype(pa.float64())
        cols[f"{c}_mean_hi"] = pd.ArrowDtype(pa.float64())
        cols[f"{c}_mean_lo"] = pd.ArrowDtype(pa.float64())
    for c in sum_cols:
        cols[f"{c}_sum"] = dtype_of(c)
    for c in min_cols:
        cols[f"{c}_min"] = dtype_of(c)
    for c in max_cols:
        cols[f"{c}_max"] = dtype_of(c)
    for c, _ in set_cols_items:
        cols[f"{c}_unique"] = "object"
    return cols


def _central_moments(work: pd.DataFrame, view_type: str, cols: Sequence[str]) -> pd.DataFrame:
    """Per view row and metric column: `{c}_m2`, the sum of squared deviations
    from the mean, and the mean as the pair `{c}_mean_hi` + `{c}_mean_lo` (a
    value near the mean and the small remainder), so a variance merged across
    partitions keeps its digits when the mean is large next to the spread.

    The deviations are taken from a first estimate of the mean and corrected
    (`m2 = sum(d^2) - sum(d)^2 / n`), the corrected two-pass form; nothing here
    subtracts two sums of squares."""
    out = {}
    key = work[view_type]
    for c in cols:
        x = pd.to_numeric(work[c], errors="coerce").astype("float64")
        grouped = x.groupby(key)
        n = grouped.count().astype("float64")
        hi = grouped.mean()
        d = x - key.map(hi)
        sd = d.groupby(key).sum()
        sd2 = (d * d).groupby(key).sum()
        with np.errstate(invalid="ignore", divide="ignore"):
            lo = (sd / n).where(n > 0, 0.0)
            m2 = (sd2 - sd * sd / n).where(n > 0, 0.0).clip(lower=0.0)
        out[f"{c}_m2"] = m2
        out[f"{c}_mean_hi"] = hi.fillna(0.0)
        out[f"{c}_mean_lo"] = lo
    return pd.DataFrame(out)


def merge_view_partials(
    df: Union[pd.DataFrame, dd.DataFrame],
    full_cols: Sequence[str],
    sum_cols: Sequence[str] = (),
    min_cols: Sequence[str] = (),
    max_cols: Sequence[str] = (),
    set_cols: Sequence[str] = (),
    flatten_fn: Optional[Callable[[Iterable[AbstractSet[Hashable]]], AbstractSet[Hashable]]] = None,
) -> Union[pd.DataFrame, dd.DataFrame]:
    """Merge partial view aggregations (the output of `partial_arrow_view_groupby`,
    one row per view row per partition) into one row per view row, ready for
    `finalize_view_partials`. The columns are named as the partials name them:

    - full columns (`full_cols`): `_sum` and `_count` add, `_min` and `_max` take
      their extremes, and `_m2` with the mean pair (`_mean_hi`, `_mean_lo`)
      combine with the pairwise (Chan) formula about each row's first mean, so a
      variance is never rebuilt from a sum of squares;
    - `sum_cols`, `min_cols`, `max_cols`: `_sum` adds, `_min` and `_max` take
      their extreme;
    - `set_cols` (the names of the set columns): `_unique` cells are joined with
      `flatten_fn`, the function the partials were flattened with.

    A column that no partial holds is skipped. The result has the partials' column
    order and each column keeps the dtype of the partials' column of that name (an
    all-null column stays its type). The index is the view row (a MultiIndex is
    grouped by every level). A Dask frame of partials is merged on the client, the
    partials being one row per view row per partition, and comes back as a
    one-partition Dask frame."""
    if set_cols and flatten_fn is None:
        raise ValueError("merge_view_partials: set_cols need a flatten_fn")
    if hasattr(df, "compute"):
        import dask.dataframe as dd
        from dask import delayed

        merged = merge_view_partials(
            df.compute(), full_cols, sum_cols, min_cols, max_cols, set_cols, flatten_fn
        )
        return dd.from_delayed([delayed(merged)], meta=merged.iloc[:0])
    levels = list(range(df.index.nlevels))
    g = lambda s: s.groupby(level=levels)  # noqa: E731
    out = {}
    for c in full_cols:
        need = [f"{c}_{k}" for k in ("sum", "count", "min", "max", "m2", "mean_hi", "mean_lo")]
        if any(name not in df.columns for name in need):
            continue
        n = df[f"{c}_count"].astype("float64").fillna(0.0)
        hi = df[f"{c}_mean_hi"].astype("float64").fillna(0.0)
        lo = df[f"{c}_mean_lo"].astype("float64").fillna(0.0)
        m2 = df[f"{c}_m2"].astype("float64").fillna(0.0)
        ref_hi, ref_lo = g(hi).transform("first"), g(lo).transform("first")
        delta = (hi - ref_hi) + (lo - ref_lo)  # each partition's mean from the first one's
        total = g(n).transform("sum")
        with np.errstate(invalid="ignore", divide="ignore"):
            centre = (g(n * delta).transform("sum") / total).where(total > 0, 0.0)
        spread = n * (delta - centre) ** 2
        out[f"{c}_sum"] = g(df[f"{c}_sum"]).sum()
        out[f"{c}_count"] = g(df[f"{c}_count"]).sum()
        out[f"{c}_min"] = g(df[f"{c}_min"]).min()
        out[f"{c}_max"] = g(df[f"{c}_max"]).max()
        out[f"{c}_m2"] = g(m2 + spread).sum().astype(df[f"{c}_m2"].dtype)
        out[f"{c}_mean_hi"] = g(ref_hi).first().astype(df[f"{c}_mean_hi"].dtype)
        out[f"{c}_mean_lo"] = (
            g(ref_lo).first() + g(pd.Series(centre, index=df.index)).first()
        ).astype(df[f"{c}_mean_lo"].dtype)
    for c in sum_cols:
        if f"{c}_sum" in df.columns:
            out[f"{c}_sum"] = g(df[f"{c}_sum"]).sum()
    for c in min_cols:
        if f"{c}_min" in df.columns:
            out[f"{c}_min"] = g(df[f"{c}_min"]).min()
    for c in max_cols:
        if f"{c}_max" in df.columns:
            out[f"{c}_max"] = g(df[f"{c}_max"]).max()
    for c in set_cols:
        if f"{c}_unique" in df.columns:
            out[f"{c}_unique"] = g(df[f"{c}_unique"]).agg(flatten_fn)
    merged = pd.DataFrame(out)
    order = _partial_agg_columns(
        full_cols, sum_cols, min_cols, max_cols, list(zip(set_cols, set_cols)), lambda c: None
    )
    return merged[[name for name in order if name in merged.columns]]


def partial_arrow_view_groupby(
    df,
    view_type,
    full_cols,
    sum_cols,
    min_cols,
    max_cols,
    set_cols_items,
    flatten_fn,
):
    """Per-partition Arrow groupby emitting mergeable partial aggregates.

    `flatten_fn` flattens a grouped set-column series when its aggregation
    object exposes no `chunk` method.
    """
    view_type_in_index = (isinstance(df.index, pd.MultiIndex) and view_type in df.index.names) or (
        df.index.name == view_type
    )
    work = df.reset_index() if view_type_in_index else df
    if work.empty:
        # Derive dtypes from the input columns so an empty partition matches
        # the meta declared by the caller (which uses the same rule).
        def _col_dtype(col, default=pd.ArrowDtype(pa.float64())):
            if col in work.columns:
                return work[col].dtype
            return default

        cols = _partial_agg_columns(
            full_cols, sum_cols, min_cols, max_cols, set_cols_items, _col_dtype
        )
        out = pd.DataFrame({name: pd.Series(dtype=dt) for name, dt in cols.items()})
        out.index = pd.Index(
            [],
            name=view_type,
            dtype=_col_dtype(view_type, default=pd.ArrowDtype(pa.int64())),
        )
        return out

    arrow_keep = [view_type]
    for lst in (full_cols, sum_cols, min_cols, max_cols):
        for c in lst:
            if c in work.columns and c not in arrow_keep:
                arrow_keep.append(c)
    tbl = pa.Table.from_pandas(work[arrow_keep], preserve_index=False)

    agg_specs = []
    for c in full_cols:
        if c not in tbl.schema.names:
            continue
        agg_specs += [
            (c, "sum"),
            (c, "count"),
            (c, "min"),
            (c, "max"),
        ]
    for c in sum_cols:
        if c in tbl.schema.names:
            agg_specs.append((c, "sum"))
    for c in min_cols:
        if c in tbl.schema.names:
            agg_specs.append((c, "min"))
    for c in max_cols:
        if c in tbl.schema.names:
            agg_specs.append((c, "max"))

    if agg_specs:
        result = tbl.group_by([view_type]).aggregate(agg_specs)
        out = result.to_pandas(types_mapper=pd.ArrowDtype)
        out = out.set_index(view_type)
        moments = _central_moments(work, view_type, [c for c in full_cols if c in work.columns])
        for name in moments.columns:
            out[name] = moments[name].reindex(out.index).astype(pd.ArrowDtype(pa.float64()))
    else:
        uniq = work[view_type].drop_duplicates().reset_index(drop=True)
        out = pd.DataFrame(index=pd.Index(uniq, name=view_type))

    for col, agg in set_cols_items:
        if col not in work.columns:
            continue
        sgb = work.groupby(view_type)[col]
        chunk_fn = getattr(agg, "chunk", None)
        partial = chunk_fn(sgb) if chunk_fn is not None else sgb.apply(flatten_fn)
        partial.name = f"{col}_unique"
        out = out.join(partial, how="left")
    order = _partial_agg_columns(
        full_cols, sum_cols, min_cols, max_cols, set_cols_items, lambda c: None
    )
    return out[[name for name in order if name in out.columns]]


def finalize_view_partials(df, full_cols):
    """Compute mean/std per view_type row from merged partials; drop helper cols."""
    if df.empty:
        return df
    out = df.copy()
    drop = []
    for c in full_cols:
        sum_c = f"{c}_sum"
        count_c = f"{c}_count"
        m2_c = f"{c}_m2"
        if sum_c not in out.columns or count_c not in out.columns:
            continue
        s = out[sum_c].astype("float64")
        n = out[count_c].astype("float64")
        mean_v = s / n
        out[f"{c}_mean"] = mean_v.astype(pd.ArrowDtype(pa.float64()))
        if m2_c in out.columns:
            m2 = out[m2_c].astype("float64")
            # sample variance is undefined for n <= 1 -> std is NaN, matching
            # pandas .std(ddof=1); avoids a divide-by-zero on (n - 1).
            with np.errstate(invalid="ignore", divide="ignore"):
                var_v = (m2 / (n - 1)).where(n > 1, np.nan)
            out[f"{c}_std"] = np.sqrt(var_v).astype(pd.ArrowDtype(pa.float64()))
            drop += [m2_c, f"{c}_mean_hi", f"{c}_mean_lo"]
        drop.append(count_c)
    if drop:
        out = out.drop(columns=drop)
    return out


def build_partial_meta(records, view_type, full_cols, sum_cols, min_cols, max_cols, set_cols_items):
    """Dask meta for the output of `partial_arrow_view_groupby`."""
    in_meta = records._meta

    def _dtype_of(col, default=pd.ArrowDtype(pa.float64())):
        if col in in_meta.columns:
            return in_meta[col].dtype
        if isinstance(in_meta.index, pd.MultiIndex) and col in in_meta.index.names:
            return in_meta.index.get_level_values(col).dtype
        return default

    cols = _partial_agg_columns(full_cols, sum_cols, min_cols, max_cols, set_cols_items, _dtype_of)
    meta = pd.DataFrame({name: pd.Series(dtype=dt) for name, dt in cols.items()})
    idx_dtype = _dtype_of(view_type, default=pd.ArrowDtype(pa.int64()))
    meta.index = pd.Index([], name=view_type, dtype=idx_dtype)
    return meta


def build_final_meta(merged, full_cols):
    """Dask meta for the output of `finalize_view_partials`."""
    cols = {}
    for c in merged.columns:
        if c.endswith("_count") and c[: -len("_count")] in full_cols:
            continue
        if any(c == f"{f}_{k}" for f in full_cols for k in ("m2", "mean_hi", "mean_lo")):
            continue
        cols[c] = merged._meta[c].dtype
    for c in full_cols:
        cols[f"{c}_mean"] = pd.ArrowDtype(pa.float64())
        cols[f"{c}_std"] = pd.ArrowDtype(pa.float64())
    meta = pd.DataFrame({name: pd.Series(dtype=dt) for name, dt in cols.items()})
    meta.index = pd.Index([], name=merged._meta.index.name, dtype=merged._meta.index.dtype)
    return meta


def normalize_arrow_dtypes(df):
    """Demote Arrow-backed category columns to object for downstream pandas ops."""
    for col in df.select_dtypes(include=["category"]).columns:
        df[col] = df[col].astype("object")
    return df
