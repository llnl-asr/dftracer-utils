"""The partial view aggregation, its meta and its merge agree: one column order,
dtypes kept across the merge, and the same results through a Dask cluster."""

import numpy as np
import pandas as pd
import pytest

pytest.importorskip("pyarrow")
dd = pytest.importorskip("dask.dataframe")
pytest.importorskip("distributed")

import pyarrow as pa  # noqa: E402

from dftracer.utils.dfanalyzer import (  # noqa: E402
    build_final_meta,
    build_partial_meta,
    finalize_view_partials,
    merge_view_partials,
    partial_arrow_view_groupby,
)

FULL = ["x", "y"]
SUM, MIN, MAX = ["s"], [], ["m"]


def records(n=3000, seed=0):
    rng = np.random.default_rng(seed)
    return pd.DataFrame(
        {
            "v": rng.integers(0, 6, n),
            "x": 1e9 + rng.normal(0, 2.0, n),
            "y": rng.normal(50.0, 5.0, n),
            "s": rng.integers(0, 100, n),
            "m": rng.normal(0, 1, n),
        }
    )


def partial(frame):
    return partial_arrow_view_groupby(frame, "v", FULL, SUM, MIN, MAX, [], lambda s: s)


def ddf(frame, parts=3):
    return dd.from_pandas(frame, npartitions=parts)


def test_data_and_meta_have_the_same_columns():
    frame = records()
    meta = build_partial_meta(ddf(frame), "v", FULL, SUM, MIN, MAX, [])
    assert list(partial(frame).columns) == list(meta.columns)


def test_an_empty_partition_has_the_same_columns():
    frame = records()
    meta = build_partial_meta(ddf(frame), "v", FULL, SUM, MIN, MAX, [])
    assert list(partial(frame.iloc[:0]).columns) == list(meta.columns)


def test_the_cluster_path_computes_and_the_std_is_exact():
    from dask.distributed import Client, LocalCluster

    frame = records()
    parts = ddf(frame, 3)
    meta = build_partial_meta(parts, "v", FULL, SUM, MIN, MAX, [])
    partials = parts.map_partitions(
        partial_arrow_view_groupby, "v", FULL, SUM, MIN, MAX, [], lambda s: s, meta=meta
    )
    with LocalCluster(
        n_workers=2, threads_per_worker=1, processes=True, silence_logs=40
    ) as cluster:
        with Client(cluster):
            computed = partials.compute()
            merged = merge_view_partials(partials, FULL)
            merged_pd = merged.compute()
    final = finalize_view_partials(merge_view_partials(computed, FULL), FULL)
    for key, group in frame.groupby("v"):
        want = np.std(group["x"].to_numpy(), ddof=1)
        got = float(final.loc[key, "x_std"])
        assert abs(got - want) / want < 1e-9
    assert list(merged_pd.columns) == list(merge_view_partials(computed, FULL).columns)


def test_the_merge_keeps_every_dtype_of_the_partials():
    frame = records()
    pieces = np.array_split(np.arange(len(frame)), 3)
    partials = pd.concat([partial(frame.iloc[ix]) for ix in pieces])
    merged = merge_view_partials(partials, FULL)
    for name in merged.columns:
        assert merged[name].dtype == partials[name].dtype, name


def test_an_all_null_column_stays_its_type_after_replace():
    frame = records()
    frame["y"] = np.nan  # null in every partition
    pieces = np.array_split(np.arange(len(frame)), 3)
    partials = pd.concat([partial(frame.iloc[ix]) for ix in pieces])
    merged = merge_view_partials(partials, FULL)
    out = merged.replace(0, pd.NA)
    for name in ("y_min", "y_max", "y_sum"):
        assert merged[name].dtype == partials[name].dtype, name
        assert out[name].dtype == partials[name].dtype, name
        assert out[name].dtype != object


def test_a_dask_frame_of_partials_merges_to_the_same_values():
    frame = records()
    parts = ddf(frame, 3)
    meta = build_partial_meta(parts, "v", FULL, SUM, MIN, MAX, [])
    partials = parts.map_partitions(
        partial_arrow_view_groupby, "v", FULL, SUM, MIN, MAX, [], lambda s: s, meta=meta
    )
    as_pandas = merge_view_partials(partials.compute(), FULL)
    as_dask = merge_view_partials(partials, FULL).compute()
    pd.testing.assert_frame_equal(as_dask.sort_index(), as_pandas.sort_index())


def test_the_final_meta_names_follow_the_merged_columns():
    frame = records()
    parts = ddf(frame, 2)
    meta = build_partial_meta(parts, "v", FULL, SUM, MIN, MAX, [])
    merged = merge_view_partials(
        parts.map_partitions(
            partial_arrow_view_groupby, "v", FULL, SUM, MIN, MAX, [], lambda s: s, meta=meta
        ),
        FULL,
    )
    final_meta = build_final_meta(merged, FULL)
    assert {"x_mean", "x_std", "y_mean", "y_std"} <= set(final_meta.columns)
    assert all(
        not c.endswith(("_m2", "_mean_hi", "_mean_lo", "_count")) for c in final_meta.columns
    )
    assert pa.types.is_float64(final_meta["x_std"].dtype.pyarrow_dtype)
