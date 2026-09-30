"""The view aggregation's std and var do not lose their digits when the mean is
large next to the spread: per-partition partials merged by the pairwise formula,
and the View's own var/std through TraceViewer and across shards."""

import gzip
import json

import numpy as np
import pandas as pd
import pytest

pytest.importorskip("pyarrow")

from dftracer.utils import AggregationConfig, Indexer, TraceViewer  # noqa: E402
from dftracer.utils.dfanalyzer import (  # noqa: E402
    finalize_view_partials,
    merge_view_partials,
    partial_arrow_view_groupby,
)

REL = 1e-9


def run(frame, parts, view="v", cols=("x",)):
    """partial per partition -> merge -> finalize, the analyzer's pipeline."""
    pieces = np.array_split(np.arange(len(frame)), parts)
    partials = [
        partial_arrow_view_groupby(frame.iloc[ix], view, list(cols), [], [], [], [], lambda s: s)
        for ix in pieces
        if len(ix)
    ]
    merged = merge_view_partials(pd.concat(partials), list(cols))
    return finalize_view_partials(merged, list(cols))


def truth(frame, view="v", col="x"):
    """numpy's two-pass std per group (pandas' own groupby std loses about 4e-9
    at an offset of 1e9, so it is no reference)."""
    stds = {
        k: np.std(g[col].dropna().to_numpy(), ddof=1) if g[col].count() > 1 else np.nan
        for k, g in frame.groupby(view)
    }
    return pd.DataFrame({"std": pd.Series(stds)})


@pytest.mark.parametrize("parts", [1, 2, 5, 16])
@pytest.mark.parametrize("offset,spread", [(0.0, 1.0), (1e6, 1.0), (1e9, 3.0), (1e12, 5.0)])
def test_std_matches_a_two_pass_reference(offset, spread, parts):
    rng = np.random.default_rng(parts)
    n = 4000
    frame = pd.DataFrame({"v": rng.integers(0, 7, n), "x": offset + rng.normal(0, spread, n)})
    got = run(frame, parts)
    want = truth(frame)
    std = got["x_std"].astype("float64")
    assert np.allclose(
        std.sort_index().to_numpy(), want["std"].sort_index().to_numpy(), rtol=REL, atol=0
    )


def test_the_sum_of_squares_route_would_lose_every_digit():
    rng = np.random.default_rng(0)
    x = 1e9 + rng.normal(0, 1, 20000)
    sq_formula = (np.sum(x * x) - np.sum(x) ** 2 / len(x)) / (len(x) - 1)
    exact = np.var(x, ddof=1)
    assert abs(sq_formula - exact) / exact > 1e-3  # the failure this change removes
    got = run(pd.DataFrame({"v": 0, "x": x}), 4)
    assert abs(got["x_std"].astype("float64").iloc[0] ** 2 - exact) / exact < REL


def test_keys_missing_from_some_partitions_and_single_rows():
    frame = pd.DataFrame(
        {
            "v": ["a"] * 6 + ["b"] * 3 + ["c"],
            "x": [1e9 + 0.5, 1e9 + 1.5, 1e9, 1e9 + 2, 1e9 + 3, 1e9 + 4, 5.0, 7.0, 6.0, 42.0],
        }
    )
    got = run(frame, 5)  # "c" has one row: its std is undefined
    want = truth(frame)
    std = got["x_std"].astype("float64")
    for key in ("a", "b"):
        assert std[key] == pytest.approx(want.loc[key, "std"], rel=REL)
    assert np.isnan(std["c"])
    assert got.loc["a", "x_sum"] == pytest.approx(frame[frame.v == "a"].x.sum())


def test_nulls_are_skipped():
    rng = np.random.default_rng(3)
    x = pd.Series(1e8 + rng.normal(0, 2, 3000))
    x[::9] = np.nan
    frame = pd.DataFrame({"v": rng.integers(0, 4, 3000), "x": x})
    got = run(frame, 6)
    assert np.allclose(
        got["x_std"].astype("float64").sort_index().to_numpy(),
        truth(frame)["std"].sort_index().to_numpy(),
        rtol=REL,
    )


def write_trace(tmp_path, shards=4, per_shard=6000, seed=5):
    rng = np.random.default_rng(seed)
    files, values = [], {"read": [], "write": []}
    for k in range(shards):
        path = str(tmp_path / f"t{k}.pfw.gz")
        with gzip.open(path, "wt") as f:
            for i in range(per_shard):
                name = ("read", "write")[i % 2]
                dur = int(1e9 + rng.normal(0, 3))
                values[name].append(dur)
                f.write(
                    json.dumps(
                        {
                            "ph": "X",
                            "name": name,
                            "cat": "POSIX",
                            "pid": 1,
                            "tid": 1,
                            "ts": 1000 + i + k * 10**6,
                            "dur": dur,
                            "args": {"fhash": "f", "ret": 64},
                        }
                    )
                    + "\n"
                )
        files.append(path)
    return files, {n: np.array(v, dtype=np.float64) for n, v in values.items()}


def check(frame, values):
    pdf = frame.to_pandas() if hasattr(frame, "to_pandas") else frame
    assert len(pdf) == 2
    for _, row in pdf.iterrows():
        v = values[row["name"]]
        assert row["var_dur"] == pytest.approx(np.var(v, ddof=1), rel=REL)
        assert row["std_dur"] == pytest.approx(np.std(v, ddof=1), rel=REL)


def test_the_view_var_and_std_are_stable(tmp_path):
    files, values = write_trace(tmp_path)
    idx = str(tmp_path / "idx")
    with Indexer(
        files=files, index_dir=idx, require_aggregation=AggregationConfig(time_interval_ms=100000)
    ) as ix:
        ix.ensure_indexed()
    check(
        TraceViewer(files, index_path=idx).group_by("name").agg("var:dur", "std:dur").collect(),
        values,
    )
    check(
        TraceViewer(files, index_path=None).group_by("name").agg("var:dur", "std:dur").collect(),
        values,
    )


def test_the_distributed_view_merges_shards_stably(tmp_path):
    pytest.importorskip("distributed")
    from dask.distributed import Client, LocalCluster

    from dftracer.utils.dask import DaskAggregatedTraceViewer, DaskTraceViewer

    files, values = write_trace(tmp_path)
    idx = str(tmp_path / "idx")
    with Indexer(
        files=files, index_dir=idx, require_aggregation=AggregationConfig(time_interval_ms=100000)
    ) as ix:
        ix.ensure_indexed()
    cluster = LocalCluster(n_workers=2, threads_per_worker=1, processes=False, silence_logs=40)
    client = Client(cluster)
    try:
        for viewer in (DaskAggregatedTraceViewer, DaskTraceViewer):
            check(
                viewer(files, idx, client=client)
                .group_by("name")
                .agg("var:dur", "std:dur")
                .collect(),
                values,
            )
    finally:
        client.close()
        cluster.close()
