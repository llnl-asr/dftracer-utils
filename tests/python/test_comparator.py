"""Tests for ComparatorUtility."""

import gzip
import json
import os

from dftracer.utils.arrow import ArrowTable
from dftracer.utils.dftracer_utils_ext import ComparatorUtility

from .common import Environment


def _create_posix_trace(env, filename="posix_trace.pfw.gz", num_events=40):
    """Create a trace file with POSIX/STDIO categories that the comparator default query matches."""
    file_path = os.path.join(env.temp_dir, filename)
    posix_ops = ["read", "write", "open", "close", "stat", "lseek"]
    stdio_ops = ["fread", "fwrite", "fopen", "fclose"]
    lines = []
    for i in range(num_events):
        if i % 3 == 0:
            cat = "STDIO"
            name = stdio_ops[i % len(stdio_ops)]
        else:
            cat = "POSIX"
            name = posix_ops[i % len(posix_ops)]
        dur = (i + 1) * 100
        size = (i + 1) * 512
        pid = i % 2
        tid = i % 4
        ts = i * 5000
        line = (
            f'{{"name":"{name}","cat":"{cat}","pid":{pid},"tid":{tid},'
            f'"ts":{ts},"dur":{dur},"ph":"X",'
            f'"args":{{"ret":{size}}}}}\n'
        )
        lines.append(line)

    with gzip.open(file_path, "wt", encoding="utf-8") as f:
        f.write("[\n")
        f.writelines(lines)
        f.write("]\n")

    env.test_files.append(file_path)
    return file_path


class TestComparatorCompare:
    def test_compare_returns_arrow_table(self):
        with Environment(lines=20) as env:
            gz_file = _create_posix_trace(env)
            result = ComparatorUtility().compare(gz_file, gz_file)
            assert isinstance(result, ArrowTable)

    def test_compare_has_rows(self):
        with Environment(lines=20) as env:
            gz_file = _create_posix_trace(env)
            result = ComparatorUtility().compare(gz_file, gz_file)
            assert result.num_rows > 0

    def test_compare_schema_columns(self):
        with Environment(lines=20) as env:
            gz_file = _create_posix_trace(env)
            result = ComparatorUtility().compare(gz_file, gz_file)
            for batch in result.batches():
                assert hasattr(batch, "__arrow_c_array__")

    def test_compare_same_file_zero_deltas(self):
        with Environment(lines=20) as env:
            gz_file = _create_posix_trace(env)
            result = ComparatorUtility().compare(gz_file, gz_file)
            try:
                import pyarrow as pa

                batches = []
                for batch in result.batches():
                    batches.append(pa.record_batch(batch))
                if batches:
                    table = pa.Table.from_batches(batches)
                    delta_col = table.column("delta")
                    for val in delta_col:
                        assert val.as_py() == 0.0 or val.as_py() is None
            except ImportError:
                pass  # pyarrow not available, skip detailed check

    def test_compare_directory(self):
        with Environment(lines=20) as env:
            _create_posix_trace(env, "a.pfw.gz")
            _create_posix_trace(env, "b.pfw.gz")
            directory = env.temp_dir
            result = ComparatorUtility().compare(directory, directory)
            assert isinstance(result, ArrowTable)
            assert result.num_rows > 0

    def test_call_delegates_to_compare(self):
        with Environment(lines=10) as env:
            gz_file = _create_posix_trace(env)
            util = ComparatorUtility()
            result = util(gz_file, gz_file)
            assert isinstance(result, ArrowTable)


class TestComparatorCompareJson:
    def test_compare_json_returns_string(self):
        with Environment(lines=20) as env:
            gz_file = _create_posix_trace(env)
            result = ComparatorUtility().compare_json(gz_file, gz_file)
            assert isinstance(result, str)

    def test_compare_json_valid_json(self):
        with Environment(lines=20) as env:
            gz_file = _create_posix_trace(env)
            result = ComparatorUtility().compare_json(gz_file, gz_file)
            parsed = json.loads(result)
            assert isinstance(parsed, dict)

    def test_compare_json_has_expected_keys(self):
        with Environment(lines=20) as env:
            gz_file = _create_posix_trace(env)
            result = ComparatorUtility().compare_json(gz_file, gz_file)
            parsed = json.loads(result)
            assert "baseline" in parsed
            assert "nodes" in parsed

    def test_compare_json_same_file_zero_pct_change(self):
        with Environment(lines=20) as env:
            gz_file = _create_posix_trace(env)
            result = ComparatorUtility().compare_json(gz_file, gz_file)
            parsed = json.loads(result)
            for node in parsed.get("nodes", []):
                summary = node.get("summary", {})
                for metric in summary.get("metrics", []):
                    assert metric["pct_change"] == 0.0


class TestComparatorCompareTable:
    def test_compare_table_returns_string(self):
        with Environment(lines=20) as env:
            gz_file = _create_posix_trace(env)
            result = ComparatorUtility().compare_table(gz_file, gz_file)
            assert isinstance(result, str)

    def test_compare_table_has_content(self):
        with Environment(lines=20) as env:
            gz_file = _create_posix_trace(env)
            result = ComparatorUtility().compare_table(gz_file, gz_file)
            assert len(result) > 0

    def test_compare_table_contains_expected_text(self):
        with Environment(lines=20) as env:
            gz_file = _create_posix_trace(env)
            result = ComparatorUtility().compare_table(gz_file, gz_file)
            result_lower = result.lower()
            assert "count" in result_lower
            assert "baseline" in result_lower or "summary" in result_lower
