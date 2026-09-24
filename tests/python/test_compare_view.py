#!/usr/bin/env python3
"""TraceViewer.compare: a lazy two-viewer comparison over one group_by/agg."""

import math

import pytest

import dftracer.utils as dftu_utils
from dftracer.utils import DFTUtilsValueError, TraceViewer

from .common import Environment


def _indexed(env):
    gz = env.create_test_gzip_file()
    with dftu_utils.Indexer(files=[gz]) as indexer:
        indexer.ensure_indexed()
    return gz


class TestCompare:
    def test_self_compare_is_zero_delta(self):
        with Environment(lines=200) as env:
            gz = _indexed(env)
            base = TraceViewer(gz).group_by("cat").agg("count")
            df = base.compare(TraceViewer(gz)).collect().to_pandas()
            assert {
                "cat",
                "l_count",
                "r_count",
                "delta_count",
                "pct_count",
            } <= set(df.columns)
            assert (df["l_count"] == df["r_count"]).all()
            assert (df["delta_count"] == 0).all()
            assert (df["pct_count"] == 0).all()

    def test_compare_two_traces_computes_delta(self):
        with Environment(lines=200) as base_env, Environment(lines=400) as var_env:
            base_gz = _indexed(base_env)
            var_gz = _indexed(var_env)
            base = TraceViewer(base_gz).group_by("cat").agg("count")
            df = base.compare(TraceViewer(var_gz)).collect().to_pandas()

            # delta and pct are self-consistent with the joined l_/r_ columns.
            assert (df["delta_count"] == df["r_count"] - df["l_count"]).all()
            for _, row in df.iterrows():
                if row["l_count"]:
                    assert math.isclose(
                        row["pct_count"],
                        100.0 * (row["r_count"] - row["l_count"]) / row["l_count"],
                        rel_tol=1e-6,
                    )
            # The variant has more events, so at least one group grew.
            assert (df["delta_count"] > 0).any()

    def test_compare_requires_an_agg_plan(self):
        with Environment(lines=100) as env:
            gz = _indexed(env)
            with pytest.raises(DFTUtilsValueError, match="group_by or agg"):
                TraceViewer(gz).compare(TraceViewer(gz))
