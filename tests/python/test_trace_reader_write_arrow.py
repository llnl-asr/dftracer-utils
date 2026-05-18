"""Tests for TraceReader.write_arrow with bloom filter pruning."""

import os
import tempfile

import pyarrow as pa
import pyarrow.ipc as ipc
import pytest

import dftracer.utils as dft_utils

from .common import Environment


class TestTraceReaderWriteArrow:
    """Test TraceReader.write_arrow functionality."""

    def test_write_arrow_basic(self):
        """Basic write_arrow produces readable Arrow IPC files."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                result = reader.write_arrow(output_dir)

                assert "partitions" in result
                assert "total_rows" in result
                assert "total_bytes" in result
                assert "chunks_scanned" in result
                assert "chunks_skipped" in result

                assert result["total_rows"] > 0

                for view_name, stats in result["partitions"].items():
                    assert "files" in stats
                    assert "rows" in stats
                    assert len(stats["files"]) > 0

                    for arrow_file in stats["files"]:
                        assert os.path.exists(arrow_file)
                        reader_ipc = ipc.open_file(arrow_file)
                        table = reader_ipc.read_all()
                        assert table.num_rows > 0

    def test_write_arrow_predefined_views(self):
        """Test predefined views (io) - view may filter some events."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                result = reader.write_arrow(output_dir, views=["io"])

                assert "io" in result["partitions"]
                stats = result["partitions"]["io"]

                if stats["rows"] > 0:
                    for arrow_file in stats["files"]:
                        reader_ipc = ipc.open_file(arrow_file)
                        table = reader_ipc.read_all()
                        assert table.num_rows > 0
                        cats = table.column("cat").to_pylist()
                        for cat in cats:
                            assert cat in ["POSIX", "STDIO"]

    def test_write_arrow_custom_query(self):
        """Test custom query view."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                views = [{"name": "reads", "query": 'name == "read"'}]
                result = reader.write_arrow(output_dir, views=views)

                assert "reads" in result["partitions"]
                stats = result["partitions"]["reads"]

                if stats["rows"] > 0:
                    for arrow_file in stats["files"]:
                        reader_ipc = ipc.open_file(arrow_file)
                        table = reader_ipc.read_all()
                        names = table.column("name").to_pylist()
                        for name in names:
                            assert name == "read"

    def test_write_arrow_bloom_filter_pruning(self):
        """Verify bloom filter pruning returns stats."""
        with Environment(lines=100) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                views = [{"name": "posix_only", "query": 'cat == "POSIX"'}]
                result = reader.write_arrow(output_dir, views=views)

                assert "chunks_scanned" in result
                assert "chunks_skipped" in result
                assert result["chunks_scanned"] >= 0
                assert result["chunks_skipped"] >= 0

    def test_write_arrow_multiple_views(self):
        """Test multiple views in single call."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                views = [
                    {"name": "reads", "query": 'name == "read"'},
                    {"name": "writes", "query": 'name == "write"'},
                ]
                result = reader.write_arrow(output_dir, views=views)

                assert "reads" in result["partitions"]
                assert "writes" in result["partitions"]

                reads_dir = os.path.join(output_dir, "reads")
                writes_dir = os.path.join(output_dir, "writes")
                assert os.path.isdir(reads_dir)
                assert os.path.isdir(writes_dir)

    def test_write_arrow_compression(self):
        """Test different compression options."""
        with Environment(lines=30) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                result_zstd = reader.write_arrow(
                    os.path.join(output_dir, "zstd"), compression="zstd"
                )
                result_none = reader.write_arrow(
                    os.path.join(output_dir, "none"), compression="none"
                )

                assert result_zstd["total_rows"] == result_none["total_rows"]

                zstd_files = result_zstd["partitions"]["all"]["files"]
                none_files = result_none["partitions"]["all"]["files"]

                zstd_size = sum(os.path.getsize(f) for f in zstd_files)
                none_size = sum(os.path.getsize(f) for f in none_files)

                assert zstd_size < none_size

    def test_write_arrow_chunk_size(self):
        """Test chunk_size_mb controls file splitting."""
        with Environment(lines=100) as env:
            gz_file = env.create_test_gzip_file(bytes_per_line=4096)
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                result = reader.write_arrow(output_dir, chunk_size_mb=0)

                stats = result["partitions"]["all"]
                assert len(stats["files"]) == 1

    def test_write_arrow_no_metadata(self):
        """Test include_metadata=False excludes metadata events."""
        with Environment(lines=30) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                views = [{"name": "no_meta", "query": 'cat == "POSIX"', "include_metadata": False}]
                result = reader.write_arrow(output_dir, views=views)

                assert "no_meta" in result["partitions"]


