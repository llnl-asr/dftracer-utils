#!/usr/bin/env python3
"""Test cases for TraceReader Python bindings."""

import pytest

import dftracer.utils as dft_utils

from .common import Environment


class TestTraceReaderCreation:
    """Construction and property tests."""

    def test_creation_basic(self):
        """TraceReader accepts a valid file path and exposes file_path."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            assert reader.file_path == gz_file

    def test_creation_nonexistent_file(self):
        """TraceReader with nonexistent file creates but read_lines fails."""
        reader = dft_utils.TraceReader("/nonexistent/path/file.pfw.gz")
        assert not reader.has_index
        with pytest.raises(RuntimeError):
            reader.read_lines()

    def test_has_index_false_without_sidecar(self):
        """has_index is False when no .idx sidecar exists."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            assert reader.has_index is False

    def test_has_index_true_after_indexer_build(self):
        """has_index is True when a sidecar was built before construction."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file) as indexer:
                indexer.build()
            # TraceReader probes for the sidecar at __init__ time
            reader = dft_utils.TraceReader(gz_file)
            assert reader.has_index is True

    def test_index_dir_default_is_empty_string(self):
        """index_dir defaults to the empty string."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            assert reader.index_dir == ""

    def test_index_dir_custom(self):
        """index_dir is stored as supplied."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            assert env.temp_dir is not None
            reader = dft_utils.TraceReader(gz_file, index_dir=env.temp_dir)
            assert reader.index_dir == env.temp_dir

    def test_has_index_is_bool(self):
        """has_index is a Python bool, not an int."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            assert isinstance(reader.has_index, bool)

    def test_file_path_is_str(self):
        """file_path property returns a str."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            assert isinstance(reader.file_path, str)


class TestTraceReaderReadLines:
    """read_lines() behaviour tests."""

    def test_read_all_lines_default_args(self):
        """read_lines() with no arguments returns all lines."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            lines = reader.read_lines()
            assert isinstance(lines, list)
            assert len(lines) == 22

    def test_read_lines_returns_strings(self):
        """Every element returned by read_lines() is a str."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            lines = reader.read_lines()
            assert all(isinstance(line, str) for line in lines)

    def test_read_lines_content_is_json(self):
        """Lines contain the JSON fields written by Environment."""
        with Environment(lines=5) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            lines = reader.read_lines()
            for line in lines:
                stripped = line.strip()
                if stripped in ("[", "]"):
                    continue
                assert '"name"' in line

    def test_read_lines_explicit_zero_zero(self):
        """read_lines(0, 0) is equivalent to read_lines()."""
        with Environment(lines=15) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            assert reader.read_lines(0, 0) == reader.read_lines()

    def test_read_lines_with_range(self):
        """read_lines(start, end) returns a subset of lines."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            all_lines = reader.read_lines()
            partial = reader.read_lines(start_line=2, end_line=6)
            assert isinstance(partial, list)
            assert 0 < len(partial) <= len(all_lines)

    def test_read_lines_range_is_subset_of_all(self):
        """Lines returned for a range are a contiguous subset of all lines."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            all_lines = reader.read_lines()
            partial = reader.read_lines(start_line=3, end_line=8)
            # Every line in the partial result must appear in all_lines
            for line in partial:
                assert line in all_lines

    def test_read_lines_negative_start_raises(self):
        """read_lines raises ValueError for a negative start_line."""
        with Environment(lines=5) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            with pytest.raises(ValueError):
                reader.read_lines(start_line=-1)

    def test_read_lines_negative_end_raises(self):
        """read_lines raises ValueError for a negative end_line."""
        with Environment(lines=5) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            with pytest.raises(ValueError):
                reader.read_lines(end_line=-1)

    def test_read_lines_with_index(self):
        """read_lines() works correctly when a sidecar index is present."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file) as indexer:
                indexer.build()
            reader = dft_utils.TraceReader(gz_file)
            assert reader.has_index
            lines = reader.read_lines()
            assert len(lines) == 22

    def test_read_lines_indexed_matches_sequential(self):
        """Indexed and sequential reads return the same content."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            # Sequential (no index)
            sequential = dft_utils.TraceReader(gz_file).read_lines()

            # Build index, then read again
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file) as indexer:
                indexer.build()
            indexed = dft_utils.TraceReader(gz_file).read_lines()

            assert sequential == indexed


class TestTraceReaderNumLines:
    """num_lines property tests."""

    def test_num_lines_matches_line_count(self):
        """num_lines equals the number of lines written."""
        with Environment(lines=30) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            assert reader.num_lines == 32

    def test_num_lines_is_int(self):
        """num_lines returns an int."""
        with Environment(lines=5) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            assert isinstance(reader.num_lines, int)

    def test_num_lines_consistent_with_read_lines(self):
        """num_lines equals len(read_lines())."""
        with Environment(lines=25) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            assert reader.num_lines == 27


class TestTraceReaderContextManager:
    """Context manager protocol tests."""

    def test_context_manager_enter_returns_self(self):
        """__enter__ returns the TraceReader instance."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            result = reader.__enter__()
            assert result is reader

    def test_with_statement_basic(self):
        """TraceReader works as a context manager via with."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                lines = reader.read_lines()
                assert len(lines) == 12

    def test_with_statement_properties_accessible(self):
        """Properties are accessible inside a with block."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                assert reader.file_path == gz_file
                assert isinstance(reader.has_index, bool)

    def test_with_statement_exit_does_not_raise(self):
        """Exiting the context manager does not raise."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file):
                pass  # normal exit


class TestTraceReaderOptionalParams:
    """Optional constructor parameter tests."""

    def test_custom_checkpoint_size_accepted(self):
        """checkpoint_size kwarg is accepted without error."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file, checkpoint_size=1024 * 1024)
            assert reader.file_path == gz_file

    def test_auto_build_index_accepted(self):
        """auto_build_index kwarg is accepted without error."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file, auto_build_index=False)
            assert reader.file_path == gz_file

    def test_index_threshold_accepted(self):
        """index_threshold kwarg is accepted without error."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file, index_threshold=16 * 1024 * 1024)
            assert reader.file_path == gz_file

    def test_all_optional_params_together(self):
        """All optional constructor params can be supplied simultaneously."""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            assert env.temp_dir is not None
            reader = dft_utils.TraceReader(
                gz_file,
                index_dir=env.temp_dir,
                checkpoint_size=512 * 1024,
                auto_build_index=False,
                index_threshold=4 * 1024 * 1024,
            )
            assert reader.file_path == gz_file
            assert reader.index_dir == env.temp_dir


