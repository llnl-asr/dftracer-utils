#!/usr/bin/env python3
"""
Test cases for DFTracer indexer Python bindings
"""

import glob
import os

import pytest

import dftracer.utils as dftu_utils
from dftracer.utils.dftracer_utils_ext import CheckpointIndexer as NativeIndexer

from .common import Environment, valgrind_scale


class TestCheckpointIndexer:
    """Test cases for checkpoint-level indexer operations via get_checkpoint_indexer"""

    def test_checkpoint_indexer_creation(self):
        """Test checkpoint indexer creation via Indexer.get_checkpoint_indexer"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()

            with dftu_utils.Indexer(files=[gz_file]) as indexer:
                indexer.ensure_indexed()
                cp_indexer = indexer.get_checkpoint_indexer(gz_file)

                assert cp_indexer.gz_path == gz_file
                assert cp_indexer.checkpoint_size > 0

    def test_checkpoint_indexer_file_info(self):
        """Test checkpoint indexer file information methods"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()

            with dftu_utils.Indexer(files=[gz_file]) as indexer:
                indexer.ensure_indexed()
                cp_indexer = indexer.get_checkpoint_indexer(gz_file)

                max_bytes = cp_indexer.get_max_bytes()
                num_lines = cp_indexer.get_num_lines()

                assert isinstance(max_bytes, int)
                assert isinstance(num_lines, int)
                assert max_bytes > 0
                assert num_lines > 0

    @pytest.mark.valgrind
    def test_checkpoint_indexer_checkpoints(self):
        """Test checkpoint indexer checkpoint functionality"""
        with Environment(lines=valgrind_scale(100000, 100)) as env:
            gz_file = env.create_test_gzip_file()
            checkpoint_size = 256 * 1024  # 256KB

            with dftu_utils.Indexer(
                files=[gz_file],
                checkpoint_size=checkpoint_size,
            ) as indexer:
                indexer.ensure_indexed()
                cp_indexer = indexer.get_checkpoint_indexer(gz_file)

                max_bytes = cp_indexer.get_max_bytes()
                num_lines = cp_indexer.get_num_lines()
                print(
                    f"File stats: {max_bytes} bytes, {num_lines} lines, "
                    f"checkpoint_size={checkpoint_size}"
                )

                assert isinstance(max_bytes, int) and max_bytes > 0
                assert isinstance(num_lines, int) and num_lines > 0

    def test_changed_checkpoint_size_triggers_rebuild(self):
        """resolve() reports work when the requested checkpoint size differs."""
        with Environment(lines=200) as env:
            gz = env.create_test_gzip_file()
            idx = env.temp_dir

            with dftu_utils.Indexer(files=[gz], index_dir=idx, checkpoint_size="512KB") as ix:
                ix.ensure_indexed()

            # Same size: cached, no work.
            with dftu_utils.Indexer(files=[gz], index_dir=idx, checkpoint_size="512KB") as ix:
                assert ix.resolve().needs_work == []

            # Different size: rebuild is required, then settles.
            with dftu_utils.Indexer(files=[gz], index_dir=idx, checkpoint_size="1MB") as ix:
                assert gz in ix.resolve().needs_work
                ix.ensure_indexed()
                assert ix.resolve().needs_work == []


