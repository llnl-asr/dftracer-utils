# SPDX-License-Identifier: MIT
"""The exact quantile aggregate and the Expr math nodes: Expr.quantile,
percentile, median, approx_quantile, n_unique, len, null_count, pow, floordiv,
mod, str.find and log1p; one-bound clip, the quantile level check and the
empty-reduction rule."""

import math

import pytest

from dftracer.utils import DataFrame, Series, col

np = pytest.importorskip("numpy")


def _frame(n=200):
    rng = np.random.default_rng(3)
    return DataFrame(
        {
            "k": Series([int(i % 3) for i in range(n)]),
            "x": Series([float(v) for v in rng.normal(size=n)]),
            "i": Series([int(v) for v in rng.integers(0, 50, size=n)]),
        }
    )


@pytest.mark.parametrize(
    "interpolation,numpy_method",
    [
        ("linear", "linear"),
        ("lower", "lower"),
        ("higher", "higher"),
        ("midpoint", "midpoint"),
    ],
)
def test_group_quantile_is_exact(interpolation, numpy_method):
    df = _frame()
    out = df.group_by("k").agg(q=col("x").quantile(0.37, interpolation)).sort_by("k")
    keys = df["k"].to_numpy()
    xs = df["x"].to_numpy()
    want = [np.quantile(xs[keys == g], 0.37, method=numpy_method) for g in (0, 1, 2)]
    assert out["q"].to_list() == pytest.approx(want, rel=1e-12)


@pytest.mark.parametrize("n", [11, 100])
@pytest.mark.parametrize("interpolation", ["linear", "nearest", "lower", "higher", "midpoint"])
def test_group_quantile_equals_series_quantile_at_every_level(n, interpolation):
    values = [float((i * 7) % n) for i in range(n)]
    df = DataFrame({"k": Series([0] * n), "x": Series(values)})
    reference = df["x"]
    levels = [i / 100 for i in range(101)]
    out = df.group_by("k").agg(
        *[col("x").quantile(q, interpolation).alias(f"q{i}") for i, q in enumerate(levels)]
    )
    lazy = (
        df.lazy()
        .group_by("k")
        .agg(*[col("x").quantile(q, interpolation).alias(f"q{i}") for i, q in enumerate(levels)])
        .collect()
    )
    for i, q in enumerate(levels):
        want = reference.quantile(q, interpolation=interpolation)
        assert out[f"q{i}"].to_list() == [want], (n, interpolation, q)
        assert lazy[f"q{i}"].to_list() == [want], (n, interpolation, q)


def test_quantile_default_is_nearest_and_percentile_is_linear():
    df = _frame()
    nearest = df.group_by("k").agg(q=col("i").quantile(0.5)).sort_by("k")["q"].to_list()
    linear = df.group_by("k").agg(q=col("i").quantile(0.5, "linear")).sort_by("k")["q"].to_list()
    pct = df.group_by("k").agg(q=col("i").percentile(50)).sort_by("k")["q"].to_list()
    median = df.group_by("k").agg(q=col("i").median()).sort_by("k")["q"].to_list()
    assert pct == linear == median
    keys = df["k"].to_numpy()
    values = df["i"].to_numpy()
    for g, got in zip((0, 1, 2), nearest):
        group = np.sort(values[keys == g])
        rank = int(math.floor((len(group) - 1) * 0.5 + 0.5))
        assert got == float(group[rank])


def test_quantile_rejects_a_bad_level_and_interpolation():
    with pytest.raises(ValueError):
        col("x").quantile(1.5)
    with pytest.raises(ValueError):
        col("x").quantile(float("nan"))
    with pytest.raises(ValueError):
        col("x").quantile(0.5, "bogus")
    with pytest.raises(ValueError):
        col("x").percentile(101)
    with pytest.raises(ValueError):
        Series([1.0, 2.0]).quantile(-0.1)
    with pytest.raises(ValueError):
        Series([1.0, 2.0]).quantile(float("nan"))


def test_approx_quantile_is_the_sketch_and_exact_differs_from_it():
    df = DataFrame({"k": Series([0] * 1000), "x": Series([float(i) for i in range(1000)])})
    exact = df.group_by("k").agg(q=col("x").quantile(0.9, "linear"))["q"].to_list()[0]
    approx = df.group_by("k").agg(q=col("x").approx_quantile(0.9))["q"].to_list()[0]
    assert exact == pytest.approx(899.1, abs=1e-9)
    assert approx == pytest.approx(899.1, rel=0.05)


def test_group_quantile_with_nulls_and_nan():
    df = DataFrame(
        {
            "k": Series([0, 0, 0, 0, 1]),
            "x": Series([1.0, None, 3.0, float("nan"), None]),
        }
    )
    out = df.group_by("k").agg(q=col("x").quantile(0.5, "linear")).sort_by("k")
    got = out["q"].to_list()
    assert got[0] == 3.0
    assert got[1] is None


def test_lazy_group_quantile_matches_eager_and_takes_interpolation():
    df = _frame()
    lazy = df.lazy().group_by("k").quantile(0.25, "lower").collect()
    eager = df.group_by("k").quantile(0.25, "lower")
    assert lazy.sort_by("k")["x"].to_list() == eager.sort_by("k")["x"].to_list()
    assert df.lazy().group_by("k").median().collect().num_rows == 3


