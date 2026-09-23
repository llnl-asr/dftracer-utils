#!/usr/bin/env python3
"""The extended aggregate ops (argmin, bit_or, distinct, the ordered-list and
sketch ops, and the two-variable co-moments) through the columnar group-by
surface."""

import pytest

pa = pytest.importorskip("pyarrow")

from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402
from dftracer.utils.columnar import F  # noqa: E402
from dftracer.utils.dataframe import _dataframe_from_arrow  # noqa: E402

pytestmark = pytest.mark.skipif(
    not hasattr(_ext, "_series_from_arrow"),
    reason="extension built without the vec column binding (needs Arrow)",
)


def _sample():
    # Two groups (g0 = rows 0,2,4,6; g1 = rows 1,3,5,7) over a `by` ordering
    # column, a name/alt String pair, a one-hot bit column, a repeating tag
    # column and y = 2*by + 1 (an exact linear fit against `by`).
    return _dataframe_from_arrow(
        pa.table(
            {
                "cat": ["g0", "g1", "g0", "g1", "g0", "g1", "g0", "g1"],
                "by": pa.array([5, 1, 2, 8, 9, 3, 4, 7], pa.int64()),
                "bits": pa.array([1, 2, 4, 8, 16, 32, 64, 128], pa.int64()),
                "yv": pa.array([11, 3, 5, 17, 19, 7, 9, 15], pa.int64()),
                "name": ["a", "b", "c", "d", "e", "f", "g", "h"],
                "tag": ["x", "x", "x", "y", "y", "z", "x", "w"],
            }
        )
    )


def _rows(frame, value):
    return {r["cat"]: r[value] for r in frame.to_arrow().to_pylist()}


def test_argmin_and_argmax():
    df = _sample()
    lo = _rows(df.group_by("cat").agg(F.name.argmin(F.by).alias("v")), "v")
    hi = _rows(df.group_by("cat").agg(F.name.argmax(F.by).alias("v")), "v")
    assert lo == {"g0": "c", "g1": "b"}
    assert hi == {"g0": "e", "g1": "d"}


def _tied_by_frame(by):
    return _dataframe_from_arrow(
        pa.table(
            {
                "cat": ["g", "g", "g"],
                "by": pa.array(by, pa.int64()),
                "name": ["b", "a", "c"],
                "alt": ["B", "Z", "A"],
            }
        )
    )


def _argmin_row(df):
    out = df.group_by("cat").agg(
        F.name.argmin(F.by).alias("min_name"),
        F.alt.argmin(F.by).alias("min_alt"),
    )
    return out.to_arrow().to_pylist()[0]


def test_argmin_specs_sharing_a_by_column_return_one_row():
    # Row 1 minimizes `by`; both specs must report that one row, so the second
    # reads "Z" there rather than its own smallest repr ("A" at row 2).
    row = _argmin_row(_tied_by_frame([3, 1, 2]))
    assert row["min_name"] == "a"
    assert row["min_alt"] == "Z"


def test_argmin_breaks_a_by_tie_on_the_first_row_seen():
    # Every `by` ties, so the row seen first wins (matching the View fold both
    # engines are compared against): row 0, not the smallest repr.
    row = _argmin_row(_tied_by_frame([1, 1, 1]))
    assert row["min_name"] == "b"
    assert row["min_alt"] == "B"


def test_bit_or():
    df = _sample()
    got = _rows(df.group_by("cat").agg(F.bits.bit_or().alias("v")), "v")
    assert got == {"g0": 1 | 4 | 16 | 64, "g1": 2 | 8 | 32 | 128}


def test_prod():
    df = _sample()
    got = _rows(df.group_by("cat").agg(F.by.prod().alias("v")), "v")
    assert got == {"g0": 5 * 2 * 9 * 4, "g1": 1 * 8 * 3 * 7}


def test_distinct_is_exact_below_k():
    df = _sample()
    got = _rows(df.group_by("cat").agg(F.name.distinct().alias("v")), "v")
    assert got == {"g0": 4, "g1": 4}