class TestNativeIndexerDirect:
    """Test native Indexer class directly for low-level operations"""

    def test_native_indexer_creation(self):
        """Test native indexer creation"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)

            with NativeIndexer(gz_file, index_path) as indexer:
                assert indexer.gz_path == gz_file
                assert indexer.index_path == index_path
                assert indexer.checkpoint_size > 0

    def test_native_indexer_build_and_rebuild(self):
        """Test native indexer build and rebuild functionality"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)

            with NativeIndexer(gz_file, index_path) as indexer:
                assert indexer.need_rebuild()
                indexer.build()
                assert os.path.exists(index_path)
                assert not indexer.need_rebuild()

            with NativeIndexer(gz_file, index_path, force_rebuild=True) as indexer_force:
                assert not indexer_force.need_rebuild()
                indexer_force.build()

    def test_native_indexer_nonexistent_file(self):
        """Test native indexer creation with non-existent file"""
        with pytest.raises(RuntimeError):
            NativeIndexer("nonexistent_file.gz")

    @pytest.mark.valgrind
    def test_native_indexer_build_bloom(self):
        """Test building with bloom=True"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)
            with NativeIndexer(gz_file, index_path, build_bloom=True) as indexer:
                indexer.build()
                assert indexer.has_bloom


class TestCheckpointIndexerIntegration:
    """Integration tests for checkpoint indexer with reader"""

    def test_checkpoint_indexer_with_viewer_creation(self):
        """A TraceViewer reads back the events from a freshly built index."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()

            with dftu_utils.Indexer(files=[gz_file]) as indexer:
                indexer.ensure_indexed()

                viewer = dftu_utils.TraceViewer(gz_file)
                assert viewer.statistics()["duration_count"] > 0

    @pytest.mark.valgrind
    def test_multiple_viewers_same_index(self):
        """Multiple viewers over one index agree on the event count."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()

            with dftu_utils.Indexer(files=[gz_file]) as indexer:
                indexer.ensure_indexed()

                counts = [
                    dftu_utils.TraceViewer(gz_file).statistics()["duration_count"] for _ in range(3)
                ]
                assert counts[0] > 0
                assert all(c == counts[0] for c in counts)


class TestCheckpointIndexerLifetime:
    """Test checkpoint indexer lifetime management"""

    def test_indexer_close_releases_wrapper_not_index_store(self):
        """close() should release the Python handle without deleting .dftindex."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)

            indexer = NativeIndexer(gz_file, index_path)
            assert indexer.need_rebuild()
            indexer.build()
            assert os.path.exists(index_path)

            indexer.close()
            assert os.path.exists(index_path)

            with NativeIndexer(gz_file, index_path) as reopened:
                assert not reopened.need_rebuild()
                assert reopened.get_num_lines() > 0

    def test_indexer_context_exit_keeps_shared_index_store(self):
        """Context exit should not tear down the shared index store."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)

            with NativeIndexer(gz_file, index_path) as indexer:
                if indexer.need_rebuild():
                    indexer.build()
                assert indexer.get_num_lines() > 0

            assert os.path.exists(index_path)

            viewer = dftu_utils.TraceViewer(gz_file)
            assert viewer.statistics()["duration_count"] > 0


class TestDirectoryIndexer:
    """Test cases for the directory-level Indexer API"""

    def test_indexer_creation(self):
        """Test directory indexer creation"""
        with Environment() as env:
            env.create_test_gzip_file()
            env.create_test_gzip_file()

            indexer = dftu_utils.Indexer(env.temp_dir)
            assert indexer is not None

    def test_indexer_context_manager(self):
        """Test directory indexer as context manager"""
        with Environment() as env:
            env.create_test_gzip_file()

            with dftu_utils.Indexer(env.temp_dir) as indexer:
                assert indexer is not None

    def test_indexer_resolve(self):
        """Test resolve() returns IndexStatus"""
        with Environment() as env:
            env.create_test_gzip_file()

            with dftu_utils.Indexer(env.temp_dir) as indexer:
                status = indexer.resolve()
                assert isinstance(status, dftu_utils.IndexStatus)
                assert status.total_files >= 1
                assert len(status.needs_work) >= 1

    def test_indexer_build(self):
        """Test build() creates indexes"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            print(f"\nCreated test file: {gz_file}")
            print(f"Directory: {env.temp_dir}")
            print(f"Files in dir: {os.listdir(env.temp_dir)}")

            with dftu_utils.Indexer(env.temp_dir) as indexer:
                status_before = indexer.resolve()
                print(f"Before build: {status_before}")
                assert len(status_before.needs_work) >= 1
                assert status_before.index_path != ""

                indexer.build()

                assert os.path.isdir(status_before.index_path), (
                    f"Index dir not created: {status_before.index_path}"
                )
                print(f"Index dir contents: {os.listdir(status_before.index_path)}")

                status_after = indexer.resolve()
                print(f"After build: {status_after}")
                assert len(status_after.ready) >= 1, (
                    f"Expected ready>=1, got {len(status_after.ready)}"
                )

    def test_indexer_ensure_indexed(self):
        """Test ensure_indexed() builds if needed"""
        with Environment() as env:
            env.create_test_gzip_file()

            with dftu_utils.Indexer(env.temp_dir) as indexer:
                status = indexer.ensure_indexed()
                assert isinstance(status, dftu_utils.IndexStatus)
                assert len(status.ready) >= 1

    def test_indexer_with_require_bloom(self):
        """Test indexer with bloom filter requirement"""
        with Environment() as env:
            env.create_test_gzip_file()

            with dftu_utils.Indexer(env.temp_dir, require_bloom=True) as indexer:
                status = indexer.ensure_indexed()
                assert len(status.ready) >= 1

    def test_indexer_with_aggregation_config(self):
        """Test indexer with aggregation config"""
        with Environment() as env:
            env.create_test_gzip_file()

            agg_config = dftu_utils.AggregationConfig(
                time_interval_ms=1000.0,
                compute_percentiles=False,
            )
            with dftu_utils.Indexer(
                env.temp_dir,
                require_aggregation=agg_config,
            ) as indexer:
                assert indexer.aggregation_config is not None
                assert indexer.aggregation_config.time_interval_ms == 1000.0

    @pytest.mark.valgrind
    def test_indexer_aggregation_true(self):
        """Test indexer with require_aggregation=True uses defaults"""
        with Environment() as env:
            env.create_test_gzip_file()

            with dftu_utils.Indexer(
                env.temp_dir,
                require_aggregation=True,
            ) as indexer:
                assert indexer.aggregation_config is not None
                assert indexer.aggregation_config.time_interval_ms == 5000.0

    def test_index_status_dataclass(self):
        """Test IndexStatus dataclass"""
        status = dftu_utils.IndexStatus(
            total_files=5,
            ready=["a.pfw.gz", "b.pfw.gz"],
            needs_work=["c.pfw.gz"],
            index_path="/tmp/index",
        )
        assert status.total_files == 5
        assert len(status.ready) == 2
        assert len(status.needs_work) == 1
        assert status.index_path == "/tmp/index"

    def test_resolve_reports_aggregation_interval(self):
        """resolve() surfaces the time interval of the cached aggregation tier."""
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            with dftu_utils.Indexer(
                directory=directory,
                require_aggregation=dftu_utils.AggregationConfig(time_interval_ms=5000),
            ) as indexer:
                indexer.ensure_indexed()
                assert indexer.resolve().aggregation_interval_us == 5_000_000

    def test_ensure_indexed_rebuilds_on_interval_change(self):
        """A new interval discards the stale aggregation tier and rebuilds it."""
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1, 2])
            with dftu_utils.Indexer(
                directory=directory,
                require_aggregation=dftu_utils.AggregationConfig(time_interval_ms=1000),
            ) as indexer:
                indexer.ensure_indexed()
                status = indexer.resolve()
                assert status.aggregation_interval_us == 1_000_000
                files = sorted(glob.glob(os.path.join(directory, "*.pfw.gz")))
                tbl = (
                    dftu_utils.TraceViewer(files, index_path=directory)
                    .group_by("name")
                    .agg("count")
                    .collect()
                    .collect()
                )
                assert tbl is not None and pa.table(tbl).num_rows > 0

    def test_aggregation_config_dataclass(self):
        """Test AggregationConfig dataclass"""
        config = dftu_utils.AggregationConfig(
            time_interval_ms=2000.0,
            group_keys=["host", "rank"],
            custom_metric_fields=["bytes"],
            compute_percentiles=True,
        )
        assert config.time_interval_ms == 2000.0
        assert config.group_keys == ["host", "rank"]
        assert config.custom_metric_fields == ["bytes"]
        assert config.compute_percentiles is True

    def test_indexer_with_files_list(self):
        """Test indexer with explicit file list instead of directory"""
        with Environment() as env:
            file_path = env.create_test_gzip_file()

            with dftu_utils.Indexer(
                files=[file_path],
                index_dir=env.temp_dir,
            ) as indexer:
                status = indexer.resolve()
                assert status.total_files == 1

    def test_indexer_files_and_directory(self):
        """Test indexer with both files and directory (files take precedence)"""
        with Environment() as env:
            file_path = env.create_test_gzip_file()

            with dftu_utils.Indexer(
                directory=env.temp_dir,
                files=[file_path],
            ) as indexer:
                status = indexer.resolve()
                assert status.total_files >= 1

    def test_indexer_requires_directory_or_files(self):
        """Test that indexer requires at least directory or files"""
        with pytest.raises(ValueError, match="directory.*files"):
            dftu_utils.Indexer()

    def test_indexer_get_checkpoint_indexer(self):
        """Test get_checkpoint_indexer returns working checkpoint indexer"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()

            with dftu_utils.Indexer(env.temp_dir) as indexer:
                indexer.ensure_indexed()

                cp_indexer = indexer.get_checkpoint_indexer(gz_file)

                assert cp_indexer.gz_path == gz_file
                assert cp_indexer.get_max_bytes() > 0
                assert cp_indexer.get_num_lines() > 0

    def test_indexer_get_checkpoint_indexer_uses_index_dir(self):
        """Test that get_checkpoint_indexer uses the same index_dir"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            custom_index_dir = os.path.join(env.temp_dir, "custom_index")
            os.makedirs(custom_index_dir, exist_ok=True)

            with dftu_utils.Indexer(
                env.temp_dir,
                index_dir=custom_index_dir,
            ) as indexer:
                indexer.ensure_indexed()

                cp_indexer = indexer.get_checkpoint_indexer(gz_file)
                assert custom_index_dir in cp_indexer.index_path


