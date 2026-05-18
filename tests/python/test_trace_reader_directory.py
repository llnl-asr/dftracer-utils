#!/usr/bin/env python3
"""Test cases for directory-level parallel TraceReader (iter_arrow / read_arrow)."""

import os

import dftracer.utils as dft_utils
from dftracer.utils.arrow import ArrowTable

from .common import Environment


def _create_directory_with_files(env, num_files=3, lines_per_file=20, nested=False):
    """Create multiple .pfw.gz files in env.temp_dir, optionally in subdirectories."""
    files = []
    for i in range(num_files):
        if nested:
            subdir = f"rank_{i}"
            filename = os.path.join(subdir, f"trace_{i}.pfw.gz")
        else:
            filename = f"trace_{i}.pfw.gz"
        f = env.create_dft_trace_file(filename=filename, num_events=lines_per_file)
        files.append(f)
    return files


class TestDirectoryIterArrow:
    """Tests for TraceReader.iter_arrow() with a directory path."""

    def test_iter_arrow_directory_returns_batches(self):
        """iter_arrow on a directory yields Arrow batches from all files."""
        with Environment(lines=20) as env:
            _create_directory_with_files(env, num_files=3, lines_per_file=20)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                batches = list(reader.iter_arrow(batch_size=100))
            rt.shutdown()
            assert len(batches) >= 1
            total_rows = sum(b.num_rows for b in batches)
            assert total_rows == 60

    def test_iter_arrow_directory_single_file(self):
        """Directory with one file produces same results as single-file path."""
        with Environment(lines=30) as env:
            files = _create_directory_with_files(env, num_files=1, lines_per_file=30)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as dir_reader:
                dir_batches = list(dir_reader.iter_arrow(batch_size=100))
            with dft_utils.TraceReader(files[0], runtime=rt) as file_reader:
                file_batches = list(file_reader.iter_arrow(batch_size=100))
            rt.shutdown()
            dir_rows = sum(b.num_rows for b in dir_batches)
            file_rows = sum(b.num_rows for b in file_batches)
            assert dir_rows == file_rows == 30

    def test_iter_arrow_directory_nested_subdirs(self):
        """iter_arrow discovers .pfw.gz files in nested subdirectories."""
        with Environment(lines=10) as env:
            _create_directory_with_files(env, num_files=4, lines_per_file=10, nested=True)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                batches = list(reader.iter_arrow(batch_size=100))
            rt.shutdown()
            total_rows = sum(b.num_rows for b in batches)
            assert total_rows == 40

    def test_iter_arrow_directory_batch_size(self):
        """Batch size is respected when reading from a directory."""
        with Environment(lines=25) as env:
            _create_directory_with_files(env, num_files=3, lines_per_file=25)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                batches = list(reader.iter_arrow(batch_size=10))
            rt.shutdown()
            for b in batches:
                assert b.num_rows <= 10
            total_rows = sum(b.num_rows for b in batches)
            assert total_rows == 75

    def test_iter_arrow_directory_empty(self):
        """Directory with no .pfw.gz files yields no batches."""
        with Environment(lines=10) as env:
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                batches = list(reader.iter_arrow(batch_size=100))
            rt.shutdown()
            assert len(batches) == 0


class TestDirectoryReadArrow:
    """Tests for TraceReader.read_arrow() with a directory path."""

    def test_read_arrow_directory(self):
        """read_arrow on a directory returns an ArrowTable with all rows."""
        with Environment(lines=15) as env:
            _create_directory_with_files(env, num_files=4, lines_per_file=15)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                table = reader.read_arrow(batch_size=100)
            rt.shutdown()
            assert isinstance(table, ArrowTable)
            assert table.num_rows == 60

    def test_read_arrow_directory_properties(self):
        """ArrowTable from directory has correct properties."""
        with Environment(lines=10) as env:
            _create_directory_with_files(env, num_files=2, lines_per_file=10)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                table = reader.read_arrow(batch_size=100)
            rt.shutdown()
            assert table.num_rows == 20
            assert table.num_batches >= 1
            assert not table.empty


class TestDirectoryWithQuery:
    """Tests for directory reading with query filtering."""

    def test_directory_query_filters_events(self):
        """Query filtering works across directory files."""
        with Environment(lines=50) as env:
            _create_directory_with_files(env, num_files=3, lines_per_file=50)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                table_all = reader.read_arrow(batch_size=1000)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                table_filtered = reader.read_arrow(batch_size=1000, query='name == "read"')
            rt.shutdown()
            assert table_all.num_rows == 150
            assert table_filtered.num_rows > 0
            assert table_filtered.num_rows < table_all.num_rows

    def test_directory_query_no_match(self):
        """Query that matches nothing returns empty table."""
        with Environment(lines=20) as env:
            _create_directory_with_files(env, num_files=2, lines_per_file=20)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                table = reader.read_arrow(batch_size=100, query='name == "nonexistent_op"')
            rt.shutdown()
            assert table.num_rows == 0


