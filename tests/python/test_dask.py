#!/usr/bin/env python3
"""
End-to-end Dask integration tests for  utilities
Tests combining indexer and reader functionality with Dask distributed computing
"""

import os

import pytest

try:
    import dask
    import dask.dataframe as dd

    DASK_AVAILABLE = True
except ImportError:
    DASK_AVAILABLE = False

import dftracer.utils as dftu_utils
from dftracer.utils.dftracer_utils_ext import CheckpointIndexer as NativeIndexer

from .common import Environment


@pytest.mark.skipif(not DASK_AVAILABLE, reason="Dask not available")
class TestDaskIntegration:
    """End-to-end tests with Dask distributed computing"""

    def test_dask_basic_import(self):
        """Test that Dask imports work correctly"""
        assert DASK_AVAILABLE
        assert hasattr(dask, "__version__")
        assert hasattr(dd, "DataFrame")

    def test_parallel_indexer_creation(self):
        """Test creating multiple indexers in parallel with Dask"""
        with Environment(lines=1000) as env:
            # Create multiple test files
            gz_files = []
            for i in range(3):
                gz_file = env.create_test_gzip_file(f"file_{i}/test_{i}.pfw.gz", bytes_per_line=512)
                gz_files.append(gz_file)

            def create_and_build_indexer(gz_file):
                """Helper function to create and build an indexer"""
                try:
                    with NativeIndexer(gz_file, checkpoint_size=256 * 1024) as indexer:
                        if indexer.need_rebuild():
                            indexer.build()
                        return {
                            "file": gz_file,
                            "max_bytes": indexer.get_max_bytes(),
                            "num_lines": indexer.get_num_lines(),
                            "success": True,
                        }
                except Exception as e:
                    return {"file": gz_file, "error": str(e), "success": False}

            # Use Dask delayed for parallel processing
            delayed_tasks = [
                dask.delayed(create_and_build_indexer)(gz_file) for gz_file in gz_files
            ]
            results = dask.compute(*delayed_tasks)

            # Verify all indexers were created successfully
            assert len(results) == 3
            for result in results:
                assert result["success"]
                assert result["max_bytes"] > 0
                assert result["num_lines"] > 0

                # Verify index store was created
                index_path = env.get_index_path(result["file"])
                assert os.path.exists(index_path)


@pytest.mark.skipif(not DASK_AVAILABLE, reason="Dask not available")
class TestDirectoryIndexerWithDask:
    """Tests for the directory-level Indexer API with Dask."""

    def test_directory_indexer_indexes_all_files(self):
        """Test that directory-level Indexer indexes all files in a directory."""
        with Environment(lines=100) as env:
            # Create multiple test files in the same directory
            gz_files = []
            for i in range(3):
                gz_file = env.create_test_gzip_file(f"test_{i}.pfw.gz", bytes_per_line=256)
                gz_files.append(gz_file)

            # Use directory-level Indexer
            indexer = dftu_utils.Indexer(env.temp_dir)

            # Check status before build
            before = indexer.resolve()
            assert before.total_files == 3
            assert len(before.needs_work) == 3
            assert len(before.ready) == 0

            # Build indexes
            indexer.build()

            # Check status after build
            after = indexer.resolve()
            assert after.total_files == 3
            assert len(after.ready) == 3
            assert len(after.needs_work) == 0

    def test_directory_indexer_ensure_indexed_idempotent(self):
        """Test that ensure_indexed is idempotent - calling multiple times is safe."""
        with Environment(lines=50) as env:
            env.create_test_gzip_file()

            indexer = dftu_utils.Indexer(env.temp_dir)

            # First call builds the index
            status1 = indexer.ensure_indexed()
            assert len(status1.ready) == 1

            # Second call should find everything already indexed
            status2 = indexer.ensure_indexed()
            assert len(status2.ready) == 1
            assert len(status2.needs_work) == 0


