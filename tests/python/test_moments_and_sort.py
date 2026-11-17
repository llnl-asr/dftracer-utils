# SPDX-License-Identifier: MIT
"""skew and kurt give the pandas values under the pandas names and the polars
values under the polars keywords; sort puts nulls first as polars does and
sort_values keeps the pandas na_position default."""

import math

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import col  # noqa: E402

pd = pytest.importorskip("pandas")

CASES = {
    "n1": [1.0],
    "n2": [1.0, 2.0],
    "n3": [1.0, 2.0, 4.0],
    "n4": [1.0, 2.0, 4.0, 8.0],
    "nulls": [1.0, None, 2.0, 4.0, None, 9.0, 3.0],
    "constant": [2.0, 2.0, 2.0, 2.0, 2.0],
    "constant3": [2.0, 2.0, 2.0],
    "many": [float((i * 7919) % 101) for i in range(60)],
}


def _same(a, b):
    if a is None or b is None:
        return a is None and b is None
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    return a == pytest.approx(b, rel=1e-9, abs=1e-12)


@pytest.mark.parametrize("name", list(CASES))
def test_skew_and_kurt_are_the_pandas_values(name):
    if name.startswith("constant") and int(pd.__version__.split(".")[0]) < 3:
        pytest.skip("pandas 2 gives 0.0 for a constant column; pandas 3 gives NaN")
    values = CASES[name]
    ours = dft.Series(values, dtype="float64")
    theirs = pd.Series(values, dtype="float64")
    assert _same(ours.skew(), float(theirs.skew()))
    assert _same(ours.kurt(), float(theirs.kurt()))


@pytest.mark.parametrize("name", list(CASES))
def test_the_polars_keywords_give_the_polars_values(name):
    pl = pytest.importorskip("polars")
    values = CASES[name]
    ours = dft.Series(values, dtype="float64")
    theirs = pl.Series(values, dtype=pl.Float64)
    assert _same(ours.skew(bias=True), theirs.skew())
    assert _same(ours.kurtosis(), theirs.kurtosis())
    assert _same(ours.kurtosis(fisher=False), theirs.kurtosis(fisher=False))
    if len([v for v in values if v is not None]) >= 4 and name != "constant":
        assert _same(ours.kurtosis(bias=False), theirs.kurtosis(bias=False))
        assert _same(
            ours.kurtosis(fisher=False, bias=False), theirs.kurtosis(fisher=False, bias=False)
        )
    if len([v for v in values if v is not None]) >= 3 and name not in ("constant", "constant3"):
        assert _same(ours.skew(), theirs.skew(bias=False))


def test_the_polars_kurtosis_is_null_for_a_small_unbiased_sample():
    pl = pytest.importorskip("polars")
    for values in ([1.0], [1.0, 2.0], [1.0, 2.0, 4.0]):
        assert dft.Series(values).kurtosis(bias=False) is None
        assert pl.Series(values).kurtosis(bias=False) is None


def test_the_expr_follows_polars():
    pl = pytest.importorskip("polars")
    frame = dft.DataFrame({"x": CASES["n4"]})
    assert frame.select(col("x").skew()).to_dict()["x"][0] == pytest.approx(
        pl.Series(CASES["n4"]).skew()
    )


def _frame_values():
    return {
        "k": ["a", "a", "a", "a", "b", "b", "b", "c", "c", "c", "c", "c"],
        "x": [1.0, 2.0, 4.0, 8.0, 3.0, 3.0, 3.0, 1.0, None, 2.0, 5.0, 9.0],
        "y": [5, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8],
    }


def test_dataframe_skew_and_kurt_are_the_pandas_values():
    data = _frame_values()
    ours = dft.DataFrame(data).select("x", "y")
    theirs = pd.DataFrame(data)[["x", "y"]]
    for method in ("skew", "kurt"):
        got = getattr(ours, method)().to_dict()
        want = getattr(theirs, method)()
        for column in ("x", "y"):
            assert _same(got[column][0], float(want[column]))


def test_groupby_skew_and_kurt_are_the_pandas_values():
    if int(pd.__version__.split(".")[0]) < 3:
        pytest.skip("pandas 2 gives 0.0 for a constant group; pandas 3 gives NaN")
    data = _frame_values()
    ours = dft.DataFrame(data).group_by("k")
    theirs = pd.DataFrame(data).groupby("k")
    for method in ("skew", "kurt"):
        if not hasattr(theirs, method):
            continue
        got = getattr(ours, method)().to_dict()
        want = getattr(theirs, method)()
        for key, x, y in zip(got["k"], got["x"], got["y"]):
            assert _same(x, float(want.loc[key, "x"])), (method, key)
            assert _same(y, float(want.loc[key, "y"])), (method, key)


SORT_VALUES = {
    "int": [3, None, 1, None, 2, 1],
    "float": [3.5, None, 1.5, None, 2.5, 1.5],
    "str": ["b", None, "a", None, "c", "a"],
}