class TestDirectoryIterJson:
    """Tests for TraceReader.iter_json() with a directory path."""

    def test_iter_json_directory_returns_events(self):
        """iter_json on a directory yields JsonDictValue events from all files."""
        with Environment(lines=20) as env:
            _create_directory_with_files(env, num_files=3, lines_per_file=20)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                events = list(reader.iter_json())
            rt.shutdown()
            assert len(events) == 60
            for ev in events:
                assert "name" in ev

    def test_iter_json_directory_single_file(self):
        """Directory with one file matches single-file iter_json."""
        with Environment(lines=30) as env:
            files = _create_directory_with_files(env, num_files=1, lines_per_file=30)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as dir_reader:
                dir_events = list(dir_reader.iter_json())
            with dft_utils.TraceReader(files[0], runtime=rt) as file_reader:
                file_events = list(file_reader.iter_json())
            rt.shutdown()
            assert len(dir_events) == len(file_events) == 30

    def test_iter_json_directory_nested_subdirs(self):
        """iter_json discovers .pfw.gz files in nested subdirectories."""
        with Environment(lines=10) as env:
            _create_directory_with_files(env, num_files=4, lines_per_file=10, nested=True)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                events = list(reader.iter_json())
            rt.shutdown()
            assert len(events) == 40

    def test_iter_json_directory_empty(self):
        """Directory with no .pfw.gz files yields no events."""
        with Environment(lines=10) as env:
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                events = list(reader.iter_json())
            rt.shutdown()
            assert len(events) == 0

    def test_read_json_directory(self):
        """read_json on a directory returns all events."""
        with Environment(lines=15) as env:
            _create_directory_with_files(env, num_files=4, lines_per_file=15)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                events = reader.read_json()
            rt.shutdown()
            assert len(events) == 60

    def test_iter_json_directory_to_dict(self):
        """JsonDictValue.to_dict() works for directory-sourced events."""
        with Environment(lines=10) as env:
            _create_directory_with_files(env, num_files=2, lines_per_file=10)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                events = list(reader.iter_json())
            rt.shutdown()
            for ev in events:
                d = ev.to_dict()
                assert isinstance(d, dict)
                assert "name" in d


class TestDirectoryIterLines:
    """Tests for TraceReader.iter_lines() with a directory path."""

    def test_iter_lines_directory_returns_lines(self):
        """iter_lines on a directory yields memoryview lines from all files."""
        with Environment(lines=20) as env:
            _create_directory_with_files(env, num_files=3, lines_per_file=20)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                lines = list(reader.iter_lines())
            rt.shutdown()
            assert len(lines) == 60
            for line in lines:
                assert isinstance(line, memoryview)

    def test_read_lines_directory(self):
        """read_lines on a directory returns all lines."""
        with Environment(lines=15) as env:
            _create_directory_with_files(env, num_files=4, lines_per_file=15)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                lines = reader.read_lines()
            rt.shutdown()
            assert len(lines) == 60

    def test_iter_lines_directory_empty(self):
        """Directory with no .pfw.gz files yields no lines."""
        with Environment(lines=10) as env:
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                lines = list(reader.iter_lines())
            rt.shutdown()
            assert len(lines) == 0


class TestDirectoryIterRaw:
    """Tests for TraceReader.iter_raw() with a directory path."""

    def test_iter_raw_directory_returns_chunks(self):
        """iter_raw on a directory yields memoryview chunks from all files."""
        with Environment(lines=20) as env:
            _create_directory_with_files(env, num_files=3, lines_per_file=20)
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                chunks = list(reader.iter_raw())
            rt.shutdown()
            assert len(chunks) >= 1
            for chunk in chunks:
                assert isinstance(chunk, memoryview)
                assert len(chunk) > 0

    def test_iter_raw_directory_empty(self):
        """Directory with no .pfw.gz files yields no chunks."""
        with Environment(lines=10) as env:
            rt = dft_utils.Runtime(threads=2)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                chunks = list(reader.iter_raw())
            rt.shutdown()
            assert len(chunks) == 0


class TestDirectoryMultiThreaded:
    """Tests for directory reading with various thread counts."""

    def test_directory_single_thread(self):
        """Directory reading works with single thread."""
        with Environment(lines=20) as env:
            _create_directory_with_files(env, num_files=3, lines_per_file=20)
            rt = dft_utils.Runtime(threads=1)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                table = reader.read_arrow(batch_size=100)
            rt.shutdown()
            assert table.num_rows == 60

    def test_directory_many_threads(self):
        """Directory reading works with more threads than files."""
        with Environment(lines=10) as env:
            _create_directory_with_files(env, num_files=2, lines_per_file=10)
            rt = dft_utils.Runtime(threads=8)
            with dft_utils.TraceReader(env.temp_dir, runtime=rt) as reader:
                table = reader.read_arrow(batch_size=100)
            rt.shutdown()
            assert table.num_rows == 20
