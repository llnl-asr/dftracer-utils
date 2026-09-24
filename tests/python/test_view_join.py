#!/usr/bin/env python3
"""Tests for TraceViewer.join: the generic LazyFrame join of two aggregated
views on their group key."""

import pyarrow as pa
import pytest

import dftracer.utils as dftu_utils
from dftracer.utils import LazyFrame, TraceViewer

from .common import Environment


def _indexed(env):
    gz = env.create_test_gzip_file()
    with dftu_utils.Indexer(files=[gz]) as indexer:
        indexer.ensure_indexed()
    return gz


def _by_cat(tbl):
    return {row["cat"]: row for row in tbl.to_pylist()}


def _sides(gz):
    left = TraceViewer(gz).group_by("cat").agg("count")
    right = TraceViewer(gz).filter('cat == "POSIX"').group_by("cat").agg("count", "sum:dur")
    return left, right


class TestViewJoin:
    def test_inner_join_carries_both_sides(self):
        with Environment(lines=200) as env:
            gz = _indexed(env)
            left, right = _sides(gz)
            joined = left.join(right, on="cat", how="inner")
            assert isinstance(joined, LazyFrame)
            tbl = pa.table(joined.collect())
            # The clashing right-side column takes the "_right" suffix.
            assert tbl.column_names == ["cat", "count", "count_right", "sum_dur"]
            rows = _by_cat(tbl)
            # Inner keeps only the shared key; the right side filtered to posix.
            assert set(rows) == {"posix"}
            assert rows["posix"]["count"] > 0
            assert rows["posix"]["count_right"] == rows["posix"]["count"]
            assert rows["posix"]["sum_dur"] > 0

    def test_left_join_null_pads_the_right(self):
        with Environment(lines=200) as env:
            gz = _indexed(env)
            left, right = _sides(gz)
            tbl = pa.table(left.join(right, on="cat", how="left").collect())
            rows = _by_cat(tbl)
            assert {"posix", "stdio"} <= set(rows)
            assert rows["posix"]["sum_dur"] is not None  # matched
            assert rows["stdio"]["count"] > 0
            assert rows["stdio"]["count_right"] is None  # left-only row nulls right
            assert rows["stdio"]["sum_dur"] is None

    @pytest.mark.parametrize("how", ["semi", "anti"])
    def test_semi_anti_return_left_columns_only(self, how):
        with Environment(lines=200) as env:
            gz = _indexed(env)
            left, right = _sides(gz)
            tbl = pa.table(left.join(right, on="cat", how=how).collect())
            assert tbl.column_names == ["cat", "count"]
            cats = set(tbl.column("cat").to_pylist())
            assert cats == ({"posix"} if how == "semi" else {"stdio"})

    def test_join_key_missing_on_one_side_raises(self):
        with Environment(lines=200) as env:
            gz = _indexed(env)
            left = TraceViewer(gz).group_by("cat").agg("count")
            right = TraceViewer(gz).group_by("pid").agg("count")
            with pytest.raises(ValueError, match="no right column named cat"):
                _ = left.join(right, on="cat", how="inner").columns

    def test_bad_how_and_bad_other_raise(self):
        with Environment(lines=100) as env:
            gz = _indexed(env)
            left = TraceViewer(gz).group_by("cat").agg("count")
            right = TraceViewer(gz).group_by("cat").agg("count")
            with pytest.raises(ValueError):
                left.join(right, on="cat", how="sideways")
            with pytest.raises(TypeError):
                left.join("not-a-viewer", on="cat")
