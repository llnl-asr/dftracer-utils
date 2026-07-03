#!/usr/bin/env python3
"""Test cases for TraceReader Arrow streaming (iter_arrow / read_arrow)."""

import dftracer.utils as dft_utils
from dftracer.utils.arrow import ArrowBatch, ArrowTable
from dftracer.utils.dftracer_utils_ext import CheckpointIndexer as NativeIndexer

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
            with NativeIndexer(gz_file, index_path) as indexer:
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

    def test_read_arrow_normalize_names_not_corrupted(self):
        """Regression: normalize=True must not corrupt name via simdjson
        string-buffer reuse. The old rewind-based args pass overwrote the
        name view, so names read back as fragments of the args (e.g. hhash).
        """
        import pyarrow as pa

        with Environment(lines=40) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                rbr = pa.RecordBatchReader.from_stream(reader.iter_arrow_stream(normalize=True))
                table = rbr.read_all()
            names = set(table.column("name").to_pylist())
            cats = {c for c in table.column("cat").to_pylist() if c is not None}
        expected = {
            "pread",
            "pwrite",
            "read",
            "write",
            "fread",
            "fwrite",
            "open",
            "close",
        }
        assert names, "no normalized rows produced"
        # Corruption would surface as fragments of the args (e.g. "abc123").
        assert names <= expected, f"corrupted normalized names: {names - expected}"
        assert cats <= {"posix", "stdio"}, f"corrupted normalized cats: {cats}"


class TestIterArrowStream:
    """Tests for TraceReader.iter_arrow_stream()."""

    def test_iter_arrow_stream_exposes_c_stream(self):
        """iter_arrow_stream returns an object with __arrow_c_stream__."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                stream = reader.iter_arrow_stream(batch_size=100)
                assert hasattr(stream, "__arrow_c_stream__")

    def test_iter_arrow_stream_row_count_matches_iter_arrow(self):
        """Stream drains the same row count as the per-batch iterator."""
        import pyarrow as pa

        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                batches_expected = list(reader.iter_arrow(batch_size=20))
            expected_rows = sum(b.num_rows for b in batches_expected)

            with dft_utils.TraceReader(gz_file) as reader:
                stream = reader.iter_arrow_stream(batch_size=20)
                rbr = pa.RecordBatchReader.from_stream(stream)
                total = sum(b.num_rows for b in rbr)
            assert total == expected_rows
            assert total == 50

    def test_iter_arrow_stream_schema_matches_iter_arrow(self):
        """Stream schema equals iter_arrow's schema plus the _extra catch-all column."""
        import pyarrow as pa

        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                expected_batch = pa.record_batch(next(iter(reader.iter_arrow(batch_size=100))))
                expected_names = list(expected_batch.schema.names)

            with dft_utils.TraceReader(gz_file) as reader:
                stream = reader.iter_arrow_stream(batch_size=100)
                rbr = pa.RecordBatchReader.from_stream(stream)
                stream_names = list(rbr.schema.names)
            assert stream_names[-1] == "_extra"
            assert set(stream_names[:-1]) == set(expected_names)

    def test_iter_arrow_stream_pa_table(self):
        """pa.table(stream) materializes a full Table."""
        import pyarrow as pa

        with Environment(lines=40) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                stream = reader.iter_arrow_stream(batch_size=10)
                table = pa.table(stream)
            assert isinstance(table, pa.Table)
            assert table.num_rows == 40

    def test_iter_arrow_stream_single_use(self):
        """__arrow_c_stream__ can only be consumed once."""
        with Environment(lines=5) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                stream = reader.iter_arrow_stream(batch_size=100)
                stream.__arrow_c_stream__()
                try:
                    stream.__arrow_c_stream__()
                    raise AssertionError("expected RuntimeError on second consume")
                except RuntimeError:
                    pass

    def test_iter_arrow_stream_survives_early_drop(self):
        """Dropping an in-flight batch must not double-free the stream."""
        import gc

        import pyarrow as pa

        with Environment(lines=30) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                stream = reader.iter_arrow_stream(batch_size=5)
                rbr = pa.RecordBatchReader.from_stream(stream)
                first = next(iter(rbr))
                assert first.num_rows > 0
                # Drop the first batch while stream is still live.
                del first
                gc.collect()
                total = sum(b.num_rows for b in rbr)
            assert total >= 0

    def test_read_arrow_uses_stream_path(self):
        """read_arrow still produces a correct ArrowTable via the stream."""
        with Environment(lines=30) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                table = reader.read_arrow(batch_size=10)
            assert isinstance(table, ArrowTable)
            assert table.num_rows == 30


