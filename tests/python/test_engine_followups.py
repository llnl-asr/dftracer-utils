# SPDX-License-Identifier: MIT
"""The engine, not Python, owns stable sorts with nulls first, unique with a
null, search_sorted sides, duplicated, and the pandas skew and kurt."""

import math

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import col  # noqa: E402

pd = pytest.importorskip("pandas")

TIES = [5, None, 2, 5, None, 5, 0, 2, 0, None]


@pytest.mark.parametrize("descending", [False, True])
@pytest.mark.parametrize("nulls_last", [False, True])
def test_expr_arg_sort_matches_polars_ties_and_nulls(descending, nulls_last):
    pl = pytest.importorskip("polars")
    want = (
        pl.DataFrame({"x": TIES})
        .select(pl.col("x").arg_sort(descending=descending, nulls_last=nulls_last))
        .to_series()
        .to_list()
    )
    got = (
        dft.DataFrame({"x": TIES})
        .select(col("x").arg_sort(descending=descending, nulls_last=nulls_last).alias("x"))
        .to_dict()["x"]
    )
    assert got == want


@pytest.mark.parametrize("nulls_last", [False, True])
def test_lazy_spilled_sort_is_stable_with_nulls(nulls_last):
    pl = pytest.importorskip("polars")
    n = 4000
    data = {
        "k": [None if i % 6 == 0 else (i * 7919) % 17 for i in range(n)],
        "s": [None if i % 7 == 0 else f"v{(i * 31) % 13}" for i in range(n)],
        "row": list(range(n)),
    }
    for by, desc in (("k", False), ("k", True), (["s", "k"], [True, False])):
        want = (
            pl.DataFrame(data)
            .sort(by, descending=desc, nulls_last=nulls_last, maintain_order=True)
            .to_dict(as_series=False)["row"]
        )
        plan = dft.DataFrame(data).lazy().memory_budget(1 << 12)
        got = plan.sort(by, descending=desc, nulls_last=nulls_last).collect().to_dict()["row"]
        assert got == want


@pytest.mark.parametrize("maintain_order", [False, True])
def test_unique_keeps_one_null_like_polars(maintain_order):
    pl = pytest.importorskip("polars")
    for values in ([3, None, 1, 3, None, 2], ["b", None, "a", "b", None], [1.5, None, 1.5, 0.5]):
        want = pl.Series(values).unique(maintain_order=maintain_order).to_list()
        got = dft.Series(values).unique(maintain_order=maintain_order).to_list()
        if maintain_order:
            assert got == want
        else:
            assert sorted(got, key=lambda v: (v is not None, v)) == sorted(
                want, key=lambda v: (v is not None, v)
            )
            assert got[0] is None


@pytest.mark.parametrize("keep", ["first", "last", False])
def test_duplicated_matches_pandas(keep):
    values = [1, 2, 1, None, 2, None, 3, 1]
    want = pd.Series(values).duplicated(keep=keep).tolist()
    assert dft.Series(values).duplicated(keep=keep).to_list() == want


@pytest.mark.parametrize("side", ["left", "right"])
def test_search_sorted_sides_match_pandas(side):
    base = [1, 2, 2, 2, 5, 7]
    probes = [0, 2, 3, 7, 9]
    want = pd.Series(base).searchsorted(probes, side=side).tolist()
    ours = dft.Series(base)
    assert ours.search_sorted(probes, side=side).to_list() == want
    assert [ours.search_sorted(p, side=side) for p in probes] == want


GROUPS = {
    "k": ["a"] * 5 + ["b"] * 3 + ["c"] * 6,
    "x": [1.0, 2.0, 4.0, 8.0, 9.0, 3.0, 3.0, 3.0, 1.0, None, 2.0, 5.0, 9.0, 4.0],
}


def _same(a, b):
    if a is None or b is None:
        return (a is None or math.isnan(a)) and (b is None or math.isnan(b))
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    return a == pytest.approx(b, rel=1e-9)


@pytest.mark.parametrize("method", ["skew", "kurt"])
@pytest.mark.parametrize("budget", [None, 1])
def test_lazy_group_and_reduce_match_pandas(method, budget):
    old_pandas = tuple(int(v) for v in pd.__version__.split(".")[:2]) < (3, 1)
    plan = dft.DataFrame(GROUPS).lazy()
    if budget:
        plan = plan.memory_budget(budget)
    want = getattr(pd.DataFrame(GROUPS).groupby("k")["x"], method)()
    got = getattr(plan.group_by("k"), method)().collect().to_dict()
    for key, value in zip(got["k"], got["x"]):
        if old_pandas and len(set(v for k, v in zip(GROUPS["k"], GROUPS["x"]) if k == key)) == 1:
            assert math.isnan(value), (method, key)
            continue
        assert _same(value, float(want.loc[key])), (method, key)
    whole = getattr(plan, method)().collect().to_dict()["x"][0]
    assert _same(whole, float(getattr(pd.Series(GROUPS["x"]), method)()))


@pytest.mark.parametrize("name", ["skew", "kurt"])
def test_agg_names_are_the_pandas_estimators(name):
    frame = dft.DataFrame(GROUPS)
    want = float(getattr(pd.Series(GROUPS["x"][:5]), name)())
    got = frame.filter(col("k") == "a").agg({"x": name}).to_dict()
    assert _same(got["x"][0], want)
    named = frame.filter(col("k") == "a").agg(out=("x", name)).to_dict()
    assert _same(named["out"][0], want)


def test_expr_bias_keyword_chooses_the_estimator():
    pl = pytest.importorskip("polars")
    values = [1.0, 2.0, 4.0, 8.0, 9.0]
    frame = dft.DataFrame({"x": values})
    for method in ("skew", "kurt"):
        sample = frame.select(getattr(col("x"), method)(bias=False)).to_dict()["x"][0]
        assert sample == pytest.approx(float(getattr(pd.Series(values), method)()))
    assert frame.select(col("x").skew()).to_dict()["x"][0] == pytest.approx(
        pl.Series(values).skew()
    )


def test_eager_to_dummies_and_pivot_ignore_a_null_key():
    frame = dft.DataFrame(
        {"k": [7, None, 7, 8, 8], "r": [1, 1, None, 2, 2], "v": [10, 20, 30, 40, 50]}
    )
    assert frame.to_dummies("k").columns == ["k_7", "k_8", "r", "v"]
    wide = frame.pivot(index="r", on="k", values="v", agg="sum")
    assert wide.columns == ["r", "7", "8"]
    assert wide.to_dict()["r"] == [1, 2]
