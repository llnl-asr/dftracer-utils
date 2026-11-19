# SPDX-License-Identifier: MIT
"""An index path reused in one process: delete and rebuild, new aggregation
config, two trace dirs on one index dir, and the dask build."""

import gzip
import os
import shutil

import pyarrow as pa
import pytest

try:
    import dask  # noqa: F401

    DASK_AVAILABLE = True
except ImportError:
    DASK_AVAILABLE = False

from dftracer.utils import AggregationConfig, Indexer, TraceViewer

from .common import Environment

NAMES = {
    "A": (["read", "write", "open"], 1),
    "B": (["zeta", "alpha", "beta", "gamma"], 2),
}


def _write_trace(path, names, pid, count=40):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with gzip.open(path, "wt") as out:
        out.write(
            f'{{"name":"HH","ph":"M","pid":{pid},"tid":1,"args":{{"name":"host","value":"hh"}}}}\n'
        )
        out.write(
            f'{{"name":"FH","ph":"M","pid":{pid},"tid":1,"args":{{"name":"/d/f","value":"ff"}}}}\n'
        )
        for i in range(count):
            out.write(
                f'{{"name":"{names[i % len(names)]}","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{1000000 + i * 1000},"dur":{100 + i},"ph":"X",'
                f'"args":{{"ret":{i},"hhash":"hh","fhash":"ff"}}}}\n'
            )


def _typed(files, index_dir):
    table = pa.table(
        TraceViewer(files, index_dir=index_dir)
        .group_by("name", "pid")
        .agg("count")
        .collect_typed()["regular"]
    )
    return sorted((row["name"], row["pid"], row["count"]) for row in table.to_pylist())


def _expected(kind, count=40):
    names, pid = NAMES[kind]
    seen = {}
    for i in range(count):
        seen[names[i % len(names)]] = seen.get(names[i % len(names)], 0) + 1
    return sorted((name, str(pid), n) for name, n in seen.items())


def _build(files, index_dir, interval_ms=None):
    kwargs = {}
    if interval_ms is not None:
        kwargs["aggregation_config"] = AggregationConfig(time_interval_ms=interval_ms)
    with Indexer(files=files, index_dir=index_dir, **kwargs) as indexer:
        indexer.ensure()
        return indexer.status(), indexer.rowset("files").to_dict()["path"]


@pytest.mark.parametrize("how", ["idx", "all"])
def test_delete_and_rebuild_at_one_path(how):
    with Environment(lines=10) as env:
        root = os.path.join(env.temp_dir, "t")
        os.makedirs(root)
        trace = os.path.join(root, "a.pfw.gz")
        for kind, interval_ms in zip("ABAABBA", (None, None, None, 5, None, None, None)):
            if how == "all":
                shutil.rmtree(root)
                os.makedirs(root)
            else:
                shutil.rmtree(os.path.join(root, ".dftindex"), ignore_errors=True)
            _write_trace(trace, *NAMES[kind])
            _, paths = _build([trace], root, interval_ms)
            assert paths == ["/d/f"]
            assert [tuple(r) for r in _typed([trace], root)] == [tuple(r) for r in _expected(kind)]


def test_new_aggregation_config_at_the_same_path_is_seen():
    with Environment(lines=10) as env:
        root = os.path.join(env.temp_dir, "t")
        trace = os.path.join(root, "a.pfw.gz")
        _write_trace(trace, *NAMES["A"], count=200)
        for interval_ms in (5, 50, 5, 50):
            status, _ = _build([trace], root, interval_ms)
            assert status.aggregation_interval_us == interval_ms * 1000
            with Indexer(
                files=[trace],
                index_dir=root,
                aggregation_config=AggregationConfig(time_interval_ms=interval_ms),
            ) as indexer:
                assert indexer.status().aggregation_interval_us == interval_ms * 1000
            counts = {name: n for name, _, n in _typed([trace], root)}
            assert sum(counts.values()) == 200


def test_two_trace_dirs_share_one_index_dir():
    with Environment(lines=10) as env:
        index_dir = os.path.join(env.temp_dir, "shared")
        left = os.path.join(env.temp_dir, "left", "a.pfw.gz")
        right = os.path.join(env.temp_dir, "right", "b.pfw.gz")
        _write_trace(left, *NAMES["A"])
        _write_trace(right, *NAMES["B"])
        for order in ([left, right], [right, left], [left, right]):
            shutil.rmtree(index_dir, ignore_errors=True)
            _build(order, index_dir)
            assert _typed(order, index_dir) == sorted(_expected("A") + _expected("B"))


@pytest.mark.skipif(not DASK_AVAILABLE, reason="Dask not available")
def test_dask_build_delete_and_rebuild():
    from dask.distributed import Client, LocalCluster

    from dftracer.utils.dask import DaskTraceViewer, distributed_index

    with Environment(lines=10) as env:
        index_dir = os.path.join(env.temp_dir, "idx")
        index_path = os.path.join(index_dir, ".dftindex")
        traces = [os.path.join(env.temp_dir, f"t{k}", "a.pfw.gz") for k in range(2)]
        cluster = LocalCluster(processes=False, n_workers=2, threads_per_worker=2)
        client = Client(cluster)
        try:
            for kind in "ABA":
                shutil.rmtree(index_dir, ignore_errors=True)
                os.makedirs(index_dir)
                for trace in traces:
                    _write_trace(trace, *NAMES[kind])
                distributed_index(traces, index_dir=index_path, client=client)
                table = pa.table(
                    DaskTraceViewer(traces, index_dir, client=client)
                    .group_by("name", "pid")
                    .agg("count")
                    .collect_typed()["regular"]
                )
                got = sorted((r["name"], r["pid"], r["count"]) for r in table.to_pylist())
                assert got == sorted((n, p, 2 * c) for n, p, c in _expected(kind))
        finally:
            client.close()
            cluster.close()