class TestIndexerDfanalyzerAPIs:
    """Test cases for dfanalyzer integration APIs (hash tables, PIDs)"""

    def test_get_hash_table_file(self):
        """Test get_hash_table returns file hash mappings"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dftu_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
            ) as indexer:
                indexer.ensure_indexed()

                file_hashes = indexer.get_hash_table("file")
                assert isinstance(file_hashes, dict)

    def test_get_hash_table_host(self):
        """Test get_hash_table returns host hash mappings"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dftu_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
            ) as indexer:
                indexer.ensure_indexed()

                host_hashes = indexer.get_hash_table("host")
                assert isinstance(host_hashes, dict)

    def test_get_hash_table_string(self):
        """Test get_hash_table returns string hash mappings"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dftu_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
            ) as indexer:
                indexer.ensure_indexed()

                string_hashes = indexer.get_hash_table("string")
                assert isinstance(string_hashes, dict)

    def test_get_hash_table_invalid_type(self):
        """Test get_hash_table raises error for invalid type"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dftu_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
            ) as indexer:
                indexer.ensure_indexed()

                with pytest.raises((ValueError, RuntimeError)):
                    indexer.get_hash_table("invalid_type")

    def test_query_file_pids(self):
        """Test query_file_pids returns set of PIDs for a file"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dftu_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
            ) as indexer:
                indexer.ensure_indexed()

                # File ID 1 is typically the first indexed file
                pids = indexer.query_file_pids(1)
                assert isinstance(pids, set)
                # PIDs should be integers
                for pid in pids:
                    assert isinstance(pid, int)

    def test_query_file_pids_nonexistent(self):
        """Test query_file_pids returns empty set for nonexistent file"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dftu_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
            ) as indexer:
                indexer.ensure_indexed()

                pids = indexer.query_file_pids(9999)
                assert isinstance(pids, set)
                assert len(pids) == 0

    def test_query_all_file_pids(self):
        """Test query_all_file_pids returns dict mapping file_id to PID sets"""
        with Environment() as env:
            gz_file1 = env.create_dft_trace_file(filename="trace1.pfw.gz")
            gz_file2 = env.create_dft_trace_file(filename="trace2.pfw.gz")

            with dftu_utils.Indexer(
                files=[gz_file1, gz_file2],
                require_bloom=True,
            ) as indexer:
                indexer.ensure_indexed()

                all_pids = indexer.query_all_file_pids()
                assert isinstance(all_pids, dict)

                for file_id, pid_set in all_pids.items():
                    assert isinstance(file_id, int)
                    assert isinstance(pid_set, set)
                    for pid in pid_set:
                        assert isinstance(pid, int)

    def test_query_all_file_pids_empty_index(self):
        """Test query_all_file_pids returns empty dict for unindexed files"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()

            with dftu_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
            ) as indexer:
                indexer.ensure_indexed()

                all_pids = indexer.query_all_file_pids()
                assert isinstance(all_pids, dict)

    @pytest.mark.valgrind
    def test_integration_hash_tables_and_pids(self):
        """Integration test: hash tables and PIDs work together"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dftu_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
            ) as indexer:
                indexer.ensure_indexed()

                # Get hash tables
                file_hashes = indexer.get_hash_table("file")
                host_hashes = indexer.get_hash_table("host")

                # Get PIDs
                all_pids = indexer.query_all_file_pids()

                # Both should be populated for a valid DFT trace
                assert isinstance(file_hashes, dict)
                assert isinstance(host_hashes, dict)
                assert isinstance(all_pids, dict)