@pytest.mark.parametrize("name", list(SORT_VALUES))
@pytest.mark.parametrize("descending", [False, True])
@pytest.mark.parametrize("nulls_last", [False, True])
def test_series_sort_matches_polars(name, descending, nulls_last):
    pl = pytest.importorskip("polars")
    values = SORT_VALUES[name]
    ours = dft.Series(values).sort(descending=descending, nulls_last=nulls_last).to_list()
    theirs = pl.Series(values).sort(descending=descending, nulls_last=nulls_last).to_list()
    assert ours == theirs


def test_sort_puts_nulls_first_by_default():
    assert dft.Series([2, None, 1]).sort().to_list() == [None, 1, 2]
    assert dft.Series([2, None, 1]).sort(nulls_last=True).to_list() == [1, 2, None]


@pytest.mark.parametrize("descending", [False, True])
@pytest.mark.parametrize("nulls_last", [False, True])
def test_a_nan_sorts_after_every_number_and_apart_from_the_nulls(descending, nulls_last):
    pl = pytest.importorskip("polars")
    values = [3.0, None, math.nan, 1.0, None, 2.0, math.nan]
    ours = dft.Series(values).sort(descending=descending, nulls_last=nulls_last).to_list()
    theirs = pl.Series(values).sort(descending=descending, nulls_last=nulls_last).to_list()
    assert [None if v is None else ("nan" if v != v else v) for v in ours] == [
        None if v is None else ("nan" if v != v else v) for v in theirs
    ]


@pytest.mark.parametrize("name", list(SORT_VALUES))
@pytest.mark.parametrize("ascending", [True, False])
@pytest.mark.parametrize("na_position", ["first", "last"])
def test_series_sort_values_matches_pandas(name, ascending, na_position):
    values = SORT_VALUES[name]
    dtype = {"int": "Int64", "float": "Float64", "str": "string"}[name]
    ours = dft.Series(values).sort_values(ascending=ascending, na_position=na_position)
    theirs = pd.Series(values, dtype=dtype).sort_values(
        ascending=ascending, na_position=na_position
    )
    assert ours.to_list() == [None if v is pd.NA else v for v in theirs.tolist()]


def test_sort_values_keeps_nulls_last_by_default():
    assert dft.Series([2, None, 1]).sort_values().to_list() == [1, 2, None]


def _frame():
    return {
        "a": [2, None, 1, 2, None, 1, 3],
        "b": ["x", "y", None, "a", "b", "c", None],
        "c": [0, 1, 2, 3, 4, 5, 6],
    }


@pytest.mark.parametrize("nulls_last", [False, True])
@pytest.mark.parametrize(
    "by,desc", [("a", False), ("a", True), (["a", "b"], [False, True]), (["b", "a"], [True, False])]
)
def test_frame_sort_matches_polars_eager_and_lazy(nulls_last, by, desc):
    pl = pytest.importorskip("polars")
    data = _frame()
    want = (
        pl.DataFrame(data)
        .sort(by, descending=desc, nulls_last=nulls_last, maintain_order=True)
        .to_dict(as_series=False)
    )
    ours = dft.DataFrame(data)
    assert ours.sort(by, descending=desc, nulls_last=nulls_last).to_dict() == want
    got = ours.lazy().sort(by, descending=desc, nulls_last=nulls_last).collect().to_dict()
    assert got == want


@pytest.mark.parametrize("na_position", ["first", "last"])
@pytest.mark.parametrize("ascending", [True, False, [True, False]])
def test_frame_sort_values_matches_pandas(na_position, ascending):
    data = _frame()
    by = ["a", "b"] if isinstance(ascending, list) else "a"
    theirs = (
        pd.DataFrame(data)
        .astype({"a": "Int64", "b": "string"})
        .sort_values(by, ascending=ascending, na_position=na_position, kind="stable")
    )
    want = {c: [None if v is pd.NA else v for v in theirs[c].tolist()] for c in theirs.columns}
    got = dft.DataFrame(data).sort_values(by, ascending=ascending, na_position=na_position)
    assert got.to_dict() == want
    got = dft.DataFrame(data).lazy().sort_values(by, ascending=ascending, na_position=na_position)
    assert got.collect().to_dict() == want


@pytest.mark.parametrize("descending", [False, True])
@pytest.mark.parametrize("nulls_last", [False, True])
def test_expr_sort_matches_polars(descending, nulls_last):
    pl = pytest.importorskip("polars")
    values = [3, None, 1, None, 2]
    want = (
        pl.DataFrame({"x": values})
        .select(pl.col("x").sort(descending=descending, nulls_last=nulls_last))["x"]
        .to_list()
    )
    got = (
        dft.DataFrame({"x": values})
        .select(col("x").sort(descending=descending, nulls_last=nulls_last).alias("x"))
        .to_dict()["x"]
    )
    assert got == want