@pytest.mark.skipif(not DASK_AVAILABLE, reason="Dask not available")
class TestDistributedWriteTrace:
    """distributed_write_trace fans TraceViewer.export across dask workers."""

    def test_writes_reindexable_shards(self):
        from dask.distributed import Client, LocalCluster

        from dftracer.utils.dask import distributed_write_trace

        with Environment(lines=200) as env:
            files = [env.create_test_gzip_file(f"f{k}/t{k}.pfw.gz") for k in range(3)]
            with dftu_utils.Indexer(files=files, index_dir=env.temp_dir) as ix:
                ix.ensure_indexed()

            cluster = LocalCluster(processes=False, n_workers=2, threads_per_worker=2)
            client = Client(cluster)
            try:
                out = os.path.join(env.temp_dir, "shards")
                res = distributed_write_trace(
                    files, out, index_dir=env.temp_dir, files_per_task=1, client=client
                )
                assert len(res["files"]) == 3
                # Each shard is a re-indexable .pfw.gz trace.
                reidx = os.path.join(env.temp_dir, "reidx")
                with dftu_utils.Indexer(files=res["files"], index_dir=reidx) as ix2:
                    ix2.ensure_indexed()
                total = sum(
                    dftu_utils.TraceViewer(f, index_path=reidx).statistics()["duration_count"]
                    for f in res["files"]
                )
                assert total > 0
            finally:
                client.close()
                cluster.close()


