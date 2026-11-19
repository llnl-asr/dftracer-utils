# SPDX-License-Identifier: MIT
"""The view aggregation's std and var do not lose their digits when the mean is
large next to the spread: the View's own var/std through TraceViewer and across
shards."""

import gzip
import json

import numpy as np
import pytest

pytest.importorskip("pyarrow")

from dftracer.utils import AggregationConfig, Indexer, TraceViewer  # noqa: E402

REL = 1e-9


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
        files=files, index_dir=idx, aggregation_config=AggregationConfig(time_interval_ms=100000)
    ) as ix:
        ix.ensure()
    check(
        TraceViewer(files, index_dir=idx).group_by("name").agg("var:dur", "std:dur").collect(),
        values,
    )
    check(
        TraceViewer(files, index_dir=None).group_by("name").agg("var:dur", "std:dur").collect(),
        values,
    )


def test_the_distributed_view_merges_shards_stably(tmp_path):
    pytest.importorskip("distributed")
    from dask.distributed import Client, LocalCluster

    from dftracer.utils.dask import DaskAggregatedTraceViewer, DaskTraceViewer

    files, values = write_trace(tmp_path)
    idx = str(tmp_path / "idx")
    with Indexer(
        files=files, index_dir=idx, aggregation_config=AggregationConfig(time_interval_ms=100000)
    ) as ix:
        ix.ensure()
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