class TestAggTierQueryFilter:
    """Query-filter semantics on the aggregation-tier typed read (the View's
    filter over the AGG CF, which replaced the indexer's iter_arrow scan)."""

    def _viewer(self, directory, query=None):
        files = sorted(glob.glob(os.path.join(directory, "*.pfw.gz")))
        tv = dftu_utils.TraceViewer(files, index_path=directory)
        if query:
            tv = tv.filter(query)
        return tv.group_by("name", "pid").agg("count")

    def _event_count(self, directory, pa, query=None):
        reg = self._viewer(directory, query).collect_typed()["regular"]
        if reg is None:
            return 0
        t = pa.table(reg)
        if "count" not in t.column_names:
            return 0
        return int(pa.compute.sum(t["count"]).as_py() or 0)

    def test_no_query(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            assert self._event_count(directory, pa) > 0

    @pytest.mark.valgrind
    def test_pid_filter(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            assert self._event_count(directory, pa, "pid == 1") > 0

    def test_pid_filter_reduces_rows(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1, 2])
            all_rows = self._event_count(directory, pa)
            filtered = self._event_count(directory, pa, "pid == 1")
            assert 0 < filtered < all_rows

    def test_invalid_query(self):
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            with pytest.raises((ValueError, RuntimeError)):
                self._viewer(directory, "invalid ==").collect_typed()

    def test_multi_pid_filter(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[10, 20, 30])
            filtered = self._event_count(directory, pa, "pid == 10 or pid == 20")
            all_rows = self._event_count(directory, pa)
            assert 0 < filtered < all_rows

    def test_string_filter(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            assert self._event_count(directory, pa, 'name == "read"') > 0

    def test_no_match(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            assert self._event_count(directory, pa, "pid == 999999") == 0


class TestCollectTypedRawFallback:
    """collect_typed on a query the aggregation tier cannot key on (e.g. a ts
    predicate) falls back to a raw scan and returns per-event rows, not a silent
    empty result - consistent with the aggregate path over the same filter."""

    def _viewer(self, directory):
        files = sorted(glob.glob(os.path.join(directory, "*.pfw.gz")))
        return dftu_utils.TraceViewer(files, index_path=directory)

    def _agg_total(self, pa, base):
        t = pa.table(base.group_by("name").agg("count").collect().collect())
        return int(pa.compute.sum(t["count"]).as_py()) if t.num_rows else 0

    def test_ts_filter_returns_raw_rows(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1, 2])
            base = self._viewer(directory).filter("ts >= 0")
            reg = pa.table(base.collect_typed()["regular"])
            for c in ("ts", "dur", "ph", "name", "cat", "pid", "tid"):
                assert c in reg.column_names
            assert reg.num_rows > 0
            assert reg.num_rows == self._agg_total(pa, base)

    def test_pid_filter_reduces_raw_rows(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1, 2])
            tv = self._viewer(directory)
            all_rows = pa.table(tv.filter("ts >= 0").collect_typed()["regular"]).num_rows
            one = pa.table(tv.filter("ts >= 0 and pid == 1").collect_typed()["regular"]).num_rows
            assert 0 < one < all_rows


class TestOccupancyMetrics:
    """busy/concurrency/utilization via the time-window reduction: each bucket is
    a 64-sub-slot coverage mask, an event ORs the sub-slots it covers, and busy
    is popcount * bucket/64 summed over buckets (the interval union to bucket/64
    resolution). Bounded by bucket count and OR-mergeable, so it streams over big
    traces and across shards."""

    def _index(self, env, lines, interval_ms=1000):
        import gzip

        from dftracer.utils import AggregationConfig, Indexer

        p = os.path.join(env.temp_dir, "occ.pfw.gz")
        with gzip.open(p, "wt") as f:
            f.write("\n".join(lines) + "\n")
        Indexer(
            files=[p],
            require_aggregation=AggregationConfig(time_interval_ms=interval_ms),
            force_rebuild=True,
        ).ensure_indexed()
        return p

    def _index_files(self, env, per_file, interval_ms=1000):
        import gzip

        from dftracer.utils import AggregationConfig, Indexer

        paths = []
        for i, lines in enumerate(per_file):
            p = os.path.join(env.temp_dir, f"occ_{i}.pfw.gz")
            with gzip.open(p, "wt") as f:
                f.write("\n".join(lines) + "\n")
            paths.append(p)
        Indexer(
            files=paths,
            require_aggregation=AggregationConfig(time_interval_ms=interval_ms),
            force_rebuild=True,
        ).ensure_indexed()
        return paths

    def test_concurrent_caps_at_window(self):
        # Two events fully overlapping [0, 500000) on two threads: sum(dur) is
        # 1000000, but only 500000 of wall-clock passed, so busy caps at the
        # window and concurrency = sum / busy = 2.
        pa = pytest.importorskip("pyarrow")
        lines = [
            '{"name":"a","cat":"c","pid":1,"tid":1,"ts":0,"dur":500000,'
            '"ph":"X","args":{"hhash":"aa"}}',
            '{"name":"a","cat":"c","pid":1,"tid":2,"ts":0,"dur":500000,'
            '"ph":"X","args":{"hhash":"aa"}}',
        ]
        with Environment() as env:
            p = self._index(env, lines)
            tv = dftu_utils.TraceViewer([p], index_path=env.temp_dir)
            t = pa.table(
                tv.time_range(0, 500_000)
                .filter('cat == "c" and ts < 500000')
                .group_by("cat")
                .agg("busy", "concurrency", "sum:dur")
                .collect()
                .collect()
            )
            assert t["sum_dur"][0].as_py() == 1_000_000
            assert t["busy"][0].as_py() == 500_000  # capped at the window
            assert abs(t["concurrency"][0].as_py() - 2.0) < 1e-9

    def test_serial_fills_window(self):
        # Two back-to-back events covering the whole window: no overlap, so busy
        # equals the window and concurrency = 1.
        pa = pytest.importorskip("pyarrow")
        lines = [
            '{"name":"a","cat":"c","pid":1,"tid":1,"ts":0,"dur":500000,'
            '"ph":"X","args":{"hhash":"aa"}}',
            '{"name":"a","cat":"c","pid":1,"tid":1,"ts":500000,"dur":500000,'
            '"ph":"X","args":{"hhash":"aa"}}',
        ]
        with Environment() as env:
            p = self._index(env, lines)
            tv = dftu_utils.TraceViewer([p], index_path=env.temp_dir)
            t = pa.table(
                tv.time_range(0, 1_000_000)
                .filter('cat == "c" and ts < 1000000')
                .group_by("cat")
                .agg("busy", "concurrency", "sum:dur")
                .collect()
                .collect()
            )
            assert t["sum_dur"][0].as_py() == 1_000_000
            assert t["busy"][0].as_py() == 1_000_000
            assert abs(t["concurrency"][0].as_py() - 1.0) < 1e-9

    def test_windowed_and_filtered(self):
        pa = pytest.importorskip("pyarrow")
        lines = [
            '{"name":"a","cat":"c","pid":1,"tid":1,"ts":0,"dur":500000,'
            '"ph":"X","args":{"hhash":"aa"}}',
            '{"name":"b","cat":"c","pid":1,"tid":2,"ts":250000,"dur":500000,'
            '"ph":"X","args":{"hhash":"aa"}}',
        ]
        with Environment() as env:
            p = self._index(env, lines)
            tv = dftu_utils.TraceViewer([p], index_path=env.temp_dir)
            # Window excludes A (ts=0) and keeps only B [250000, 750000): busy is
            # B's duration, uncapped since it fits the window.
            win = pa.table(
                tv.time_range(100_000, 900_000)
                .filter('cat == "c" and ts >= 100000')
                .group_by("cat")
                .agg("busy", "count")
                .collect()
                .collect()
            )
            assert win["count"][0].as_py() == 1
            assert win["busy"][0].as_py() == 500_000

    def test_merges_across_files(self):
        # Two events overlapping the same window but in SEPARATE files: occupancy
        # must OR-merge across sources to the union (500000), not sum to 1000000.
        # This is the same merge_accum path shards/MPI ranks combine through, so
        # it guards the distributed merge, not just a single scan.
        pa = pytest.importorskip("pyarrow")
        f0 = [
            '{"name":"a","cat":"c","pid":1,"tid":1,"ts":0,"dur":500000,'
            '"ph":"X","args":{"hhash":"aa"}}'
        ]
        f1 = [
            '{"name":"a","cat":"c","pid":2,"tid":1,"ts":0,"dur":500000,'
            '"ph":"X","args":{"hhash":"aa"}}'
        ]
        with Environment() as env:
            paths = self._index_files(env, [f0, f1])
            tv = dftu_utils.TraceViewer(paths, index_path=env.temp_dir)
            t = pa.table(
                tv.time_range(0, 500_000)
                .filter('cat == "c" and ts < 500000')
                .group_by("cat")
                .agg("busy", "concurrency", "sum:dur")
                .collect()
                .collect()
            )
            assert t["sum_dur"][0].as_py() == 1_000_000
            assert t["busy"][0].as_py() == 500_000  # union across files, OR-merged
            assert abs(t["concurrency"][0].as_py() - 2.0) < 1e-9

    def test_active_peak_concurrency(self):
        # 3 events overlapping the window -> peak concurrent headcount 3.
        pa = pytest.importorskip("pyarrow")
        lines = [
            '{"name":"a","cat":"c","pid":1,"tid":%d,"ts":0,"dur":100000,'
            '"ph":"X","args":{"hhash":"aa"}}' % tid
            for tid in (1, 2, 3)
        ]
        with Environment() as env:
            p = self._index(env, lines)
            tv = dftu_utils.TraceViewer([p], index_path=env.temp_dir)
            t = pa.table(
                tv.time_range(0, 100_000)
                .filter('cat == "c" and ts < 100000')
                .group_by("cat")
                .agg("active", "count")
                .collect()
                .collect()
            )
            assert t["count"][0].as_py() == 3
            assert t["active"][0].as_py() == 3  # peak concurrent

    def test_partial_transport_round_trip(self):
        # The distributed wire: each "rank" viewer serializes a partial via
        # aggregate_partial(); the coordinator merges them with
        # merge_partials_to_table(). Occupancy must survive that serialization
        # and OR-merge to the union - if the partial dropped the masks, busy
        # would come back 0.
        pa = pytest.importorskip("pyarrow")
        f0 = [
            '{"name":"a","cat":"c","pid":1,"tid":1,"ts":0,"dur":500000,'
            '"ph":"X","args":{"hhash":"aa"}}'
        ]
        f1 = [
            '{"name":"a","cat":"c","pid":2,"tid":1,"ts":0,"dur":500000,'
            '"ph":"X","args":{"hhash":"aa"}}'
        ]
        with Environment() as env:
            p0, p1 = self._index_files(env, [f0, f1])

            def partial(path):
                v = (
                    dftu_utils.TraceViewer([path], index_path=env.temp_dir)
                    .filter('cat == "c"')
                    .group_by("cat")
                    .agg("busy", "concurrency", "sum:dur")
                )
                return v, v.aggregate_partial()

            v0, b0 = partial(p0)
            _, b1 = partial(p1)
            t = pa.table(v0.merge_partials_to_table([b0, b1]))
            assert t["sum_dur"][0].as_py() == 1_000_000
            assert t["busy"][0].as_py() == 500_000  # union survived the wire
            assert abs(t["concurrency"][0].as_py() - 2.0) < 1e-9


class TestShardPartitionCompleteness:
    """Disjoint shard ranges must union to the full scan - no dropped or
    double-counted events. Regression guard for the multi-worker typed read
    that silently lost events when shard ranges did not cover the keyspace."""

    def _viewer(self, directory):
        files = sorted(glob.glob(os.path.join(directory, "*.pfw.gz")))
        return dftu_utils.TraceViewer(files, index_path=directory).group_by("name").agg("count")

    @staticmethod
    def _count(reg, pa):
        if reg is None:
            return 0
        return int(pa.compute.sum(pa.table(reg)["count"]).as_py() or 0)

    def test_shard_ranges_union_to_full_scan(self):
        pa = pytest.importorskip("pyarrow")
        import pyarrow.compute  # noqa: F401

        from dftracer.utils.dftracer_utils_ext import NUM_SHARDS

        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1, 2, 3, 4])
            tv = self._viewer(directory)
            full = self._count(tv.collect_typed()["regular"], pa)
            assert full > 0
            for parts in (2, 4, 8):
                span = (NUM_SHARDS + parts - 1) // parts
                total = 0
                for i in range(parts):
                    begin = min(NUM_SHARDS, i * span)
                    end = min(NUM_SHARDS, begin + span)
                    if begin >= end:
                        continue
                    total += self._count(
                        tv.collect_typed(shard_begin=begin, shard_end=end)["regular"], pa
                    )
                assert total == full, f"{parts}-way shard split summed {total}, full scan {full}"


if __name__ == "__main__":
    pytest.main([__file__])
