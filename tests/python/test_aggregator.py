"""Tests for AggregatorUtility."""

import gzip
from pathlib import Path
from typing import Dict, Tuple

import pytest

from dftracer.utils.arrow import ArrowTable
from dftracer.utils.dftracer_utils_ext import AggregatorUtility

from .common import Environment


class TestAggregatorUtility:
    EXPECTED_BASE_COLUMNS = {
        "batch_type",
        "cat",
        "name",
        "pid",
        "tid",
        "hhash",
        "fhash",
        "time_bucket",
        "count",
        "dur_total",
        "dur_min",
        "dur_max",
        "dur_mean",
        "dur_std",
        "size_total",
        "size_min",
        "size_max",
        "size_mean",
        "size_std",
        "ts",
        "te",
    }

    @staticmethod
    def _write_mixed_counter_trace(env: Environment) -> str:
        path = Path(env.temp_dir) / "mixed_trace.pfw.gz"
        content = (
            "\n".join(
                [
                    '{"name":"read","cat":"POSIX","pid":7,"tid":3,"ts":1000,"dur":50,"ph":"X","args":{"ret":64,"bytes":64,"hhash":"event_h","fhash":"event_f"}}',
                    '{"name":"cpu_usage","cat":"PROFILE","pid":7,"tid":3,"ts":1500,"dur":0,"ph":"C","args":{"count":4,"dur_sum":80,"dur_min":10,"dur_max":30,"ret_sum":400,"ret_min":50,"ret_max":150,"bytes_sum":1000,"bytes_min":100,"bytes_max":400,"hhash":"profile_h","fhash":"profile_f"}}',
                    '{"name":"mem_bw","cat":"sys","pid":7,"tid":3,"ts":2500,"dur":0,"ph":"C","args":{"count":2,"dur_sum":40,"dur_min":15,"dur_max":25,"ret_sum":600,"ret_min":250,"ret_max":350,"bytes_sum":1200,"bytes_min":500,"bytes_max":700,"hhash":"system_h","fhash":"system_f"}}',
                ]
            )
            + "\n"
        )
        with gzip.open(path, "wt", encoding="utf-8") as f:
            f.write(content)
        env.test_files.append(str(path))
        return str(path)

    @staticmethod
    def _rows_by_key(table: ArrowTable) -> Dict[Tuple[int, str, str], dict]:
        pa = pytest.importorskip("pyarrow")
        batches = [pa.record_batch(batch) for batch in table.batches()]
        rows: Dict[Tuple[int, str, str], dict] = {}
        for row in pa.Table.from_batches(batches).to_pylist():
            rows[(row["batch_type"], row["cat"], row["name"])] = row
        return rows

    def test_process_returns_arrow_table(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            result = AggregatorUtility().process(directory)
            assert isinstance(result, ArrowTable)

    def test_process_has_rows(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            result = AggregatorUtility().process(directory)
            assert result.num_rows > 0

    def test_process_batches_have_arrow_protocol(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            result = AggregatorUtility().process(directory)
            for batch in result.batches():
                pa = pytest.importorskip("pyarrow")
                assert hasattr(batch, "__arrow_c_array__")
                pa_batch = pa.record_batch(batch)
                assert pa_batch.num_columns == len(self.EXPECTED_BASE_COLUMNS)
                assert set(pa_batch.schema.names) == self.EXPECTED_BASE_COLUMNS

    def test_call_delegates_to_process(self):
        with Environment(lines=10) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            util = AggregatorUtility()
            result = util(directory)
            assert isinstance(result, ArrowTable)

    def test_iter_arrow_streams_batches(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            util = AggregatorUtility()
            batches = list(util.iter_arrow(directory))
            assert len(batches) >= 1
            for batch in batches:
                assert hasattr(batch, "__arrow_c_array__")
                assert batch.num_rows > 0

    def test_process_with_categories_filter(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            result = AggregatorUtility().process(directory, categories=["cat_1"])
            assert isinstance(result, ArrowTable)

    def test_process_with_time_interval(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            result = AggregatorUtility().process(directory, time_interval_ms=1000.0)
            assert isinstance(result, ArrowTable)
            assert result.num_rows > 0

    def test_process_empty_directory(self):
        with Environment(lines=0) as env:
            directory = env.temp_dir
            result = AggregatorUtility().process(directory)
            assert isinstance(result, ArrowTable)
            assert result.num_rows == 0

    def test_process_aggregates_profile_and_system_counters(self):
        with Environment(lines=0) as env:
            self._write_mixed_counter_trace(env)
            result = AggregatorUtility().process(
                env.temp_dir,
                index_dir=env.temp_dir,
                force_rebuild=True,
                custom_metric_fields=["bytes"],
                event_batch_size=1,
            )

            rows = self._rows_by_key(result)
            assert len(rows) == 3

            event = rows[(0, "POSIX", "read")]
            assert event["count"] == 1
            assert event["dur_total"] == 50
            assert event["size_total"] == 64
            assert event["bytes_total"] == 64

            profile = rows[(1, "PROFILE", "cpu_usage")]
            assert profile["count"] == 4
            assert profile["dur_total"] == 80
            assert profile["dur_min"] == 10
            assert profile["dur_max"] == 30
            assert profile["dur_mean"] == pytest.approx(20.0)
            assert profile["size_total"] == 400
            assert profile["size_mean"] == pytest.approx(100.0)
            assert profile["bytes_total"] == 1000
            assert profile["bytes_min"] == 100
            assert profile["bytes_max"] == 400
            assert profile["bytes_mean"] == pytest.approx(250.0)

            system = rows[(2, "sys", "mem_bw")]
            assert system["count"] == 2
            assert system["dur_total"] == 40
            assert system["dur_mean"] == pytest.approx(20.0)
            assert system["size_total"] == 600
            assert system["size_mean"] == pytest.approx(300.0)
            assert system["bytes_total"] == 1200
            assert system["bytes_min"] == 500
            assert system["bytes_max"] == 700
            assert system["bytes_mean"] == pytest.approx(600.0)

    def test_iter_arrow_emits_separate_event_profile_and_system_batches(self):
        with Environment(lines=0) as env:
            self._write_mixed_counter_trace(env)
            util = AggregatorUtility()
            result = ArrowTable(
                list(
                    util.iter_arrow(
                        env.temp_dir,
                        index_dir=env.temp_dir,
                        force_rebuild=True,
                        custom_metric_fields=["bytes"],
                        event_batch_size=1,
                    )
                )
            )

            rows = self._rows_by_key(result)
            assert set(rows) == {
                (0, "POSIX", "read"),
                (1, "PROFILE", "cpu_usage"),
                (2, "sys", "mem_bw"),
            }

    def test_process_unions_group_keys_and_custom_metric_columns(self):
        with Environment(lines=0) as env:
            path = Path(env.temp_dir) / "mixed_schema_trace.pfw.gz"
            content = (
                "\n".join(
                    [
                        '{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"ph":"X","args":{"ret":4,"epoch":"1","bytes":4,"hhash":"h1"}}',
                        '{"name":"write","cat":"POSIX","pid":1,"tid":1,"ts":2000,"dur":20,"ph":"X","args":{"ret":8,"step":"2","ops":3,"hhash":"h2"}}',
                    ]
                )
                + "\n"
            )
            with gzip.open(path, "wt", encoding="utf-8") as f:
                f.write(content)
            env.test_files.append(str(path))

            table = AggregatorUtility().process(
                env.temp_dir,
                index_dir=env.temp_dir,
                force_rebuild=True,
                group_keys=["epoch", "step"],
                custom_metric_fields=["bytes", "ops"],
                event_batch_size=10,
            )

            pa = pytest.importorskip("pyarrow")
            pa_batches = [pa.record_batch(batch) for batch in table.batches()]
            rows = pa.Table.from_batches(pa_batches).to_pylist()

            assert {"epoch", "step", "bytes_total", "ops_total"} <= set(pa_batches[0].schema.names)

            by_name = {row["name"]: row for row in rows}
            assert by_name["read"]["epoch"] == "1"
            assert by_name["read"]["step"] is None
            assert by_name["read"]["bytes_total"] == 4
            assert by_name["read"]["ops_total"] is None
            assert by_name["write"]["epoch"] is None
            assert by_name["write"]["step"] == "2"
            assert by_name["write"]["bytes_total"] is None
            assert by_name["write"]["ops_total"] == 3

    def test_time_interval_expansion_adds_ci_columns(self):
        """When querying with smaller interval than stored, CI columns appear."""
        with Environment(lines=0) as env:
            # Create trace with events spread across time
            path = Path(env.temp_dir) / "time_interval_trace.pfw.gz"
            content = (
                "\n".join(
                    [
                        '{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"ph":"X","args":{"ret":4,"hhash":"h1","fhash":"f1"}}',
                        '{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":2000,"dur":10,"ph":"X","args":{"ret":4,"hhash":"h1","fhash":"f1"}}',
                        '{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":3000,"dur":10,"ph":"X","args":{"ret":4,"hhash":"h1","fhash":"f1"}}',
                        '{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":4000,"dur":10,"ph":"X","args":{"ret":4,"hhash":"h1","fhash":"f1"}}',
                    ]
                )
                + "\n"
            )
            with gzip.open(path, "wt", encoding="utf-8") as f:
                f.write(content)
            env.test_files.append(str(path))

            # First, build index with large time interval (5000ms)
            _ = AggregatorUtility().process(
                env.temp_dir,
                index_dir=env.temp_dir,
                force_rebuild=True,
                time_interval_ms=5000.0,
            )

            # Query with smaller interval (1000ms) - should trigger expansion
            result = AggregatorUtility().process(
                env.temp_dir,
                index_dir=env.temp_dir,
                force_rebuild=False,
                time_interval_ms=1000.0,
            )

            pa = pytest.importorskip("pyarrow")
            batches = [pa.record_batch(batch) for batch in result.batches()]
            assert len(batches) > 0

            # CI columns should be present when expansion happened
            schema_names = set(batches[0].schema.names)
            assert "count_ci_lower" in schema_names, (
                f"Missing count_ci_lower. Schema: {schema_names}"
            )
            assert "count_ci_upper" in schema_names, (
                f"Missing count_ci_upper. Schema: {schema_names}"
            )

            # Verify CI values are sensible (upper >= count >= lower)
            rows = pa.Table.from_batches(batches).to_pylist()
            for row in rows:
                assert row["count_ci_lower"] <= row["count"]
                assert row["count_ci_upper"] >= row["count"]

    def test_time_interval_shrink_no_ci_columns(self):
        """When querying with larger interval than stored, no CI columns (lossless)."""
        with Environment(lines=0) as env:
            path = Path(env.temp_dir) / "shrink_trace.pfw.gz"
            content = (
                "\n".join(
                    [
                        '{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"ph":"X","args":{"ret":4,"hhash":"h1","fhash":"f1"}}',
                        '{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":6000,"dur":10,"ph":"X","args":{"ret":4,"hhash":"h1","fhash":"f1"}}',
                    ]
                )
                + "\n"
            )
            with gzip.open(path, "wt", encoding="utf-8") as f:
                f.write(content)
            env.test_files.append(str(path))

            # Build with small interval (1000ms)
            _ = AggregatorUtility().process(
                env.temp_dir,
                index_dir=env.temp_dir,
                force_rebuild=True,
                time_interval_ms=1000.0,
            )

            # Query with larger interval (5000ms) - shrinking is lossless
            result = AggregatorUtility().process(
                env.temp_dir,
                index_dir=env.temp_dir,
                force_rebuild=False,
                time_interval_ms=5000.0,
            )

            pa = pytest.importorskip("pyarrow")
            batches = [pa.record_batch(batch) for batch in result.batches()]
            assert len(batches) > 0

            # CI columns should NOT be present for shrinking (lossless)
            schema_names = set(batches[0].schema.names)
            assert "count_ci_lower" not in schema_names
            assert "count_ci_upper" not in schema_names

    def test_process_with_query_filter(self):
        """Query parameter filters aggregation results."""
        with Environment(lines=0) as env:
            path = Path(env.temp_dir) / "query_trace.pfw.gz"
            content = (
                "\n".join(
                    [
                        '{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"ph":"X","args":{"ret":4,"hhash":"h1","fhash":"f1"}}',
                        '{"name":"compute","cat":"APP","pid":1,"tid":1,"ts":2000,"dur":20,"ph":"X","args":{"ret":8,"hhash":"h2","fhash":"f2"}}',
                        '{"name":"write","cat":"POSIX","pid":1,"tid":1,"ts":3000,"dur":15,"ph":"X","args":{"ret":6,"hhash":"h3","fhash":"f3"}}',
                    ]
                )
                + "\n"
            )
            with gzip.open(path, "wt", encoding="utf-8") as f:
                f.write(content)
            env.test_files.append(str(path))

            # Query for POSIX only
            result = AggregatorUtility().process(
                env.temp_dir,
                index_dir=env.temp_dir,
                force_rebuild=True,
                query='cat == "POSIX"',
            )

            pa = pytest.importorskip("pyarrow")
            batches = [pa.record_batch(batch) for batch in result.batches()]
            rows = pa.Table.from_batches(batches).to_pylist()

            # Should only have POSIX entries
            assert len(rows) == 2
            assert all(row["cat"] == "POSIX" for row in rows)

    def test_iter_arrow_with_query_filter(self):
        """Query parameter filters streaming results."""
        with Environment(lines=0) as env:
            path = Path(env.temp_dir) / "iter_query_trace.pfw.gz"
            content = (
                "\n".join(
                    [
                        '{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"ph":"X","args":{"ret":4,"hhash":"h1","fhash":"f1"}}',
                        '{"name":"compute","cat":"APP","pid":1,"tid":1,"ts":2000,"dur":20,"ph":"X","args":{"ret":8,"hhash":"h2","fhash":"f2"}}',
                    ]
                )
                + "\n"
            )
            with gzip.open(path, "wt", encoding="utf-8") as f:
                f.write(content)
            env.test_files.append(str(path))

            util = AggregatorUtility()
            batches = list(
                util.iter_arrow(
                    env.temp_dir,
                    index_dir=env.temp_dir,
                    force_rebuild=True,
                    query='cat == "APP"',
                )
            )

            pa = pytest.importorskip("pyarrow")
            pa_batches = [pa.record_batch(b) for b in batches]
            rows = pa.Table.from_batches(pa_batches).to_pylist()

            # Should only have APP entries
            assert len(rows) == 1
            assert rows[0]["cat"] == "APP"

    def test_write_arrow_creates_files(self):
        """write_arrow creates Arrow IPC files."""
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            output_dir = Path(env.temp_dir) / "arrow_output"

            result = AggregatorUtility().write_arrow(
                env.temp_dir,
                str(output_dir),
                index_dir=env.temp_dir,
                force_rebuild=True,
            )

            assert "views" in result
            assert "all" in result["views"]
            assert result["views"]["all"]["rows"] > 0
            assert len(result["views"]["all"]["files"]) > 0

            # Verify files are readable
            ipc = pytest.importorskip("pyarrow.ipc")
            first_file = result["views"]["all"]["files"][0]
            with ipc.open_file(first_file) as f:
                assert f.num_record_batches > 0

    def test_write_arrow_with_views(self):
        """write_arrow with views creates filtered outputs."""
        with Environment(lines=0) as env:
            path = Path(env.temp_dir) / "views_trace.pfw.gz"
            content = (
                "\n".join(
                    [
                        '{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"ph":"X","args":{"ret":4,"hhash":"h1","fhash":"f1"}}',
                        '{"name":"compute","cat":"APP","pid":1,"tid":1,"ts":2000,"dur":20,"ph":"X","args":{"ret":8,"hhash":"h2","fhash":"f2"}}',
                        '{"name":"write","cat":"POSIX","pid":1,"tid":1,"ts":3000,"dur":15,"ph":"X","args":{"ret":6,"hhash":"h3","fhash":"f3"}}',
                    ]
                )
                + "\n"
            )
            with gzip.open(path, "wt", encoding="utf-8") as f:
                f.write(content)
            env.test_files.append(str(path))

            output_dir = Path(env.temp_dir) / "views_output"

            result = AggregatorUtility().write_arrow(
                env.temp_dir,
                str(output_dir),
                index_dir=env.temp_dir,
                force_rebuild=True,
                views=[
                    {"name": "io", "query": 'cat == "POSIX"'},
                    {"name": "compute", "query": 'cat == "APP"'},
                ],
            )

            assert "io" in result["views"]
            assert "compute" in result["views"]
            assert result["views"]["io"]["rows"] == 2
            assert result["views"]["compute"]["rows"] == 1

            # Verify view directories exist
            assert (output_dir / "io").exists()
            assert (output_dir / "compute").exists()

            # Verify content is filtered correctly
            pa = pytest.importorskip("pyarrow")
            ipc = pytest.importorskip("pyarrow.ipc")

            io_files = list((output_dir / "io").glob("*.arrow"))
            assert len(io_files) > 0
            with ipc.open_file(str(io_files[0])) as f:
                table = pa.Table.from_batches([f.get_batch(i) for i in range(f.num_record_batches)])
                assert all(row["cat"] == "POSIX" for row in table.to_pylist())

    def test_write_arrow_compression(self):
        """write_arrow respects compression setting."""
        with Environment(lines=20) as env:
            env.create_test_gzip_file()

            # Write with no compression
            output_none = Path(env.temp_dir) / "arrow_none"
            result_none = AggregatorUtility().write_arrow(
                env.temp_dir,
                str(output_none),
                index_dir=env.temp_dir,
                force_rebuild=True,
                compression="none",
            )

            # Write with zstd compression
            output_zstd = Path(env.temp_dir) / "arrow_zstd"
            result_zstd = AggregatorUtility().write_arrow(
                env.temp_dir,
                str(output_zstd),
                index_dir=env.temp_dir,
                force_rebuild=False,
                compression="zstd",
            )

            # Both should have same row count
            assert result_none["total_rows"] == result_zstd["total_rows"]

            # Compressed should be smaller (or at least both readable)
            ipc = pytest.importorskip("pyarrow.ipc")

            for result in [result_none, result_zstd]:
                first_file = result["views"]["all"]["files"][0]
                with ipc.open_file(first_file) as f:
                    assert f.num_record_batches > 0
