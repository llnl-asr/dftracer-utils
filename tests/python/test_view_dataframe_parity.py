#!/usr/bin/env python3
"""Op parity between the two surfaces.

Every op that exists on both the lazy ``View`` (``TraceViewer``, applied in the
plan) and the materialized ``vec`` batch (``VecBatch``, applied after collect)
must return the same result. This file pins that contract: for each op it runs
``view.<op>(...).collect()`` and ``view.collect().<op>(...)`` and asserts the two
batches are identical column-for-column.
"""

import pytest

pa = pytest.importorskip("pyarrow")
pytest.importorskip("numpy")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import TraceViewer  # noqa: E402

from .common import Environment  # noqa: E402


@pytest.fixture(scope="module")
def indexed_trace():
    with Environment(lines=400) as env:
        gz = env.create_test_gzip_file()
        with dft.Indexer(files=[gz]) as ix:
            ix.ensure_indexed()
        yield gz


def _view(gz):
    return TraceViewer(gz).group_by("cat").agg("count", "sum:dur", "mean:dur")


def _dict(batch):
    return batch.to_arrow().to_pydict()


# (op-name, view-call, vec-call) - each call takes the built object and returns
# a VecBatch, one via the View plan and one on the materialized batch.
PARITY_OPS = [
    ("sort_by-count-asc", lambda v: v.sort_by("count"), lambda b: b.sort_by("count")),
    (
        "sort_by-count-desc",
        lambda v: v.sort_by("count", descending=True),
        lambda b: b.sort_by("count", descending=True),
    ),
    (
        "sort_by-sum_dur-desc",
        lambda v: v.sort_by("sum_dur", descending=True),
        lambda b: b.sort_by("sum_dur", descending=True),
    ),
    (
        "topk-count-largest-1",
        lambda v: v.topk("count", 1),
        lambda b: b.topk("count", 1, largest=True),
    ),
    (
        "topk-count-largest-2",
        lambda v: v.topk("count", 2),
        lambda b: b.topk("count", 2, largest=True),
    ),
    (
        "topk-sum_dur-smallest-2",
        lambda v: v.topk("sum_dur", 2, largest=False),
        lambda b: b.topk("sum_dur", 2, largest=False),
    ),
]


@pytest.mark.parametrize("name,view_op,vec_op", PARITY_OPS, ids=[o[0] for o in PARITY_OPS])
def test_view_vec_op_parity(indexed_trace, name, view_op, vec_op):
    on_view = view_op(_view(indexed_trace)).collect().collect()
    on_vec = vec_op(_view(indexed_trace).collect().collect())
    assert _dict(on_view) == _dict(on_vec)


def test_view_vec_chained_parity(indexed_trace):
    # A chain of ops must also agree end to end.
    on_view = (
        _view(indexed_trace)
        .sort_by("sum_dur", descending=True)
        .topk("count", 2)
        .collect()
        .collect()
    )
    on_vec = (
        _view(indexed_trace)
        .collect()
        .collect()
        .sort_by("sum_dur", descending=True)
        .topk("count", 2)
    )
    assert _dict(on_view) == _dict(on_vec)