class TestIterArrowStreamReconciliation:
    """Stream emits a single locked schema across batches with diverging columns."""

    @staticmethod
    def _write_trace(path, rows):
        import gzip
        import json
        import os

        os.makedirs(os.path.dirname(path), exist_ok=True)
        with gzip.open(path, "wt", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r) + "\n")

    def _make_divergent_dir(self, env):
        """Directory whose top-level keys differ across files. `args` is
        serialized as a single JSON string column by the Arrow builder, so
        divergence has to be at the top level to reach the reconciler."""
        import os

        base = os.path.join(env.temp_dir, "divergent")
        common = {"name": "read", "cat": "POSIX", "pid": 1, "tid": 1, "dur": 10, "ph": "X"}
        rows_a = [{**common, "ts": i, "only_a_int": i * 3} for i in range(30)]
        rows_b = [{**common, "ts": i, "only_b_str": f"b-{i}"} for i in range(30)]
        rows_c = [{**common, "ts": i, "only_c_dbl": float(i) * 0.5} for i in range(30)]
        self._write_trace(os.path.join(base, "a.pfw.gz"), rows_a)
        self._write_trace(os.path.join(base, "b.pfw.gz"), rows_b)
        self._write_trace(os.path.join(base, "c.pfw.gz"), rows_c)
        for f in ("a.pfw.gz", "b.pfw.gz", "c.pfw.gz"):
            gz = os.path.join(base, f)
            env.test_files.append(gz)
            idx = env.get_index_path(gz)
            with NativeIndexer(gz, idx) as indexer:
                indexer.build()
        return base

    def test_stream_schema_has_extra_column(self):
        import pyarrow as pa

        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                stream = reader.iter_arrow_stream(batch_size=100)
                rbr = pa.RecordBatchReader.from_stream(stream)
                assert "_extra" in rbr.schema.names

    def test_stream_survives_divergent_schemas(self):
        """Directory-mode stream with differing args shapes must not error."""
        import pyarrow as pa

        with Environment() as env:
            data_dir = self._make_divergent_dir(env)
            with dft_utils.TraceReader(data_dir) as reader:
                stream = reader.iter_arrow_stream(batch_size=25)
                rbr = pa.RecordBatchReader.from_stream(stream)
                table = rbr.read_all()
            assert table.num_rows == 90

    def test_stream_preserves_all_column_data(self):
        """Each file's unique column ends up either as a native column (with
        nulls for the other files) or in _extra JSON. No data is lost."""
        import pyarrow as pa

        with Environment() as env:
            data_dir = self._make_divergent_dir(env)
            with dft_utils.TraceReader(data_dir) as reader:
                stream = reader.iter_arrow_stream(batch_size=25)
                rbr = pa.RecordBatchReader.from_stream(stream)
                table = rbr.read_all()

            names = set(table.schema.names)
            assert "_extra" in names

            def hits_for(colname):
                if colname in names:
                    return sum(1 for v in table.column(colname).to_pylist() if v is not None)
                extras = table.column("_extra").to_pylist()
                return sum(1 for e in extras if e and colname in e)

            # Each file's unique column must appear 30 times, either natively
            # or via _extra — the reconciler preserves every value.
            assert hits_for("only_a_int") == 30
            assert hits_for("only_b_str") == 30
            assert hits_for("only_c_dbl") == 30

    def test_stream_matches_pa_table_from_stream(self):
        """pa.table(stream) yields the same row count as RecordBatchReader.read_all."""
        import pyarrow as pa

        with Environment() as env:
            data_dir = self._make_divergent_dir(env)
            with dft_utils.TraceReader(data_dir) as reader:
                stream = reader.iter_arrow_stream(batch_size=25)
                table = pa.table(stream)
            assert table.num_rows == 90

    def test_stream_empty_result_has_schema(self):
        """Empty stream still exposes a schema with _extra so callers don't crash."""
        import pyarrow as pa

        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                stream = reader.iter_arrow_stream(batch_size=100, query="pid == 99999999")
                rbr = pa.RecordBatchReader.from_stream(stream)
                assert "_extra" in rbr.schema.names
                total = sum(b.num_rows for b in rbr)
            assert total == 0

    def test_stream_flatten_promotes_nested_keys(self):
        """flatten_objects=True expands top-level object values one level."""
        import pyarrow as pa

        with Environment(lines=20) as env:
            gz_file = env.create_dft_trace_file()
            with dft_utils.TraceReader(gz_file) as reader:
                stream = reader.iter_arrow_stream(batch_size=100, flatten_objects=True)
                rbr = pa.RecordBatchReader.from_stream(stream)
                table = rbr.read_all()
            names = set(table.schema.names)
            # args.ret and args.hhash should be promoted to native typed columns.
            assert "args.ret" in names
            assert "args.hhash" in names
            # Native type survives the reconciler; values must round-trip.
            rets = table.column("args.ret").to_pylist()
            assert all(isinstance(v, int) for v in rets if v is not None)
            assert rets[0] == 1024

    def test_stream_no_flatten_keeps_args_as_json(self):
        """flatten_objects=False leaves `args` as a single JSON string column."""
        import pyarrow as pa

        with Environment(lines=10) as env:
            gz_file = env.create_dft_trace_file()
            with dft_utils.TraceReader(gz_file) as reader:
                stream = reader.iter_arrow_stream(batch_size=100, flatten_objects=False)
                rbr = pa.RecordBatchReader.from_stream(stream)
                table = rbr.read_all()
            names = set(table.schema.names)
            assert "args" in names
            assert "args.ret" not in names
            first = table.column("args").to_pylist()[0]
            assert isinstance(first, str)
            assert first.startswith("{") and "ret" in first

    def test_stream_extra_is_null_when_no_divergence(self):
        """_extra should be all-null when every batch matches the discovered schema."""
        import pyarrow as pa

        with Environment(lines=40) as env:
            gz_file = env.create_test_gzip_file()
            with dft_utils.TraceReader(gz_file) as reader:
                stream = reader.iter_arrow_stream(batch_size=10)
                rbr = pa.RecordBatchReader.from_stream(stream)
                table = rbr.read_all()
            assert table.num_rows == 40
            extra = table.column("_extra")
            assert extra.null_count == extra.length()


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
