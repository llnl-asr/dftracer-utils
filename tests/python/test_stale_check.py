"""Staleness detection: stat-only (mtime + size) checks and end-to-end refresh.

Covers the IndexDatabase.find_stale_files binding and verifies that a full
aggregation run rebuilds only the traces whose source files changed.
"""

import dftracer.utils as dftu_utils
from dftracer.utils.dftracer_utils_ext import IndexDatabase

from .common import Environment, determine_index_path


def _build_index(directory):
    with dftu_utils.Indexer(directory) as indexer:
        indexer.ensure_indexed()


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
                "format_outdated",
                "stale",
            }
            assert res["stale"] is False
            assert res["changed"] == []
            assert res["added"] == []
            assert res["removed"] == []
            assert res["format_outdated"] is False

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
            # The registry now keys on the full canonical path.
            assert res["removed"] == [b]
            assert res["changed"] == []