def test_expr_median_n_unique_len_null_count():
    df = DataFrame(
        {
            "k": Series([0, 0, 0, 1, 1]),
            "x": Series([1.0, 2.0, None, 5.0, 5.0]),
        }
    )
    out = (
        df.group_by("k")
        .agg(
            m=col("x").median(),
            u=col("x").n_unique(),
            n=col("x").len(),
            z=col("x").null_count(),
        )
        .sort_by("k")
    )
    assert out["m"].to_list() == [1.5, 5.0]
    assert out["u"].to_list() == [2, 1]
    assert out["n"].to_list() == [3, 2]
    assert out["z"].to_list() == [1, 0]


def test_keyless_reduction_of_an_empty_frame_is_one_row():
    df = DataFrame({"a": Series([1, 2, 3])}).filter(Series([False, False, False]))
    out = df.sum()
    assert out.num_rows == 1
    assert out["a"].to_list() == [0]
    mean = df.mean()
    assert mean.num_rows == 1
    assert mean["a"].to_list() in ([None], [float("nan")]) or math.isnan(mean["a"].to_list()[0])


def test_expr_pow_floordiv_mod():
    df = DataFrame({"a": Series([7, -7, 9, 2]), "b": Series([2, 2, -4, 3])})
    assert (col("a") // col("b")).apply(df).to_list() == [3, -4, -3, 0]
    assert (col("a") % col("b")).apply(df).to_list() == [1, 1, -3, 2]
    assert (col("a") // 2).apply(df).to_list() == [3, -4, 4, 1]
    assert (col("a") % 4).apply(df).to_list() == [3, 1, 1, 2]
    assert (100 // col("b")).apply(df).to_list() == [50, 50, -25, 33]
    assert (col("b") ** 2).apply(df).to_list() == [4, 4, 16, 9]
    pos = DataFrame({"b": Series([2, 2, 4, 3])})
    assert (2 ** col("b")).apply(pos).to_list() == [4, 4, 16, 8]
    assert col("b").pow(col("b")).apply(pos).to_list() == [4, 4, 256, 27]
    assert col("a").floordiv(2).apply(df).to_list() == [3, -4, 4, 1]
    assert col("a").mod(col("b")).apply(df).to_list() == [1, 1, -3, 2]


def test_expr_str_find_is_null_on_a_miss_and_series_keeps_minus_one():
    df = DataFrame({"s": Series(["abcabc", "xyz", None, "bc"])})
    assert col("s").str.find("bc").apply(df).to_list() == [1, None, None, 0]
    assert df["s"].str.find("bc").to_list() == [1, -1, None, 0]


def test_expr_str_find_matches_series_find_in_every_form():
    texts = ["abcabc", "xyz", None, "bc", "", "\u00e9bc", "b"]
    df = DataFrame({"s": Series(texts)})
    for needle in ["bc", "", "b", "\u00e9", "zz"]:
        series_find = df["s"].str.find(needle).to_list()
        want = [None if v is None or v < 0 else v for v in series_find]
        assert col("s").str.find(needle).apply(df).to_list() == want, needle
        lazy = df.lazy().with_column("p", col("s").str.find(needle)).collect()["p"].to_list()
        assert lazy == want, needle
        kept = df.lazy().filter(col("s").str.find(needle) >= 0).collect()["s"].to_list()
        assert kept == [t for t, v in zip(texts, want) if v is not None], needle


def test_log1p_and_expm1_are_exact_for_a_tiny_x():
    tiny = 1e-20
    s = Series([tiny, 0.0, -0.5, 1.0])
    got = s.log1p().to_list()
    assert got[0] == pytest.approx(tiny, rel=1e-12)
    assert got[1] == 0.0
    assert got[2] == pytest.approx(math.log1p(-0.5), rel=1e-15)
    assert s.expm1().to_list()[0] == pytest.approx(tiny, rel=1e-12)
    df = DataFrame({"x": s})
    assert col("x").log1p().apply(df).to_list()[0] == pytest.approx(tiny, rel=1e-12)
    assert col("x").expm1().apply(df).to_list()[0] == pytest.approx(tiny, rel=1e-12)


def test_empty_reduction_type_error_comes_first():
    empty = Series(["a"]).filter(Series([False]))
    full = Series(["a", "b"])
    with pytest.raises(TypeError):
        full.mean()
    with pytest.raises(TypeError):
        empty.mean()
    assert Series([1.0]).filter(Series([False])).mean() is None
    assert Series([1.0]).filter(Series([False])).min() is None
    assert Series([1.0]).filter(Series([False])).var() is None
    assert Series([1.0]).var() != Series([1.0]).var()  # NaN: one value, sample form


def test_clip_with_one_bound_on_integers():
    s = Series([-5, 0, 7, 2**62])
    assert col("x").clip(lower_bound=1).apply(DataFrame({"x": s})).to_list() == [1, 1, 7, 2**62]
    assert col("x").clip(upper_bound=3).apply(DataFrame({"x": s})).to_list() == [-5, 0, 3, 3]
    assert col("x").clip(1, 3).apply(DataFrame({"x": s})).to_list() == [1, 1, 3, 3]
    u = Series([0, 5, 2**63], dtype="uint64")
    assert col("x").clip(lower_bound=3).apply(DataFrame({"x": u})).to_list() == [3, 5, 2**63]


def test_lazy_group_quantile_takes_a_numpy_level():
    df = DataFrame({"k": [1, 1, 1, 2, 2], "x": [1.0, 2.0, 3.0, 4.0, 6.0]})
    for level in (np.float64(0.5), np.float32(0.5)):
        got = (
            df.lazy()
            .group_by("k")
            .agg(q=col("x").quantile(level, "linear"), p=col("x").percentile(level * 100))
            .collect()
            .sort("k")
        )
        assert got["q"].to_list() == [2.0, 5.0]
        assert got["p"].to_list() == [2.0, 5.0]
