# SPDX-License-Identifier: MIT
"""``Expr.arg_max`` / ``Expr.arg_min`` against Polars: the position within its
group of the first extreme present value, null for a group with no present
value, a NaN not present, exact for integers a double cannot tell apart."""

import pytest

import dftracer.utils as dft

pytest.importorskip("dftracer.utils.dftracer_utils_ext")
pl = pytest.importorskip("polars")

NAN = float("nan")
BIG = 4611686018427387904

CASES = {
    "ints_ties": ([0, 1, 0, 0, 1, 2, 2], [3, 5, 9, 9, 5, None, None]),
    "unsorted_keys": ([2, 0, 1, 0, 2, 1, 0], [4, 1, 7, -1, 4, 7, 1]),
    "floats_nan": ([0, 0, 0, 0, 1, 2, 2], [NAN, 2.0, NAN, 1.0, NAN, 4.0, 4.0]),
    "big_ints": ([0, 0, 1, 1], [BIG, BIG + 1, BIG + 1, BIG]),
    "one_row_groups": ([3, 1, 2], [1, None, 5]),
    "negatives": ([0, 0, 0], [-5, -9, -5]),
}


def _frames(keys, vals):
    return dft.DataFrame({"k": keys, "v": vals}), pl.DataFrame({"k": keys, "v": vals})


@pytest.mark.parametrize("name", CASES)
@pytest.mark.parametrize("method", ["arg_max", "arg_min"])
def test_group_by_matches_polars(name, method):
    keys, vals = CASES[name]
    ours, ref = _frames(keys, vals)
    got = ours.group_by("k").agg(getattr(dft.col("v"), method)().alias("r")).sort("k")
    want = ref.group_by("k").agg(getattr(pl.col("v"), method)().alias("r")).sort("k")
    assert got["k"].to_list() == want["k"].to_list()
    assert got["r"].to_list() == want["r"].to_list()
    assert got["r"].dtype == dft.DType.INT64


@pytest.mark.parametrize("name", CASES)
@pytest.mark.parametrize("method", ["arg_max", "arg_min"])
def test_whole_column_is_the_row_position(name, method):
    keys, vals = CASES[name]
    ours, ref = _frames(keys, vals)
    got = ours.select(getattr(dft.col("v"), method)().alias("r"))
    want = ref.select(getattr(pl.col("v"), method)().alias("r"))
    assert got["r"].to_list() == want["r"].to_list()


def test_all_null_and_empty_columns_give_null():
    ours = dft.DataFrame({"k": [0, 0], "v": [None, None]}).astype({"v": "int64"})
    got = ours.group_by("k").agg(dft.col("v").arg_max().alias("r"))
    assert got["r"].to_list() == [None]
    empty = dft.DataFrame({"v": []}).astype({"v": "int64"})
    assert empty.select(dft.col("v").arg_max().alias("r"))["r"].to_list() == [None]


def test_two_columns_and_both_directions_in_one_agg():
    keys, vals = CASES["unsorted_keys"]
    other = [v * -1 if v is not None else None for v in vals]
    ours = dft.DataFrame({"k": keys, "v": vals, "w": other})
    ref = pl.DataFrame({"k": keys, "v": vals, "w": other})
    got = (
        ours.group_by("k")
        .agg(
            dft.col("v").arg_max().alias("a"),
            dft.col("v").arg_min().alias("b"),
            dft.col("w").arg_max().alias("c"),
            dft.col("v").sum().alias("s"),
        )
        .sort("k")
    )
    want = (
        ref.group_by("k")
        .agg(
            pl.col("v").arg_max().alias("a"),
            pl.col("v").arg_min().alias("b"),
            pl.col("w").arg_max().alias("c"),
            pl.col("v").sum().alias("s"),
        )
        .sort("k")
    )
    for c in ("a", "b", "c", "s"):
        assert got[c].to_list() == want[c].to_list()


def test_a_large_frame_takes_the_ordered_single_state_and_matches_polars():
    n = 1_500_000
    keys = [(i * 7919) % 997 for i in range(n)]
    vals = [(i * 104729) % 100_003 for i in range(n)]
    ours, ref = _frames(keys, vals)
    for method in ("arg_max", "arg_min"):
        got = ours.group_by("k").agg(getattr(dft.col("v"), method)().alias("r")).sort("k")
        want = ref.group_by("k").agg(getattr(pl.col("v"), method)().alias("r")).sort("k")
        assert got["r"].to_list() == want["r"].to_list()


def test_lazy_string_spec_and_a_text_column():
    keys, vals = CASES["ints_ties"]
    ours, ref = _frames(keys, vals)
    got = ours.lazy().group_by("k").agg("arg_max:v:r").collect().sort("k")
    want = ref.group_by("k").agg(pl.col("v").arg_max().alias("r")).sort("k")
    assert got["r"].to_list() == want["r"].to_list()
    text = dft.DataFrame({"k": [0, 0], "v": ["a", "b"]})
    with pytest.raises(Exception, match="arg_max/arg_min"):
        text.group_by("k").agg(dft.col("v").arg_max().alias("r"))


def test_a_trace_aggregate_refuses_a_position():
    from dftracer.utils.trace import partials

    for method in ("arg_max", "arg_min"):
        with pytest.raises(ValueError, match="no meaning across trace files"):
            partials.agg_specs([getattr(dft.col("dur"), method)()])
