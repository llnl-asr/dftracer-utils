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


if __name__ == "__main__":
    pytest.main([__file__])