class TestTraceReaderIterLines:
    """iter_lines() streaming iterator tests."""

    def test_iter_lines_returns_iterator(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            it = reader.iter_lines()
            assert hasattr(it, "__iter__")
            assert hasattr(it, "__next__")

    def test_iter_lines_yields_strings(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            for line in reader.iter_lines():
                assert isinstance(line, str)

    def test_iter_lines_count(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            count = sum(1 for _ in reader.iter_lines())
            assert count == 22

    def test_iter_lines_matches_read_lines(self):
        with Environment(lines=15) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            from_iter = list(reader.iter_lines())
            from_read = reader.read_lines()
            assert from_iter == from_read

    def test_iter_lines_with_range(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            all_lines = list(reader.iter_lines())
            partial = list(reader.iter_lines(start_line=2, end_line=6))
            assert 0 < len(partial) <= len(all_lines)

    def test_iter_lines_early_break(self):
        """Early break from iterator does not hang or leak."""
        with Environment(lines=100) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            count = 0
            for line in reader.iter_lines():
                count += 1
                if count >= 5:
                    break
            assert count == 5

    def test_iter_lines_negative_raises(self):
        with Environment(lines=5) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            with pytest.raises(ValueError):
                list(reader.iter_lines(start_line=-1))

    def test_iter_lines_buffer_size_accepted(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            lines = list(reader.iter_lines(buffer_size=1024))
            assert len(lines) == 12


class TestTraceReaderIterRaw:
    """iter_raw() streaming iterator tests."""

    def test_iter_raw_returns_iterator(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            it = reader.iter_raw()
            assert hasattr(it, "__iter__")
            assert hasattr(it, "__next__")

    def test_iter_raw_yields_bytes(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            for chunk in reader.iter_raw():
                assert isinstance(chunk, bytes)

    def test_iter_raw_single_line_mode(self):
        """multi_line=False yields one chunk per line."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            chunks = list(reader.iter_raw(multi_line=False))
            assert len(chunks) == 22

    def test_iter_raw_content_consistent_with_lines(self):
        """Total raw content matches total line content."""
        with Environment(lines=15) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            lines = list(reader.iter_lines())
            raw_chunks = list(reader.iter_raw(multi_line=False))
            assert len(raw_chunks) == len(lines)

    def test_iter_raw_early_break(self):
        with Environment(lines=100) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            count = 0
            for chunk in reader.iter_raw():
                count += 1
                if count >= 3:
                    break
            assert count == 3

    def test_iter_raw_negative_raises(self):
        with Environment(lines=5) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            with pytest.raises(ValueError):
                list(reader.iter_raw(start_line=-1))


class TestTraceReaderReadRaw:
    """read_raw() materialized list tests."""

    def test_read_raw_returns_list_of_bytes(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            chunks = reader.read_raw()
            assert isinstance(chunks, list)
            assert all(isinstance(c, bytes) for c in chunks)

    def test_read_raw_matches_iter_raw(self):
        with Environment(lines=15) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            from_read = reader.read_raw()
            from_iter = list(reader.iter_raw())
            assert from_read == from_iter

    def test_read_raw_single_line_count(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            chunks = reader.read_raw(multi_line=False)
            assert len(chunks) == 22


class TestTraceReaderWithRuntime:
    """TraceReader with explicit Runtime."""

    def test_accepts_runtime_kwarg(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            rt = dft_utils.Runtime(threads=2)
            reader = dft_utils.TraceReader(gz_file, runtime=rt)
            lines = reader.read_lines()
            assert len(lines) == 12
            rt.shutdown()

    def test_iter_lines_with_runtime(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            rt = dft_utils.Runtime(threads=2)
            reader = dft_utils.TraceReader(gz_file, runtime=rt)
            count = sum(1 for _ in reader.iter_lines())
            assert count == 12
            rt.shutdown()

    def test_default_runtime_works(self):
        """TraceReader without explicit runtime uses default."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            lines = list(reader.iter_lines())
            assert len(lines) == 12


class TestTraceReaderJSON:
    """JSON reading tests."""

    def test_read_lines_json_returns_list(self):
        """read_lines_json returns a list of JSON objects."""
        with Environment(lines=32) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            result = reader.read_lines_json()
            assert isinstance(result, list)
            assert len(result) == 32

    def test_read_lines_json_objects_have_keys(self):
        """Each JSON object has expected keys."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            result = reader.read_lines_json()
            for obj in result:
                assert "name" in obj
                assert "cat" in obj
                assert "dur" in obj

    def test_read_lines_json_values_correct(self):
        """JSON values match what was written."""
        with Environment(lines=5) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            result = reader.read_lines_json()
            assert result[0]["name"] == "write"
            assert result[0]["cat"] == "POSIX"
            assert result[0]["ph"] == "X"

    def test_iter_lines_json_is_lazy(self):
        """iter_lines_json returns an iterator, not a list."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            it = reader.iter_lines_json()
            assert hasattr(it, "__iter__")
            assert hasattr(it, "__next__")
            items = list(it)
            assert len(items) == 10

    def test_iter_lines_json_partial_iteration(self):
        """Can stop iterating early."""
        with Environment(lines=100) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            it = reader.iter_lines_json()
            first = next(it)
            assert "name" in first
            # Don't exhaust the iterator

    def test_read_lines_json_with_line_range(self):
        """read_lines_json respects start_line/end_line."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)
            # Lines are 1-indexed; line 1 is "[", line 2 is first JSON, etc.
            # But iter_lines_json skips non-JSON lines, so we get JSON objects
            all_json = reader.read_lines_json()
            subset = reader.read_lines_json(start_line=1, end_line=10)
            assert len(subset) <= len(all_json)
            assert len(subset) > 0

    def test_read_lines_json_with_byte_range(self):
        """read_lines_json respects start_byte/end_byte."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)
            max_bytes = reader.get_max_bytes()
            if max_bytes > 0:
                half = max_bytes // 2
                subset = reader.read_lines_json(start_byte=0, end_byte=half)
                assert len(subset) > 0
                assert len(subset) < 50


class TestTraceReaderMetadata:
    """get_max_bytes and get_num_lines tests."""

    def test_get_max_bytes_indexed(self):
        """get_max_bytes returns positive value for indexed files."""
        with Environment(lines=32) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)
            max_bytes = reader.get_max_bytes()
            assert isinstance(max_bytes, int)
            assert max_bytes > 0

    def test_get_num_lines_indexed(self):
        """get_num_lines returns correct count for indexed files."""
        with Environment(lines=32) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)
            num_lines = reader.get_num_lines()
            assert isinstance(num_lines, int)
            assert num_lines > 0

    def test_get_max_bytes_unindexed_compressed(self):
        """get_max_bytes returns 0 for compressed files without index."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            assert reader.get_max_bytes() == 0

    def test_get_num_lines_unindexed(self):
        """get_num_lines returns 0 for files without index."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            assert reader.get_num_lines() == 0

    def test_num_lines_property_still_works(self):
        """num_lines property falls back to counting for unindexed files."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            # Property should still work (falls back to reading all lines)
            assert reader.num_lines > 0


class TestTraceReaderClamping:
    """Test that out-of-range requests are clamped, not errored."""

    def test_end_line_beyond_total_clamped(self):
        """Requesting more lines than exist returns what's available."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)
            # Request way more lines than exist
            result = reader.read_lines(start_line=1, end_line=99999)
            assert len(result) > 0

    def test_start_line_beyond_total_returns_empty(self):
        """Requesting lines starting beyond total returns empty."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)
            result = reader.read_lines(start_line=99999, end_line=100000)
            assert len(result) == 0

    def test_end_byte_beyond_max_clamped(self):
        """Requesting bytes beyond max returns what's available."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)
            max_bytes = reader.get_max_bytes()
            result = reader.read_lines(start_byte=0, end_byte=max_bytes * 10)
            assert len(result) > 0

    def test_start_byte_beyond_max_returns_empty(self):
        """Requesting bytes starting beyond max returns empty."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)
            max_bytes = reader.get_max_bytes()
            result = reader.read_lines(start_byte=max_bytes * 10, end_byte=max_bytes * 20)
            assert len(result) == 0

    def test_clamping_with_json(self):
        """JSON reading also clamps correctly."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)
            result = reader.read_lines_json(start_line=1, end_line=99999)
            assert len(result) > 0
            assert len(result) == 10

    def test_streaming_clamping_no_index(self):
        """Without index, requesting too many lines just returns what exists."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)
            result = reader.read_lines(start_line=1, end_line=99999)
            assert len(result) > 0


class TestTraceReaderQuery:
    """Query filtering tests."""

    @staticmethod
    def _create_dft_trace(env):
        """Create a .pfw.gz with DFTracer events for query testing."""
        import gzip
        import os

        pfw_path = os.path.join(env.temp_dir, "query_test.pfw.gz")
        with gzip.open(pfw_path, "wt") as f:
            names = ["read", "write", "open", "close"]
            cats = ["IO", "IO", "IO", "COMPUTE"]
            for i in range(40):
                name = names[i % len(names)]
                cat = cats[i % len(cats)]
                f.write(
                    f'{{"ph":"X","name":"{name}","cat":"{cat}",'
                    f'"pid":1,"tid":{1 + i % 3},"ts":{1000 + i * 100},'
                    f'"dur":{10 + i},"args":{{}}}}\n'
                )
        return pfw_path

    def test_query_filters_by_cat(self):
        with Environment() as env:
            gz = self._create_dft_trace(env)
            reader = dft_utils.TraceReader(gz)
            all_lines = reader.read_lines()
            filtered = reader.read_lines(query='cat == "IO"')
            assert len(filtered) > 0
            assert len(filtered) < len(all_lines)

    def test_query_no_match(self):
        with Environment() as env:
            gz = self._create_dft_trace(env)
            reader = dft_utils.TraceReader(gz)
            filtered = reader.read_lines(query='cat == "NONEXISTENT"')
            assert len(filtered) == 0

    def test_query_by_name(self):
        with Environment() as env:
            gz = self._create_dft_trace(env)
            reader = dft_utils.TraceReader(gz)
            filtered = reader.read_lines(query='name == "read"')
            assert len(filtered) > 0
            for line in filtered:
                assert '"name":"read"' in line

    def test_query_and(self):
        with Environment() as env:
            gz = self._create_dft_trace(env)
            reader = dft_utils.TraceReader(gz)
            cat_only = reader.read_lines(query='cat == "IO"')
            both = reader.read_lines(query='cat == "IO" and name == "read"')
            assert len(both) > 0
            assert len(both) <= len(cat_only)

    def test_query_or(self):
        with Environment() as env:
            gz = self._create_dft_trace(env)
            reader = dft_utils.TraceReader(gz)
            reads = reader.read_lines(query='name == "read"')
            writes = reader.read_lines(query='name == "write"')
            either = reader.read_lines(query='name == "read" or name == "write"')
            assert len(either) == len(reads) + len(writes)

    def test_query_with_range(self):
        with Environment() as env:
            gz = self._create_dft_trace(env)
            reader = dft_utils.TraceReader(gz)
            filtered = reader.read_lines(start_line=1, end_line=10, query='cat == "IO"')
            assert len(filtered) <= 10

    def test_empty_query_reads_all(self):
        with Environment() as env:
            gz = self._create_dft_trace(env)
            reader = dft_utils.TraceReader(gz)
            all_lines = reader.read_lines()
            with_empty = reader.read_lines(query="")
            assert len(all_lines) == len(with_empty)

    def test_iter_lines_with_query(self):
        with Environment() as env:
            gz = self._create_dft_trace(env)
            reader = dft_utils.TraceReader(gz)
            lines = list(reader.iter_lines(query='name == "write"'))
            assert len(lines) > 0
            for line in lines:
                assert '"name":"write"' in line

    def test_iter_lines_json_with_query(self):
        with Environment() as env:
            gz = self._create_dft_trace(env)
            reader = dft_utils.TraceReader(gz)
            events = list(reader.iter_lines_json(query='cat == "COMPUTE"'))
            assert len(events) > 0
            for ev in events:
                assert ev["cat"] == "COMPUTE"

    def test_query_with_field_class(self):
        from dftracer.utils.query import Field

        cat = Field("cat")
        name = Field("name")
        q = (cat == "IO") & (name == "read")

        with Environment() as env:
            gz = self._create_dft_trace(env)
            reader = dft_utils.TraceReader(gz)
            filtered = reader.read_lines(query=str(q))
            assert len(filtered) > 0
            for line in filtered:
                assert '"cat":"IO"' in line
                assert '"name":"read"' in line


if __name__ == "__main__":
    pytest.main([__file__])
