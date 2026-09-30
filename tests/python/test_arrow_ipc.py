"""Tests for Arrow IPC file output and readback via pyarrow."""

import pytest

pa = pytest.importorskip("pyarrow")

import dftracer.utils as dftu_utils  # noqa: E402

from .common import Environment  # noqa: E402


class TestArrowIpcReadback:
    """Verify Arrow output is readable by pyarrow."""

    def test_trace_viewer_stream_roundtrip(self):
        """TraceViewer.stream Arrow output is readable by pyarrow."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            with dftu_utils.Indexer(files=[gz_file], index_dir=env.temp_dir) as ix:
                ix.ensure_indexed()

            viewer = dftu_utils.TraceViewer(gz_file, index_path=env.temp_dir)
            batches = [pa.record_batch(c) for c in viewer.stream(batch_size=100)]
            assert batches
            for pa_batch in batches:
                assert pa_batch.num_rows > 0
                col_names = set(pa_batch.schema.names)
                assert "name" in col_names or "cat" in col_names