@pytest.mark.skipif(not DASK_AVAILABLE, reason="Dask not available")
class TestDaskTraceViewer:
    """Distributed TraceViewer: collect via partials, cursor pages, write."""

    def _cluster_files(self, env):
        files = [env.create_test_gzip_file(f"f{k}/t{k}.pfw.gz") for k in range(4)]
        with dftu_utils.Indexer(files=files, index_dir=env.temp_dir) as ix:
            ix.ensure_indexed()
        return files

    def test_collect_matches_single_node(self):
        import pyarrow as pa
        from dask.distributed import Client, LocalCluster

        from dftracer.utils import TraceViewer
        from dftracer.utils.dask import DaskTraceViewer

        with Environment(lines=200) as env:
            files = self._cluster_files(env)
            cluster = LocalCluster(processes=False, n_workers=2, threads_per_worker=2)
            client = Client(cluster)
            try:
                dist = pa.table(
                    DaskTraceViewer(files, env.temp_dir, client=client)
                    .group_by("cat")
                    .agg("count", "mean:dur", "std:dur")
                    .collect()
                ).sort_by("cat")
                whole = pa.table(
                    TraceViewer(files, index_path=env.temp_dir)
                    .group_by("cat")
                    .agg("count", "mean:dur", "std:dur")
                    .collect()
                    .collect()
                ).sort_by("cat")
                assert dist.to_pandas().round(4).equals(whole.to_pandas().round(4))
            finally:
                client.close()
                cluster.close()

    def test_flamegraph_matches_single_node(self, tmp_path):
        import gzip
        import json

        from dask.distributed import Client, LocalCluster

        from dftracer.utils import TraceViewer
        from dftracer.utils.dask import DaskTraceViewer

        # One file per pid so each (pid,tid) lane lives entirely on one shard.
        files = []
        for pid in (1, 2):
            p = str(tmp_path / f"p{pid}.pfw.gz")
            rows = [
                {"ph": "X", "name": n, "cat": "c", "pid": pid, "tid": 1, "ts": t, "dur": d}
                for n, t, d in [("A", 0, 100), ("B", 10, 30), ("C", 15, 10)]
            ]
            with gzip.open(p, "wt") as f:
                f.write("\n".join(json.dumps(r) for r in rows) + "\n")
            files.append(p)
        with dftu_utils.Indexer(files=files, index_dir=str(tmp_path)) as ix:
            ix.ensure_indexed()

        cluster = LocalCluster(processes=False, n_workers=2, threads_per_worker=2)
        client = Client(cluster)
        try:
            dist = (
                DaskTraceViewer(files, str(tmp_path), client=client)
                .flamegraph()
                .to_arrow()
                .to_pydict()
            )
            whole = TraceViewer(files, index_path=str(tmp_path)).flamegraph().to_arrow().to_pydict()

            def totals(d):
                return {n: d["total"][i] for i, n in enumerate(d["name"])}

            assert totals(dist) == totals(whole)
            # A folds across both pid lanes: 100 + 100.
            assert totals(dist)["A"] == 200
        finally:
            client.close()
            cluster.close()

    def test_materialize_row_view_distributed(self, tmp_path):
        import pyarrow as pa
        from dask.distributed import Client, LocalCluster

        from dftracer.utils import TraceViewer
        from dftracer.utils.dask import DaskTraceViewer

        with Environment(lines=200) as env:
            files = self._cluster_files(env)
            mv_root = str(tmp_path / "mv")

            def full_count():
                tv = (
                    TraceViewer(files, index_path=env.temp_dir)
                    .filter('cat == "STDIO"')
                    .views_root(mv_root)
                )
                return sum(pa.record_batch(c).num_rows for c in tv.stream(batch_size=128))

            cluster = LocalCluster(processes=False, n_workers=2, threads_per_worker=2)
            client = Client(cluster)
            try:
                base = full_count()
                assert base > 0

                # Each shard materializes its own subdir; coordinator writes one
                # manifest over the full base set.
                DaskTraceViewer(files, env.temp_dir, client=client, files_per_task=1).filter(
                    'cat == "STDIO"'
                ).views_root(mv_root).materialize()

                parts = [
                    f for _root, _dirs, fs in os.walk(mv_root) for f in fs if f.endswith(".pfw.gz")
                ]
                assert len(parts) >= 2, "expected one part per shard"

                # A matching full read returns the same events (served from parts).
                assert full_count() == base
            finally:
                client.close()
                cluster.close()

    def test_pages_cover_all_events_once(self, tmp_path):
        import gzip

        from dask.distributed import Client, LocalCluster

        from dftracer.utils.dask import DaskTraceViewer

        # Disjoint, globally-unique ts per file so cursor paging has no ties.
        files = []
        for k in range(4):
            p = str(tmp_path / f"t{k}.pfw.gz")
            with gzip.open(p, "wt") as f:
                for i in range(250):
                    f.write(
                        '{"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,'
                        '"ts":%d,"dur":%d,"args":{}}\n' % (1000 + k * 1000 + i, 10 + i)
                    )
            files.append(p)
        with dftu_utils.Indexer(files=files, index_dir=str(tmp_path)) as ix:
            ix.ensure_indexed()

        cluster = LocalCluster(processes=False, n_workers=2, threads_per_worker=2)
        client = Client(cluster)
        try:
            dv = DaskTraceViewer(files, str(tmp_path), client=client).select("ts", "dur")
            seen = []
            for tbl in dv.pages(page_size=97):
                seen.extend(tbl.column("ts").to_pylist())
            assert len(seen) == 1000  # all events, once
            assert len(set(seen)) == 1000  # no page overlap
        finally:
            client.close()
            cluster.close()

    def test_collect_cache_matches_and_reuses(self, tmp_path):
        import pyarrow as pa
        from dask.distributed import Client, LocalCluster

        from dftracer.utils.dask import DaskTraceViewer

        with Environment(lines=200) as env:
            files = self._cluster_files(env)
            cluster = LocalCluster(processes=False, n_workers=2, threads_per_worker=2)
            client = Client(cluster)
            try:

                def agg():
                    return (
                        DaskTraceViewer(files, env.temp_dir, client=client)
                        .rollup_root(str(tmp_path / "vc"))
                        .group_by("cat")
                        .agg("count", "mean:dur")
                    )

                plain = pa.table(agg().collect()).sort_by("cat")
                assert agg().reconstruct_if_cached() is None  # cold
                agg().materialize()  # build the rollup MV
                warm = agg().reconstruct_if_cached()
                assert warm is not None  # read it back, no scan
                assert (
                    pa.table(warm)
                    .sort_by("cat")
                    .to_pandas()
                    .round(6)
                    .equals(plain.to_pandas().round(6))
                )
            finally:
                client.close()
                cluster.close()

    def test_cache_terminals_gated_to_aggregation(self):
        from dftracer.utils.dask import DaskAggregatedTraceViewer, DaskTraceViewer

        with Environment(lines=50) as env:
            files = self._cluster_files(env)
            base = DaskTraceViewer(files, env.temp_dir)
            # Raw viewer has no cache terminals; group_by promotes to the type
            # that does.
            assert not hasattr(base, "reconstruct_if_cached")
            assert not hasattr(base.select("ts"), "reconstruct_if_cached")
            assert isinstance(base.group_by("cat"), DaskAggregatedTraceViewer)
            assert isinstance(base.agg("count"), DaskAggregatedTraceViewer)
            # An aggregated viewer stays aggregated through further ops.
            assert isinstance(base.group_by("cat").time_range(0, 1), DaskAggregatedTraceViewer)


if __name__ == "__main__":
    pytest.main([__file__])