def test_distinct_estimates_beyond_k():
    n = 2000
    df = _dataframe_from_arrow(pa.table({"cat": ["g"] * n, "v": [f"v{i}" for i in range(n)]}))
    est = df.group_by("cat").agg(F.v.distinct(256).alias("d")).to_arrow().to_pylist()[0]["d"]
    assert n // 2 < est < n * 2


def test_list_sorted_top_k_and_bottom_k():
    df = _sample()
    ordered = _rows(df.group_by("cat").agg(F.name.list_sorted(F.by).alias("v")), "v")
    assert ordered == {"g0": ["c", "g", "a", "e"], "g1": ["b", "f", "h", "d"]}

    top = _rows(df.group_by("cat").agg(F.name.topk(F.by, 2).alias("v")), "v")
    assert top == {"g0": ["e", "a"], "g1": ["d", "h"]}

    bottom = _rows(df.group_by("cat").agg(F.name.bottomk(F.by, 2).alias("v")), "v")
    assert bottom == {"g0": ["c", "g"], "g1": ["b", "f"]}


def test_approx_topk_counts_heavy_hitters():
    df = _sample()
    got = _rows(df.group_by("cat").agg(F.tag.approx_topk(4).alias("v")), "v")
    assert got["g0"] == [{"value": "x", "count": 3}, {"value": "y", "count": 1}]
    assert got["g1"] == [
        {"value": "w", "count": 1},
        {"value": "x", "count": 1},
        {"value": "y", "count": 1},
        {"value": "z", "count": 1},
    ]


def test_sample_keeps_distinct_values_sorted():
    df = _sample()
    got = _rows(df.group_by("cat").agg(F.tag.sample(8).alias("v")), "v")
    assert got == {"g0": ["x", "y"], "g1": ["w", "x", "y", "z"]}

    n = 500
    wide = _dataframe_from_arrow(pa.table({"cat": ["g"] * n, "v": [f"v{i}" for i in range(n)]}))
    kept = wide.group_by("cat").agg(F.v.sample(16).alias("s")).to_arrow().to_pylist()[0]["s"]
    assert len(kept) == 16
    assert kept == sorted(kept)


def test_co_moments_on_an_exact_linear_fit():
    df = _sample()
    out = df.group_by("cat").agg(
        F.yv.corr(F.by).alias("corr"),
        F.yv.covar_pop(F.by).alias("covar_pop"),
        F.yv.covar_samp(F.by).alias("covar_samp"),
        F.yv.regr_slope(F.by).alias("slope"),
        F.yv.regr_intercept(F.by).alias("intercept"),
        F.yv.regr_r2(F.by).alias("r2"),
    )
    rows = {r["cat"]: r for r in out.to_arrow().to_pylist()}
    for cat in ("g0", "g1"):
        assert rows[cat]["corr"] == pytest.approx(1.0)
        assert rows[cat]["slope"] == pytest.approx(2.0)
        assert rows[cat]["intercept"] == pytest.approx(1.0)
        assert rows[cat]["r2"] == pytest.approx(1.0)
    assert rows["g0"]["covar_pop"] == pytest.approx(13.0)
    assert rows["g1"]["covar_pop"] == pytest.approx(16.375)
    assert rows["g0"]["covar_samp"] == pytest.approx(52.0 / 3.0)
    assert rows["g1"]["covar_samp"] == pytest.approx(65.5 / 3.0)


def test_co_moments_are_zero_below_two_rows():
    df = _dataframe_from_arrow(
        pa.table(
            {
                "cat": ["g"],
                "by": pa.array([3], pa.int64()),
                "yv": pa.array([7], pa.int64()),
            }
        )
    )
    row = (
        df.group_by("cat")
        .agg(
            F.yv.corr(F.by).alias("corr"),
            F.yv.covar_samp(F.by).alias("covar_samp"),
            F.yv.regr_slope(F.by).alias("slope"),
        )
        .to_arrow()
        .to_pylist()[0]
    )
    assert row["corr"] == 0.0
    assert row["covar_samp"] == 0.0
    assert row["slope"] == 0.0
