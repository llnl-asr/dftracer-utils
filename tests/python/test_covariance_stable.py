"""corr, cov and the regressions keep their digits for shifted data (the repro of
the stable-covariance change), and degenerate input gives the pandas values."""

import math

import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame, col

N = 1_000_000


def two_pass(x, y):
    """Exactly-rounded sums: the reference must not lose the digits under test."""
    n = len(x)
    mx, my = math.fsum(x) / n, math.fsum(y) / n
    dx = [a - mx for a in x]
    dy = [b - my for b in y]
    cxx = math.fsum(a * a for a in dx) - math.fsum(dx) ** 2 / n
    cyy = math.fsum(b * b for b in dy) - math.fsum(dy) ** 2 / n
    cxy = math.fsum(a * b for a, b in zip(dx, dy)) - math.fsum(dx) * math.fsum(dy) / n
    return {
        "corr": cxy / math.sqrt(cxx * cyy),
        "cov": cxy / (n - 1),
        "slope": cxy / cxx,
    }


def rel(a, b):
    return abs(a - b) / abs(b)


@pytest.fixture(scope="module")
def shifted():
    """The proposal's repro: x = 1e9 + z, y = 5e8 + z / 2 + noise."""
    rng = np.random.default_rng(0)
    z = rng.normal(0, 1, N)
    x = 1e9 + z
    y = 5e8 + 0.5 * z + rng.normal(0, 1, N)
    return x, y, two_pass(x.tolist(), y.tolist())


def frame(x, y):
    n = len(x)
    return DataFrame.from_dict({"g": ["a"] * n, "x": list(map(float, x)), "y": list(map(float, y))})


def test_series_corr_cov_keep_their_digits(shifted):
    x, y, ref = shifted
    f = frame(x, y)
    assert 0.44 < ref["corr"] < 0.46  # the repro: pandas and numpy give 0.4476719
    assert rel(f["x"].corr(f["y"]), ref["corr"]) < 1e-9
    assert rel(f["x"].cov(f["y"]), ref["cov"]) < 1e-9
    assert rel(pd.Series(x).corr(pd.Series(y)), ref["corr"]) < 1e-9


def test_group_by_aggregates_keep_their_digits(shifted):
    x, y, ref = shifted
    r = (
        frame(x, y)
        .group_by("g")
        .agg(
            c=col("y").corr(col("x")),
            v=col("y").covar_samp(col("x")),
            s=col("y").regr_slope(col("x")),
        )
        .to_pandas()
    )
    assert rel(r["c"][0], ref["corr"]) < 1e-9
    assert rel(r["v"][0], ref["cov"]) < 1e-9
    assert rel(r["s"][0], ref["slope"]) < 1e-9


def test_a_tiny_spread():
    rng = np.random.default_rng(1)
    z = rng.normal(0, 1, 200_000)
    x = 1e3 + 1e-3 * z
    y = 1e3 + 5e-4 * z + 1e-3 * rng.normal(0, 1, 200_000)
    ref = two_pass(x.tolist(), y.tolist())
    f = frame(x, y)
    assert rel(f["x"].corr(f["y"]), ref["corr"]) < 1e-9
    assert rel(f["x"].cov(f["y"]), ref["cov"]) < 1e-9


def test_rows_with_a_null_are_skipped():
    rng = np.random.default_rng(2)
    z = rng.normal(0, 1, 5000)
    x = (1e9 + z).tolist()
    y = (5e8 + 0.5 * z + rng.normal(0, 1, 5000)).tolist()
    for i in range(0, 5000, 7):
        x[i] = None
    for i in range(3, 5000, 11):
        y[i] = None
    f = DataFrame.from_dict({"x": x, "y": y})
    keep = [(a, b) for a, b in zip(x, y) if a is not None and b is not None]
    ref = two_pass([a for a, _ in keep], [b for _, b in keep])
    assert rel(f["x"].corr(f["y"]), ref["corr"]) < 1e-9
    assert rel(f["x"].cov(f["y"]), ref["cov"]) < 1e-9
    assert (
        rel(pd.Series(x, dtype="float64").corr(pd.Series(y, dtype="float64")), ref["corr"]) < 1e-9
    )


