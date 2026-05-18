#!/usr/bin/env python3
"""
Test cases for DFTracer indexer Python bindings
"""

import os

import pytest

import dftracer.utils as dft_utils
from dftracer.utils.dftracer_utils_ext import CheckpointIndexer as NativeIndexer

from .common import Environment


class TestCheckpointIndexer:
    """Test cases for checkpoint-level indexer operations via get_checkpoint_indexer"""

    def test_checkpoint_indexer_creation(self):
        """Test checkpoint indexer creation via Indexer.get_checkpoint_indexer"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()

            with dft_utils.Indexer(files=[gz_file]) as indexer:
                indexer.ensure_indexed()
                cp_indexer = indexer.get_checkpoint_indexer(gz_file)

                assert cp_indexer.gz_path == gz_file
                assert cp_indexer.checkpoint_size > 0

    def test_checkpoint_indexer_file_info(self):
        """Test checkpoint indexer file information methods"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()

            with dft_utils.Indexer(files=[gz_file]) as indexer:
                indexer.ensure_indexed()
                cp_indexer = indexer.get_checkpoint_indexer(gz_file)

                max_bytes = cp_indexer.get_max_bytes()
                num_lines = cp_indexer.get_num_lines()

                assert isinstance(max_bytes, int)
                assert isinstance(num_lines, int)
                assert max_bytes > 0
                assert num_lines > 0

    def test_checkpoint_indexer_checkpoints(self):
        """Test checkpoint indexer checkpoint functionality"""
        with Environment(lines=100000) as env:
            gz_file = env.create_test_gzip_file()
            checkpoint_size = 256 * 1024  # 256KB

            with dft_utils.Indexer(
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

                checkpoints = cp_indexer.get_checkpoints()
                assert isinstance(checkpoints, list)
                print(f"Number of checkpoints created: {len(checkpoints)}")

                for checkpoint in checkpoints:
                    assert hasattr(checkpoint, "checkpoint_idx")
                    assert hasattr(checkpoint, "uc_offset")
                    assert hasattr(checkpoint, "uc_size")
                    assert hasattr(checkpoint, "c_offset")
                    assert hasattr(checkpoint, "c_size")
                    assert hasattr(checkpoint, "bits")
                    assert hasattr(checkpoint, "num_lines")

                    assert isinstance(checkpoint.checkpoint_idx, int)
                    assert isinstance(checkpoint.uc_offset, int)
                    assert isinstance(checkpoint.num_lines, int)
                    assert checkpoint.checkpoint_idx >= 0
                    assert checkpoint.uc_offset >= 0
                    assert checkpoint.num_lines >= 0

    def test_checkpoint_indexer_find_checkpoint(self):
        """Test checkpoint indexer single checkpoint search"""
        with Environment(lines=2000) as env:
            gz_file = env.create_test_gzip_file(bytes_per_line=2048)
            checkpoint_size = 512 * 1024  # 512KB

            with dft_utils.Indexer(
                files=[gz_file],
                checkpoint_size=checkpoint_size,
            ) as indexer:
                indexer.ensure_indexed()
                cp_indexer = indexer.get_checkpoint_indexer(gz_file)

                max_bytes = cp_indexer.get_max_bytes()
                checkpoints = cp_indexer.get_checkpoints()

                print(f"File has {max_bytes} bytes and {len(checkpoints)} checkpoints")

                target_offset = max_bytes // 2 if max_bytes > 0 else 0
                checkpoint = cp_indexer.find_checkpoint(target_offset)

                if checkpoint is not None:
                    assert hasattr(checkpoint, "uc_offset")
                    assert hasattr(checkpoint, "uc_size")
                    assert hasattr(checkpoint, "num_lines")
                    assert checkpoint.uc_offset <= target_offset
                    assert isinstance(checkpoint.uc_offset, int)
                    assert isinstance(checkpoint.uc_size, int)
                    assert isinstance(checkpoint.num_lines, int)

                # find_checkpoint(0) should return None per implementation
                checkpoint_0 = cp_indexer.find_checkpoint(0)
                assert checkpoint_0 is None

                if max_bytes > 0:
                    checkpoint_beyond = cp_indexer.find_checkpoint(max_bytes + 1000)
                    if checkpoint_beyond is not None:
                        assert checkpoint_beyond.uc_offset <= max_bytes


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

    def test_native_indexer_build_bloom(self):
        """Test building with bloom=True"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)
            with NativeIndexer(gz_file, index_path, build_bloom=True) as indexer:
                indexer.build()
                assert indexer.has_bloom

    def test_native_indexer_build_manifest(self):
        """Test building with manifest=True"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)
            with NativeIndexer(gz_file, index_path, build_manifest=True) as indexer:
                indexer.build()
                assert indexer.has_manifest


class TestCheckpointIndexerIntegration:
    """Integration tests for checkpoint indexer with reader"""

    def test_checkpoint_indexer_with_reader_creation(self):
        """Test creating readers from checkpoint indexer"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()

            with dft_utils.Indexer(files=[gz_file]) as indexer:
                indexer.ensure_indexed()

                reader = dft_utils.TraceReader(gz_file)
                assert reader.get_max_bytes() > 0
                assert reader.path == gz_file

    def test_multiple_readers_same_index(self):
        """Test creating multiple readers from the same index"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()

            with dft_utils.Indexer(files=[gz_file]) as indexer:
                indexer.ensure_indexed()

                readers = []
                for i in range(3):
                    reader = dft_utils.TraceReader(gz_file)
                    assert reader.get_max_bytes() > 0
                    readers.append(reader)

                max_bytes = readers[0].get_max_bytes()
                for reader in readers[1:]:
                    assert reader.get_max_bytes() == max_bytes


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

            reader = dft_utils.TraceReader(gz_file)
            assert reader.get_num_lines() > 0


class TestDirectoryIndexer:
    """Test cases for the directory-level Indexer API"""

    def test_indexer_creation(self):
        """Test directory indexer creation"""
        with Environment() as env:
            env.create_test_gzip_file()
            env.create_test_gzip_file()

            indexer = dft_utils.Indexer(env.temp_dir)
            assert indexer is not None

    def test_indexer_context_manager(self):
        """Test directory indexer as context manager"""
        with Environment() as env:
            env.create_test_gzip_file()

            with dft_utils.Indexer(env.temp_dir) as indexer:
                assert indexer is not None

    def test_indexer_resolve(self):
        """Test resolve() returns IndexStatus"""
        with Environment() as env:
            env.create_test_gzip_file()

            with dft_utils.Indexer(env.temp_dir) as indexer:
                status = indexer.resolve()
                assert isinstance(status, dft_utils.IndexStatus)
                assert status.total_files >= 1
                assert len(status.needs_work) >= 1

    def test_indexer_build(self):
        """Test build() creates indexes"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            print(f"\nCreated test file: {gz_file}")
            print(f"Directory: {env.temp_dir}")
            print(f"Files in dir: {os.listdir(env.temp_dir)}")

            with dft_utils.Indexer(env.temp_dir) as indexer:
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

            with dft_utils.Indexer(env.temp_dir) as indexer:
                status = indexer.ensure_indexed()
                assert isinstance(status, dft_utils.IndexStatus)
                assert len(status.ready) >= 1

    def test_indexer_with_require_bloom(self):
        """Test indexer with bloom filter requirement"""
        with Environment() as env:
            env.create_test_gzip_file()

            with dft_utils.Indexer(env.temp_dir, require_bloom=True) as indexer:
                status = indexer.ensure_indexed()
                assert len(status.ready) >= 1

    def test_indexer_with_require_manifest(self):
        """Test indexer with manifest requirement"""
        with Environment() as env:
            env.create_test_gzip_file()

            with dft_utils.Indexer(env.temp_dir, require_manifest=True) as indexer:
                status = indexer.ensure_indexed()
                assert len(status.ready) >= 1

    def test_indexer_with_aggregation_config(self):
        """Test indexer with aggregation config"""
        with Environment() as env:
            env.create_test_gzip_file()

            agg_config = dft_utils.AggregationConfig(
                time_interval_ms=1000.0,
                compute_percentiles=False,
            )
            with dft_utils.Indexer(
                env.temp_dir,
                require_aggregation=agg_config,
            ) as indexer:
                assert indexer.aggregation_config is not None
                assert indexer.aggregation_config.time_interval_ms == 1000.0

    def test_indexer_aggregation_true(self):
        """Test indexer with require_aggregation=True uses defaults"""
        with Environment() as env:
            env.create_test_gzip_file()

            with dft_utils.Indexer(
                env.temp_dir,
                require_aggregation=True,
            ) as indexer:
                assert indexer.aggregation_config is not None
                assert indexer.aggregation_config.time_interval_ms == 5000.0

    def test_index_status_dataclass(self):
        """Test IndexStatus dataclass"""
        status = dft_utils.IndexStatus(
            total_files=5,
            ready=["a.pfw.gz", "b.pfw.gz"],
            needs_work=["c.pfw.gz"],
            index_path="/tmp/index",
        )
        assert status.total_files == 5
        assert len(status.ready) == 2
        assert len(status.needs_work) == 1
        assert status.index_path == "/tmp/index"

    def test_aggregation_config_dataclass(self):
        """Test AggregationConfig dataclass"""
        config = dft_utils.AggregationConfig(
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

            with dft_utils.Indexer(
                files=[file_path],
                index_dir=env.temp_dir,
            ) as indexer:
                status = indexer.resolve()
                assert status.total_files == 1

    def test_indexer_files_and_directory(self):
        """Test indexer with both files and directory (files take precedence)"""
        with Environment() as env:
            file_path = env.create_test_gzip_file()

            with dft_utils.Indexer(
                directory=env.temp_dir,
                files=[file_path],
            ) as indexer:
                status = indexer.resolve()
                assert status.total_files >= 1

    def test_indexer_requires_directory_or_files(self):
        """Test that indexer requires at least directory or files"""
        with pytest.raises(ValueError, match="directory.*files"):
            dft_utils.Indexer()

    def test_indexer_get_checkpoint_indexer(self):
        """Test get_checkpoint_indexer returns working checkpoint indexer"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()

            with dft_utils.Indexer(env.temp_dir) as indexer:
                indexer.ensure_indexed()

                cp_indexer = indexer.get_checkpoint_indexer(gz_file)

                assert cp_indexer.gz_path == gz_file
                assert cp_indexer.get_max_bytes() > 0
                assert cp_indexer.get_num_lines() > 0
                checkpoints = cp_indexer.get_checkpoints()
                assert isinstance(checkpoints, list)

    def test_indexer_get_checkpoint_indexer_uses_index_dir(self):
        """Test that get_checkpoint_indexer uses the same index_dir"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            custom_index_dir = os.path.join(env.temp_dir, "custom_index")
            os.makedirs(custom_index_dir, exist_ok=True)

            with dft_utils.Indexer(
                env.temp_dir,
                index_dir=custom_index_dir,
            ) as indexer:
                indexer.ensure_indexed()

                cp_indexer = indexer.get_checkpoint_indexer(gz_file)
                assert custom_index_dir in cp_indexer.index_path


class TestIndexerDfanalyzerAPIs:
    """Test cases for dfanalyzer integration APIs (hash tables, PID manifest)"""

    def test_get_hash_table_file(self):
        """Test get_hash_table returns file hash mappings"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dft_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
                require_manifest=True,
            ) as indexer:
                indexer.ensure_indexed()

                file_hashes = indexer.get_hash_table("file")
                assert isinstance(file_hashes, dict)

    def test_get_hash_table_host(self):
        """Test get_hash_table returns host hash mappings"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dft_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
                require_manifest=True,
            ) as indexer:
                indexer.ensure_indexed()

                host_hashes = indexer.get_hash_table("host")
                assert isinstance(host_hashes, dict)

    def test_get_hash_table_string(self):
        """Test get_hash_table returns string hash mappings"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dft_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
                require_manifest=True,
            ) as indexer:
                indexer.ensure_indexed()

                string_hashes = indexer.get_hash_table("string")
                assert isinstance(string_hashes, dict)

    def test_get_hash_table_invalid_type(self):
        """Test get_hash_table raises error for invalid type"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dft_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
                require_manifest=True,
            ) as indexer:
                indexer.ensure_indexed()

                with pytest.raises((ValueError, RuntimeError)):
                    indexer.get_hash_table("invalid_type")

    def test_query_file_pids(self):
        """Test query_file_pids returns set of PIDs for a file"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dft_utils.Indexer(
                files=[gz_file],
                require_manifest=True,
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

            with dft_utils.Indexer(
                files=[gz_file],
                require_manifest=True,
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

            with dft_utils.Indexer(
                files=[gz_file1, gz_file2],
                require_manifest=True,
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

            with dft_utils.Indexer(
                files=[gz_file],
                require_manifest=False,
            ) as indexer:
                # Only checkpoint tier, no manifest
                indexer.ensure_indexed()

                all_pids = indexer.query_all_file_pids()
                assert isinstance(all_pids, dict)

    def test_integration_hash_tables_and_pids(self):
        """Integration test: hash tables and PIDs work together"""
        with Environment() as env:
            gz_file = env.create_dft_trace_file()

            with dft_utils.Indexer(
                files=[gz_file],
                require_bloom=True,
                require_manifest=True,
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


class TestQueryFilter:
    """Test cases for query filter parameter in iter_arrow_dfanalyzer APIs"""

    def _make_indexer(self, directory):
        return dft_utils.Indexer(
            directory=directory,
            require_aggregation=dft_utils.AggregationConfig(time_interval_ms=5000),
        )

    def test_iter_arrow_dfanalyzer_all_no_query(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            with self._make_indexer(directory) as indexer:
                indexer.ensure_indexed()
                result = indexer.iter_arrow_dfanalyzer_all()
                rows = sum(pa.record_batch(b).num_rows for b in result.get("events", []))
                assert rows > 0

    def test_iter_arrow_dfanalyzer_all_pid_filter(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            with self._make_indexer(directory) as indexer:
                indexer.ensure_indexed()
                result = indexer.iter_arrow_dfanalyzer_all(query="pid == 1")
                rows = sum(pa.record_batch(b).num_rows for b in result.get("events", []))
                assert rows > 0

    def test_iter_arrow_dfanalyzer_all_pid_filter_reduces_rows(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1, 2])
            with self._make_indexer(directory) as indexer:
                indexer.ensure_indexed()

                all_rows = sum(
                    pa.record_batch(b).num_rows
                    for b in indexer.iter_arrow_dfanalyzer_all().get("events", [])
                )
                filtered_rows = sum(
                    pa.record_batch(b).num_rows
                    for b in indexer.iter_arrow_dfanalyzer_all(query="pid == 1").get("events", [])
                )
                assert 0 < filtered_rows < all_rows

    def test_iter_arrow_dfanalyzer_all_invalid_query(self):
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            with self._make_indexer(directory) as indexer:
                indexer.ensure_indexed()
                with pytest.raises((ValueError, RuntimeError)):
                    indexer.iter_arrow_dfanalyzer_all(query="invalid ==")

    def test_iter_arrow_dfanalyzer_query_param(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            with self._make_indexer(directory) as indexer:
                indexer.ensure_indexed()
                batches = list(indexer.iter_arrow_dfanalyzer("events", query="pid == 1"))
                rows = sum(pa.record_batch(b).num_rows for b in batches)
                assert rows > 0

    def test_iter_arrow_dfanalyzer_query_matches_all(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            with self._make_indexer(directory) as indexer:
                indexer.ensure_indexed()

                single_rows = sum(
                    pa.record_batch(b).num_rows
                    for b in indexer.iter_arrow_dfanalyzer("events", query="pid == 1")
                )
                all_rows = sum(
                    pa.record_batch(b).num_rows
                    for b in indexer.iter_arrow_dfanalyzer_all(query="pid == 1").get("events", [])
                )
                assert single_rows == all_rows

    def test_iter_arrow_dfanalyzer_all_multi_pid_filter(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[10, 20, 30])
            with self._make_indexer(directory) as indexer:
                indexer.ensure_indexed()

                filtered_rows = sum(
                    pa.record_batch(b).num_rows
                    for b in indexer.iter_arrow_dfanalyzer_all(query="pid == 10 or pid == 20").get(
                        "events", []
                    )
                )
                all_rows = sum(
                    pa.record_batch(b).num_rows
                    for b in indexer.iter_arrow_dfanalyzer_all().get("events", [])
                )
                assert 0 < filtered_rows < all_rows

    def test_iter_arrow_dfanalyzer_all_string_filter(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            with self._make_indexer(directory) as indexer:
                indexer.ensure_indexed()
                result = indexer.iter_arrow_dfanalyzer_all(query='cat == "POSIX"')
                rows = sum(pa.record_batch(b).num_rows for b in result.get("events", []))
                assert rows > 0

    def test_iter_arrow_dfanalyzer_all_no_match(self):
        pa = pytest.importorskip("pyarrow")
        with Environment() as env:
            directory = env.create_indexed_traces(pids=[1])
            with self._make_indexer(directory) as indexer:
                indexer.ensure_indexed()
                result = indexer.iter_arrow_dfanalyzer_all(query="pid == 999999")
                rows = sum(pa.record_batch(b).num_rows for b in result.get("events", []))
                assert rows == 0


if __name__ == "__main__":
    pytest.main([__file__])