class TestElasticArrowSchema:
    """Test elastic Arrow schema with varying event fields."""

    def test_varying_schema_single_file(self):
        """Events with different fields produce consistent Arrow schema."""
        with Environment(lines=500) as env:
            gz_file = env.create_varying_schema_file()
            env.build_index(gz_file, checkpoint_size_bytes=4 * 1024)
            reader = dft_utils.TraceReader(gz_file, checkpoint_size=4 * 1024)

            with tempfile.TemporaryDirectory() as output_dir:
                result = reader.write_arrow(output_dir)

                assert result["total_rows"] > 0
                stats = result["partitions"]["all"]
                assert len(stats["files"]) >= 1

                schemas = []
                for arrow_file in stats["files"]:
                    reader_ipc = ipc.open_file(arrow_file)
                    schemas.append(reader_ipc.schema)

                if len(schemas) > 1:
                    first_schema = schemas[0]
                    for i, schema in enumerate(schemas[1:], 1):
                        assert schema.equals(first_schema), (
                            f"Schema mismatch between file 0 and file {i}"
                        )

    def test_varying_schema_column_order_stable(self):
        """Column order remains consistent across batches."""
        with Environment(lines=1000) as env:
            gz_file = env.create_varying_schema_file(num_events=1000)
            env.build_index(gz_file, checkpoint_size_bytes=2 * 1024)
            reader = dft_utils.TraceReader(gz_file, checkpoint_size=2 * 1024)

            with tempfile.TemporaryDirectory() as output_dir:
                result = reader.write_arrow(output_dir, batch_size=100)

                assert result["total_rows"] > 0
                stats = result["partitions"]["all"]

                all_tables = []
                for arrow_file in stats["files"]:
                    reader_ipc = ipc.open_file(arrow_file)
                    all_tables.append(reader_ipc.read_all())

                if len(all_tables) > 1:
                    first_columns = all_tables[0].column_names
                    for i, table in enumerate(all_tables[1:], 1):
                        assert table.column_names == first_columns, (
                            f"Column order mismatch between table 0 and table {i}"
                        )

    def test_varying_schema_null_backfill(self):
        """Fields not present in all events are backfilled with nulls."""
        with Environment(lines=500) as env:
            gz_file = env.create_varying_schema_file()
            env.build_index(gz_file, checkpoint_size_bytes=4 * 1024)
            reader = dft_utils.TraceReader(gz_file, checkpoint_size=4 * 1024)

            with tempfile.TemporaryDirectory() as output_dir:
                result = reader.write_arrow(output_dir)

                stats = result["partitions"]["all"]
                tables = [ipc.open_file(f).read_all() for f in stats["files"]]
                combined = pa.concat_tables(tables)

                if "rare_field" in combined.column_names:
                    rare_col = combined.column("rare_field")
                    null_count = rare_col.null_count
                    assert null_count > 0, "rare_field should have null values"
                    assert null_count < len(rare_col), "rare_field should have some non-null values"

    def test_varying_schema_pyarrow_concat(self):
        """Multiple IPC files can be concatenated with pyarrow."""
        with Environment(lines=1000) as env:
            gz_file = env.create_varying_schema_file(num_events=1000)
            env.build_index(gz_file, checkpoint_size_bytes=2 * 1024)
            reader = dft_utils.TraceReader(gz_file, checkpoint_size=2 * 1024)

            with tempfile.TemporaryDirectory() as output_dir:
                result = reader.write_arrow(output_dir)

                stats = result["partitions"]["all"]
                if len(stats["files"]) > 1:
                    tables = [ipc.open_file(f).read_all() for f in stats["files"]]
                    combined = pa.concat_tables(tables)
                    assert combined.num_rows == result["total_rows"]


class TestTraceReaderWriteArrowDask:
    """Test write_arrow integration with Dask."""

    def test_write_arrow_dask_read(self):
        """Verify Arrow output is readable by Dask."""
        pytest.importorskip("dask")
        pytest.importorskip("dask.dataframe")
        import dask.dataframe as dd

        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                result = reader.write_arrow(output_dir)

                arrow_files = result["partitions"]["all"]["files"]
                assert len(arrow_files) > 0

                tables = []
                for f in arrow_files:
                    reader_ipc = ipc.open_file(f)
                    tables.append(reader_ipc.read_all())

                combined = pa.concat_tables(tables)
                pdf = combined.to_pandas()

                ddf = dd.from_pandas(pdf, npartitions=2)
                assert len(ddf) == result["total_rows"]

    def test_write_arrow_parallel_views_dask(self):
        """Test reading multiple view outputs with Dask."""
        pytest.importorskip("dask")
        pytest.importorskip("dask.dataframe")
        import dask.dataframe as dd

        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                views = [
                    {"name": "posix", "query": 'cat == "POSIX"'},
                    {"name": "stdio", "query": 'cat == "STDIO"'},
                ]
                result = reader.write_arrow(output_dir, views=views)

                for view_name in ["posix", "stdio"]:
                    if result["partitions"][view_name]["rows"] > 0:
                        files = result["partitions"][view_name]["files"]
                        tables = [ipc.open_file(f).read_all() for f in files]
                        combined = pa.concat_tables(tables)
                        ddf = dd.from_pandas(combined.to_pandas(), npartitions=1)
                        assert len(ddf) == result["partitions"][view_name]["rows"]


