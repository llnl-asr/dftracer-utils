"""Staleness detection: stat-only (mtime + size) checks and end-to-end refresh.

Covers the IndexDatabase.find_stale_files binding and verifies that a full
aggregation run rebuilds only the traces whose source files changed.
"""

import os

import pytest

import dftracer.utils as dft_utils
from dftracer.utils.dftracer_utils_ext import AggregatorUtility, IndexDatabase

from .common import Environment, determine_index_path


def _build_index(directory):
    with dft_utils.Indexer(directory) as indexer:
        indexer.ensure_indexed()


def _sum_event_count(table):
    pa = pytest.importorskip("pyarrow")
    batches = [pa.record_batch(batch) for batch in table.batches()]
    rows = pa.Table.from_batches(batches).to_pylist()
    return sum(row["count"] for row in rows)


class TestFindStaleFiles:
    def test_fresh_index_reports_nothing_stale(self):
        with Environment(lines=20) as env:
            a = env.create_dft_trace_file("a.pfw.gz")
            b = env.create_dft_trace_file("b.pfw.gz")
            _build_index(env.temp_dir)

            db = IndexDatabase(determine_index_path(a))
            res = db.find_stale_files([a, b])

            assert set(res.keys()) == {
                "changed",
                "added",
                "removed",
                "schema_outdated",
                "stale",
            }
            assert res["stale"] is False
            assert res["changed"] == []
            assert res["added"] == []
            assert res["removed"] == []
            assert res["schema_outdated"] is False

    def test_detects_changed_file(self):
        with Environment(lines=20) as env:
            a = env.create_dft_trace_file("a.pfw.gz")
            b = env.create_dft_trace_file("b.pfw.gz")
            _build_index(env.temp_dir)

            # Rewrite b with a different size -> stat-only check flags it.
            env.create_dft_trace_file("b.pfw.gz", num_events=60)

            db = IndexDatabase(determine_index_path(a))
            res = db.find_stale_files([a, b])
            assert res["stale"] is True
            assert res["changed"] == [b]
            assert res["added"] == []
            assert res["removed"] == []

    def test_detects_added_file(self):
        with Environment(lines=20) as env:
            a = env.create_dft_trace_file("a.pfw.gz")
            _build_index(env.temp_dir)

            c = env.create_dft_trace_file("c.pfw.gz")
            db = IndexDatabase(determine_index_path(a))
            res = db.find_stale_files([a, c])
            assert res["added"] == [c]
            assert res["changed"] == []

    def test_detects_removed_file(self):
        with Environment(lines=20) as env:
            a = env.create_dft_trace_file("a.pfw.gz")
            b = env.create_dft_trace_file("b.pfw.gz")
            _build_index(env.temp_dir)

            db = IndexDatabase(determine_index_path(a))
            res = db.find_stale_files([a])
            assert res["removed"] == [os.path.basename(b)]
            assert res["changed"] == []


class TestStaleEndToEnd:
    def test_aggregation_rebuilds_changed_trace(self):
        # Open the index only through the aggregations so no external handle
        # holds the RocksDB lock across runs.
        with Environment(lines=20) as env:
            env.create_dft_trace_file("a.pfw.gz", num_events=20)
            env.create_dft_trace_file("b.pfw.gz", num_events=20)
            directory = env.temp_dir

            first = _sum_event_count(AggregatorUtility().process(directory))
            assert first == 40

            # Grow b; the stat-only check must flag it stale on the next run.
            env.create_dft_trace_file("b.pfw.gz", num_events=80)
            second = _sum_event_count(AggregatorUtility().process(directory))

            # a stays cached (20), b is rebuilt with its new content (80). If
            # staleness were missed, b would be served as the stale 20 -> 40.
            assert second == 100