def test_each_group_keeps_its_own_accuracy():
    rng = np.random.default_rng(3)
    z0, z1 = rng.normal(0, 1, 20000), rng.normal(0, 1, 20000)
    x0, y0 = 1e9 + z0, 5e8 + 0.5 * z0 + rng.normal(0, 1, 20000)
    x1, y1 = 2e3 + 1e-2 * z1, -4e4 + 5e-3 * z1 + 1e-2 * rng.normal(0, 1, 20000)
    keys = ["a", "b"] * 20000
    xs = [v for pair in zip(x0, x1) for v in pair]
    ys = [v for pair in zip(y0, y1) for v in pair]
    f = DataFrame.from_dict({"g": keys, "x": xs, "y": ys})
    r = f.group_by("g").agg(c=col("y").corr(col("x"))).to_pandas().set_index("g")["c"]
    assert rel(r["a"], two_pass(x0.tolist(), y0.tolist())["corr"]) < 1e-9
    assert rel(r["b"], two_pass(x1.tolist(), y1.tolist())["corr"]) < 1e-9


def test_dataframe_corr_and_cov_keep_their_digits():
    rng = np.random.default_rng(4)
    z = rng.normal(0, 1, 3000)
    a, b = (1e9 + z).tolist(), (5e8 + 0.5 * z + rng.normal(0, 1, 3000)).tolist()
    f = DataFrame.from_dict({"a": a, "b": b})
    ab, aa, bb = two_pass(a, b), two_pass(a, a), two_pass(b, b)
    corr = f.corr().to_pandas().set_index("column")
    cov = f.cov().to_pandas().set_index("column")
    assert rel(corr.loc["a", "b"], ab["corr"]) < 1e-9 and rel(corr.loc["b", "a"], ab["corr"]) < 1e-9
    assert corr.loc["a", "a"] == pytest.approx(1.0) and corr.loc["b", "b"] == pytest.approx(1.0)
    assert rel(cov.loc["a", "b"], ab["cov"]) < 1e-9
    assert rel(cov.loc["a", "a"], aa["cov"]) < 1e-9 and rel(cov.loc["b", "b"], bb["cov"]) < 1e-9


def test_a_constant_column_gives_nan_as_in_pandas():
    f = DataFrame.from_dict({"k": [5.0] * 100, "v": [float(i) for i in range(100)]})
    p = pd.DataFrame({"k": [5.0] * 100, "v": [float(i) for i in range(100)]})
    assert math.isnan(f["k"].corr(f["v"]))
    with np.errstate(invalid="ignore"):
        assert math.isnan(p["k"].corr(p["v"]))
    got = f.corr().to_pandas().set_index("column")
    assert math.isnan(got.loc["k", "v"]) and math.isnan(got.loc["k", "k"])
    assert got.loc["v", "v"] == pytest.approx(1.0)
    # The covariance of a constant column is a real 0.
    assert f["k"].cov(f["v"]) == 0.0


def test_a_single_pair_gives_nan():
    f = DataFrame.from_dict({"x": [1.0], "y": [2.0]})
    assert math.isnan(f["x"].corr(f["y"]))
    assert math.isnan(f["x"].cov(f["y"]))


def test_the_group_by_aggregate_keeps_its_zero_for_no_spread():
    f = DataFrame.from_dict({"g": ["a"] * 50, "x": [5e8] * 50, "y": [float(i) for i in range(50)]})
    r = f.group_by("g").agg(c=col("y").corr(col("x")), s=col("y").regr_slope(col("x"))).to_pandas()
    assert r["c"][0] == 0.0
    assert r["s"][0] == 0.0