class TestTraceReaderViewChunks:
    """Test get_view_chunks and write_view_chunk APIs."""

    def test_get_view_chunks_basic(self):
        """Test get_view_chunks returns chunk metadata."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            result = reader.get_view_chunks(view={"name": "all", "query": 'cat == "POSIX"'})

            assert "chunks" in result
            assert "total_checkpoints" in result
            assert "skipped_checkpoints" in result
            assert "file_may_match" in result
            assert result["total_checkpoints"] >= 0

    def test_write_view_chunk_basic(self):
        """Test write_view_chunk writes Arrow IPC file."""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            reader = dft_utils.TraceReader(gz_file)

            chunks_result = reader.get_view_chunks()
            if not chunks_result["chunks"]:
                pytest.skip("No chunks to process")

            chunk = chunks_result["chunks"][0]

            with tempfile.TemporaryDirectory() as output_dir:
                output_file = os.path.join(output_dir, "chunk-00000.arrow")
                result = reader.write_view_chunk(
                    output_file=output_file,
                    checkpoint_idx=chunk["checkpoint_idx"],
                    start_byte=chunk["start_byte"],
                    end_byte=chunk["end_byte"],
                )

                assert "output_file" in result
                assert "rows_written" in result
                assert os.path.exists(result["output_file"])

                if result["rows_written"] > 0:
                    reader_ipc = ipc.open_file(result["output_file"])
                    table = reader_ipc.read_all()
                    assert table.num_rows == result["rows_written"]

    def test_write_view_chunks_parallel(self):
        """Test write_view_chunks processes multiple chunks in parallel."""
        with Environment(lines=5000) as env:
            gz_file = env.create_test_gzip_file(bytes_per_line=512)
            env.build_index(gz_file, checkpoint_size_bytes=4 * 1024)
            reader = dft_utils.TraceReader(gz_file, checkpoint_size=4 * 1024)

            chunks_result = reader.get_view_chunks()
            if len(chunks_result["chunks"]) < 2:
                pytest.skip("Need at least 2 chunks for parallel test")

            chunks = chunks_result["chunks"][:4]

            with tempfile.TemporaryDirectory() as output_dir:
                result = reader.write_view_chunks(
                    chunks=chunks,
                    output_dir=output_dir,
                )

                assert "results" in result
                assert "total_rows" in result
                assert "total_events_matched" in result

                assert len(result["results"]) == len(chunks)

                total_rows = 0
                for r in result["results"]:
                    assert "output_file" in r
                    assert "rows_written" in r
                    if r["rows_written"] > 0:
                        assert os.path.exists(r["output_file"])
                        reader_ipc = ipc.open_file(r["output_file"])
                        table = reader_ipc.read_all()
                        assert table.num_rows == r["rows_written"]
                        total_rows += r["rows_written"]

                assert result["total_rows"] == total_rows


class TestDistributedWriteArrow:
    """Test distributed_write_arrow with Dask."""

    def test_distributed_write_arrow_basic(self):
        """Test distributed_write_arrow produces readable files."""
        pytest.importorskip("dask")
        from dftracer.utils.arrow import read_arrow
        from dftracer.utils.dask import distributed_write_arrow

        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                result = distributed_write_arrow(
                    gz_file, output_dir, view={"name": "all", "query": 'cat == "POSIX"'}
                )

                assert "files" in result
                assert "total_chunks" in result
                assert "skipped_chunks" in result
                assert "total_rows" in result

                if result["files"]:
                    table = read_arrow(result["files"])
                    assert table is not None
                    assert table.num_rows == result["total_rows"]

    def test_distributed_write_arrow_with_view(self):
        """Test distributed_write_arrow with predefined view."""
        pytest.importorskip("dask")
        from dftracer.utils.arrow import read_arrow
        from dftracer.utils.dask import distributed_write_arrow

        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)

            with tempfile.TemporaryDirectory() as output_dir:
                result = distributed_write_arrow(gz_file, output_dir)

                assert "files" in result
                if result["files"]:
                    table = read_arrow(result["files"])
                    assert table is not None

    def test_distributed_write_arrow_batched(self):
        """Test distributed_write_arrow with chunks_per_task batching."""
        pytest.importorskip("dask")
        from dftracer.utils.arrow import read_arrow
        from dftracer.utils.dask import distributed_write_arrow

        with Environment(lines=5000) as env:
            gz_file = env.create_test_gzip_file(bytes_per_line=512)
            env.build_index(gz_file, checkpoint_size_bytes=4 * 1024)

            with tempfile.TemporaryDirectory() as output_dir:
                result = distributed_write_arrow(
                    gz_file,
                    output_dir,
                    view={"name": "all", "query": 'cat == "POSIX"'},
                    checkpoint_size=4 * 1024,
                    chunks_per_task=2,
                )

                assert "files" in result
                assert "total_chunks" in result
                assert "total_rows" in result

                if result["files"]:
                    table = read_arrow(result["files"])
                    assert table is not None
                    assert table.num_rows == result["total_rows"]
