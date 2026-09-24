#!/usr/bin/env python3
"""TraceViewer.agg accepts unified ``F`` aggregate expressions, additive to the
legacy spec strings. Each ``F.<field>.<op>()`` matches its ``"op:field"`` string
form, and the ``F.any`` wildcard maps to the numeric-args path."""

import pytest

pa = pytest.importorskip("pyarrow")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import TraceViewer  # noqa: E402
from dftracer.utils.columnar import F  # noqa: E402

from .common import Environment  # noqa: E402


@pytest.fixture(scope="module")
def indexed_trace():
    with Environment(lines=400) as env:
        gz = env.create_test_gzip_file()
        with dft.Indexer(files=[gz]) as ix:
            ix.ensure_indexed()
        yield gz


def _dict(viewer):
    return viewer.collect().to_arrow().to_pydict()


def test_field_expr_agg_matches_string_form(indexed_trace):
    gz = indexed_trace
    by_expr = _dict(
        TraceViewer(gz).group_by("cat").agg(F.dur.sum(), F("dur").mean(), F.dur.count())
    )
    by_str = _dict(TraceViewer(gz).group_by("cat").agg("sum:dur", "mean:dur", "count"))
    assert by_expr == by_str
    assert set(by_expr) == {"cat", "sum_dur", "mean_dur", "count"}


def test_field_expr_agg_mixes_with_strings(indexed_trace):
    gz = indexed_trace
    mixed = _dict(TraceViewer(gz).group_by("cat").agg("count", F.dur.sum()))
    ref = _dict(TraceViewer(gz).group_by("cat").agg("count", "sum:dur"))
    assert mixed == ref


def test_any_wildcard_count(indexed_trace):
    gz = indexed_trace
    got = _dict(TraceViewer(gz).group_by("cat").agg(F.any.count()))
    assert got == _dict(TraceViewer(gz).group_by("cat").agg("count"))


def test_any_wildcard_mean_aggregates_numeric_args(indexed_trace):
    gz = indexed_trace
    # F.any.mean() == agg_numeric_args(): every numeric args.* field (here `ret`)
    # gets a per-group mean column.
    got = _dict(TraceViewer(gz).group_by("cat").agg(F.any.mean()))
    ref = _dict(TraceViewer(gz).group_by("cat").agg_numeric_args())
    assert got == ref
    # The wildcard discovered at least one numeric args.* mean column.
    assert set(got) - {"cat", "count"}


def test_any_wildcard_sum_aggregates_numeric_args(indexed_trace):
    gz = indexed_trace
    # F.any.sum() now applies a sum to every numeric args.* field, one
    # sum_<arg> column each (the bare name stays reserved for the legacy mean).
    got = _dict(TraceViewer(gz).group_by("cat").agg(F.any.sum()))
    assert any(c.startswith("sum_") for c in got)


def test_agg_numeric_args_multiple_reductions(indexed_trace):
    gz = indexed_trace
    got = _dict(TraceViewer(gz).group_by("cat").agg_numeric_args("sum", "max", "mean"))
    # Each discovered numeric arg emits one <op>_<arg> column per reduction.
    assert any(c.startswith("sum_") for c in got)
    assert any(c.startswith("max_") for c in got)
    assert any(c.startswith("mean_") for c in got)


def test_agg_numeric_args_percentile(indexed_trace):
    gz = indexed_trace
    # A per-arg percentile collects a per-arg sketch; pNN shorthand names the
    # column p<NN>_<arg>.
    got = _dict(TraceViewer(gz).group_by("cat").agg_numeric_args("p90", "mean"))
    assert any(c.startswith("p90_") for c in got)
    assert any(c.startswith("mean_") for c in got)
