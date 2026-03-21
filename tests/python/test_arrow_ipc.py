"""Tests for Arrow IPC file output and readback via pyarrow."""

import os
import shutil
import subprocess
import tempfile

import pyarrow as pa
import pyarrow.ipc as ipc

import dftracer.utils as dft_utils
from dftracer.utils.dftracer_utils_ext import (
    AggregatorUtility,
    ViewReaderUtility,
)

from .common import Environment


class TestArrowIpcReadback:
    """Verify Arrow output is readable by pyarrow."""

    def test_aggregator_cli_arrow_output(self):
        """dftracer_aggregator --format arrow produces a valid IPC file."""
        binary = shutil.which("dftracer_aggregator")
        if binary is None:
            return  # CLI not installed

        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir

            with tempfile.NamedTemporaryFile(suffix=".arrows", delete=False) as f:
                output_path = f.name

            try:
                subprocess.run(
                    [
                        binary,
                        "-d",
                        directory,
                        "-o",
                        output_path,
                        "--format",
                        "arrow",
                    ],
                    capture_output=True,
                    text=True,
                    timeout=60,
                    check=True,
                )

                assert os.path.exists(output_path)
                assert os.path.getsize(output_path) > 0

                reader = ipc.open_file(output_path)
                table = reader.read_all()

                assert table.num_rows > 0
                assert table.num_columns == 18

                col_names = set(table.column_names)
                assert "cat" in col_names
                assert "name" in col_names
                assert "count" in col_names
                assert "dur_total" in col_names
                assert "time_bucket" in col_names

            finally:
                if os.path.exists(output_path):
                    os.unlink(output_path)

    def test_aggregator_python_roundtrip(self):
        """AggregatorUtility Arrow output is readable by pyarrow."""
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir

            table = AggregatorUtility().process(directory)
            assert table.num_rows > 0

            for batch in table.batches():
                pa_batch = pa.record_batch(batch)
                assert pa_batch.num_rows > 0
                assert pa_batch.num_columns == 18

                schema = pa_batch.schema
                assert schema.field("cat").type == pa.utf8()
                assert schema.field("count").type == pa.uint64()
                assert schema.field("dur_mean").type == pa.float64()

    def test_view_reader_roundtrip(self):
        """ViewReaderUtility Arrow output is readable by pyarrow."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()

            table = ViewReaderUtility().process(gz_file, predicates={"cat": ["cat_1"]})

            if table.num_rows > 0:
                for batch in table.batches():
                    pa_batch = pa.record_batch(batch)
                    assert pa_batch.num_rows > 0
                    assert pa_batch.num_columns >= 1

    def test_trace_reader_roundtrip(self):
        """TraceReader Arrow output is readable by pyarrow."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            reader = dft_utils.TraceReader(gz_file)

            for batch in reader.iter_arrow(batch_size=100):
                pa_batch = pa.record_batch(batch)
                assert pa_batch.num_rows > 0
                col_names = set(pa_batch.schema.names)
                assert "name" in col_names or "cat" in col_names
