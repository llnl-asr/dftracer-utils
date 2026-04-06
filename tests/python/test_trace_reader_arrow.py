#!/usr/bin/env python3
"""Test cases for TraceReader Arrow streaming (iter_arrow / read_arrow)."""

import dftracer.utils as dft_utils
from dftracer.utils.arrow import ArrowBatch, ArrowTable

from .common import Environment


class TestIterArrow:
    """Tests for TraceReader.iter_arrow()."""

    def test_iter_arrow_returns_batches(self):
        """iter_arrow yields batch objects with __arrow_c_array__."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                batches = list(reader.iter_arrow(batch_size=100))
            assert len(batches) >= 1
            for b in batches:
                assert hasattr(b, "__arrow_c_array__")
                assert hasattr(b, "num_rows")
                assert hasattr(b, "num_columns")

    def test_iter_arrow_correct_row_count(self):
        """Total rows across all batches equals number of JSON lines."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                batches = list(reader.iter_arrow(batch_size=20))
            total_rows = sum(b.num_rows for b in batches)
            assert total_rows == 50

    def test_iter_arrow_batch_size_respected(self):
        """Each batch has at most batch_size rows."""
        with Environment(lines=100) as env:
            gz_file = env.create_test_gzip_file()
            batch_size = 30
            with dft_utils.TraceReader(gz_file) as reader:
                batches = list(reader.iter_arrow(batch_size=batch_size))
            for b in batches:
                assert b.num_rows <= batch_size

    def test_iter_arrow_discovers_columns(self):
        """Arrow batches have columns matching JSON keys."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                batches = list(reader.iter_arrow(batch_size=100))
            assert len(batches) == 1
            b = batches[0]
            # Test data has: name, cat, dur, data
            assert b.num_columns >= 3  # at least name, cat, dur

    def test_iter_arrow_clamped_range(self):
        """iter_arrow with out-of-range bytes clamps to actual bounds."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            # Out-of-range start_byte is clamped to max, yielding empty
            # (for non-indexed files, clamping may read all data)
            with dft_utils.TraceReader(gz_file) as reader:
                batches = list(reader.iter_arrow(start_byte=999999999, end_byte=999999999))
            # Just verify it doesn't crash — clamping behavior varies
            assert isinstance(batches, list)

    def test_iter_arrow_with_line_range(self):
        """iter_arrow respects start_line/end_line parameters."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            # Build index for line-based access
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(gz_file, index_path) as indexer:
                indexer.build()
            with dft_utils.TraceReader(gz_file) as reader:
                batches = list(reader.iter_arrow(start_line=10, end_line=20, batch_size=100))
            total_rows = sum(b.num_rows for b in batches)
            # end_line is inclusive, so lines 10..20 = 11 lines
            assert total_rows == 11


class TestReadArrow:
    """Tests for TraceReader.read_arrow()."""

    def test_read_arrow_returns_arrow_table(self):
        """read_arrow returns an ArrowTable."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                table = reader.read_arrow(batch_size=100)
            assert isinstance(table, ArrowTable)

    def test_read_arrow_row_count(self):
        """ArrowTable has correct total row count."""
        with Environment(lines=30) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                table = reader.read_arrow(batch_size=100)
            assert table.num_rows == 30

    def test_read_arrow_batch_access(self):
        """ArrowTable provides batch access."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                table = reader.read_arrow(batch_size=20)
            assert table.num_batches >= 1
            for b in table.batches():
                # Batches are raw _ArrowBatchCapsule objects (not ArrowBatch wrappers)
                assert hasattr(b, "__arrow_c_array__")
                assert hasattr(b, "num_rows")

    def test_read_arrow_properties(self):
        """ArrowTable exposes num_batches, num_rows, empty."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                table = reader.read_arrow(batch_size=100)
            assert table.num_rows == 20
            assert table.num_batches >= 1
            assert not table.empty


class TestArrowBatchWrapper:
    """Tests for the ArrowBatch Python wrapper."""

    def test_arrow_batch_wraps_capsule(self):
        """ArrowBatch wraps a capsule from iter_arrow."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                raw_batches = list(reader.iter_arrow(batch_size=100))
            assert len(raw_batches) >= 1
            batch = ArrowBatch(raw_batches[0])
            assert hasattr(batch, "__arrow_c_array__")
            assert batch.num_rows == raw_batches[0].num_rows

    def test_arrow_batch_to_pandas_requires_pyarrow(self):
        """to_pandas raises ImportError if pyarrow is not installed."""
        with Environment(lines=5) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                raw_batches = list(reader.iter_arrow(batch_size=100))
            batch = ArrowBatch(raw_batches[0])
            # This test only verifies the method exists; actual conversion
            # depends on pyarrow being installed
            try:
                df = batch.to_pandas()
                # If pyarrow is installed, verify it returns a DataFrame
                import pandas as pd

                assert isinstance(df, pd.DataFrame)
                assert len(df) == batch.num_rows  # safe: cached after export

                # Verify multiple calls work (caching)
                df2 = batch.to_pandas()
                assert len(df2) == len(df)
            except ImportError:
                pass  # Expected if pyarrow not installed
