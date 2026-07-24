"""Tests for ComparatorUtility.

Each compare() runs a full build+aggregate, which is very slow under Valgrind.
The cases below only inspect different facets of the same
compare-a-trace-with-itself result, so the aggregations are computed once per
method (module-scoped fixtures) and every case asserts on the cached result.
"""

import gzip
import json
import os

import pytest

from dftracer.utils.arrow import ArrowTable
from dftracer.utils.dftracer_utils_ext import ComparatorUtility

from .common import Environment


def _write_posix_trace(dir_path, filename="posix_trace.pfw.gz", num_events=40):
    """Write a POSIX/STDIO trace the comparator default query matches."""
    file_path = os.path.join(dir_path, filename)
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
    return file_path


def _create_posix_trace(env, filename="posix_trace.pfw.gz", num_events=40):
    file_path = _write_posix_trace(env.temp_dir, filename, num_events)
    env.test_files.append(file_path)
    return file_path


@pytest.fixture(scope="module")
def shared_gz(tmp_path_factory):
    """One trace reused by every compare-with-self case (built once)."""
    return _write_posix_trace(str(tmp_path_factory.mktemp("comparator_shared")))


@pytest.fixture(scope="module")
def cmp_result(shared_gz):
    """compare() run once; ArrowTable.batches()/num_rows are re-readable."""
    return ComparatorUtility().compare(shared_gz, shared_gz)


@pytest.fixture(scope="module")
def cmp_json(shared_gz):
    return ComparatorUtility().compare_json(shared_gz, shared_gz)


@pytest.fixture(scope="module")
def cmp_table(shared_gz):
    return ComparatorUtility().compare_table(shared_gz, shared_gz)


class TestComparatorCompare:
    def test_compare_returns_arrow_table(self, cmp_result):
        assert isinstance(cmp_result, ArrowTable)

    def test_compare_has_rows(self, cmp_result):
        assert cmp_result.num_rows > 0

    def test_compare_schema_columns(self, cmp_result):
        for batch in cmp_result.batches():
            assert hasattr(batch, "__arrow_c_array__")

    def test_compare_same_file_zero_deltas(self, cmp_result):
        try:
            import pyarrow as pa

            batches = [pa.record_batch(batch) for batch in cmp_result.batches()]
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

    def test_call_delegates_to_compare(self, shared_gz):
        util = ComparatorUtility()
        result = util(shared_gz, shared_gz)
        assert isinstance(result, ArrowTable)


class TestComparatorCompareJson:
    def test_compare_json_returns_string(self, cmp_json):
        assert isinstance(cmp_json, str)

    def test_compare_json_valid_json(self, cmp_json):
        parsed = json.loads(cmp_json)
        assert isinstance(parsed, dict)

    def test_compare_json_has_expected_keys(self, cmp_json):
        parsed = json.loads(cmp_json)
        assert "baseline" in parsed
        assert "nodes" in parsed

    def test_compare_json_same_file_zero_pct_change(self, cmp_json):
        parsed = json.loads(cmp_json)
        for node in parsed.get("nodes", []):
            summary = node.get("summary", {})
            for metric in summary.get("metrics", []):
                assert metric["pct_change"] == 0.0


class TestComparatorCompareTable:
    def test_compare_table_returns_string(self, cmp_table):
        assert isinstance(cmp_table, str)

    def test_compare_table_has_content(self, cmp_table):
        assert len(cmp_table) > 0

    def test_compare_table_contains_expected_text(self, cmp_table):
        result_lower = cmp_table.lower()
        assert "count" in result_lower
        assert "baseline" in result_lower or "summary" in result_lower
