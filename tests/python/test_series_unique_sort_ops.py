# SPDX-License-Identifier: MIT
"""Series.unique, drop_duplicates, arg_sort, search_sorted, the arithmetic
names and DataFrame.kurtosis against real pandas and Polars."""

import math

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as dft  # noqa: E402

pd = pytest.importorskip("pandas")

WITH_NULLS = [3, 1, 3, None, 2, None, 1]
NO_NULLS = [3, 1, 3, 2, 1]


def _nan_to_none(values):
    return [None if v is None or (isinstance(v, float) and math.isnan(v)) else v for v in values]


@pytest.mark.parametrize("values", [WITH_NULLS, NO_NULLS], ids=["nulls", "no-nulls"])
def test_drop_duplicates_matches_pandas(values):
    for keep in ("first", "last", False):
        ours = dft.Series(values).drop_duplicates(keep).to_list()
        theirs = pd.Series(values, dtype="Int64").drop_duplicates(keep=keep).tolist()
        assert ours == [None if v is pd.NA else v for v in theirs], keep


@pytest.mark.parametrize("values", [WITH_NULLS, NO_NULLS], ids=["nulls", "no-nulls"])
def test_unique_matches_polars(values):
    pl = pytest.importorskip("polars")
    q = pl.Series(values, dtype=pl.Int64)
    ordered = dft.Series(values).unique(maintain_order=True).to_list()
    assert ordered == q.unique(maintain_order=True).to_list()
    loose = dft.Series(values).unique().to_list()
    # The polars order is unspecified; ours is sorted, nulls first. Same values.
    key = lambda v: (v is not None, v if v is not None else 0)  # noqa: E731
    assert sorted(loose, key=key) == sorted(q.unique().to_list(), key=key)
    assert loose == sorted(loose, key=key)
    assert loose.count(None) == q.unique().to_list().count(None)


def test_unique_keeps_one_null():
    assert dft.Series(WITH_NULLS).unique().to_list() == [None, 1, 2, 3]
    assert dft.Series(WITH_NULLS).unique(maintain_order=True).to_list() == [3, 1, None, 2]
    assert dft.Series(["b", None, "a", "b"]).unique(maintain_order=True).to_list() == [
        "b",
        None,
        "a",
    ]


@pytest.mark.parametrize("descending", [False, True])
@pytest.mark.parametrize("nulls_last", [False, True])
def test_arg_sort_matches_polars_and_sort(descending, nulls_last):
    pl = pytest.importorskip("polars")
    values = [3, None, 1, 2, None, 5]
    ours = dft.Series(values).arg_sort(descending=descending, nulls_last=nulls_last)
    theirs = pl.Series(values).arg_sort(descending=descending, nulls_last=nulls_last)
    assert ours.to_list() == theirs.to_list()
    s = dft.Series(values)
    assert (
        s.gather(ours).to_list() == s.sort(descending=descending, nulls_last=nulls_last).to_list()
    )


@pytest.mark.parametrize("descending", [False, True])
@pytest.mark.parametrize("nulls_last", [False, True])
def test_expr_arg_sort_gathers_what_sort_gives(descending, nulls_last):
    pl = pytest.importorskip("polars")
    values = [3, None, 1, 2, None, 5]
    frame = dft.DataFrame({"x": values})
    idx = frame.select(
        dft.col("x").arg_sort(descending=descending, nulls_last=nulls_last).alias("i")
    ).to_dict()["i"]
    got = [values[i] for i in idx]
    assert got == pl.Series(values).sort(descending=descending, nulls_last=nulls_last).to_list()
    # With one null the indices are polars' exactly.
    single = [3, None, 1, 2, 5]
    one = dft.DataFrame({"x": single}).select(
        dft.col("x").arg_sort(descending=descending, nulls_last=nulls_last).alias("i")
    )
    want = pl.Series(single).arg_sort(descending=descending, nulls_last=nulls_last).to_list()
    assert one.to_dict()["i"] == want


SORTED = [1, 2, 4, 4, 7]


@pytest.mark.parametrize("side", ["left", "right"])
@pytest.mark.parametrize("needle", [0, 2, 4, 5, 9])
def test_searchsorted_scalar_matches_pandas(side, needle):
    ours = dft.Series(SORTED).searchsorted(needle, side=side)
    assert isinstance(ours, int)
    assert ours == int(pd.Series(SORTED).searchsorted(needle, side=side))


@pytest.mark.parametrize("side", ["left", "right"])
def test_searchsorted_values_match_pandas(side):
    needles = [0, 2, 4, 5, 9]
    ours = dft.Series(SORTED).search_sorted(needles, side=side).to_list()
    assert ours == [int(v) for v in pd.Series(SORTED).searchsorted(needles, side=side)]
    as_series = dft.Series(SORTED).search_sorted(dft.Series(needles), side=side).to_list()
    assert as_series == ours


@pytest.mark.parametrize("side", ["any", "left", "right"])
def test_search_sorted_matches_polars(side):
    pl = pytest.importorskip("polars")
    for needle in (0, 2, 4, 9):
        ours = dft.Series(SORTED).search_sorted(needle, side=side)
        theirs = pl.Series(SORTED).search_sorted(needle, side=side)
        if side == "any":
            # "any" may return either insertion point; ours is the left one.
            assert ours == pl.Series(SORTED).search_sorted(needle, side="left")
        else:
            assert ours == theirs


def test_search_sorted_refuses_a_bad_side():
    with pytest.raises(ValueError, match="side"):
        dft.Series(SORTED).search_sorted(2, side="middle")


def test_arithmetic_names_take_a_scalar_and_the_pandas_names_are_the_same():
    pl = pytest.importorskip("polars")
    values = [6, 4, None, 2]
    ours = dft.Series(values)
    theirs = pl.Series(values)
    assert ours.mul(3).to_list() == (theirs * 3).to_list()
    assert ours.sub(1).to_list() == (theirs - 1).to_list()
    assert ours.add(1).to_list() == (theirs + 1).to_list()
    assert ours.truediv(2).to_list() == (theirs / 2).to_list()
    other = dft.Series([1, 2, 3, 4])
    assert ours.mul(other).to_list() == (theirs * pl.Series([1, 2, 3, 4])).to_list()
    for alias, canonical in (
        ("multiply", "mul"),
        ("subtract", "sub"),
        ("divide", "truediv"),
        ("div", "truediv"),
    ):
        assert getattr(dft.Series, alias) is getattr(dft.Series, canonical)


def test_series_positional_accessors_without_labels_agree():
    s = dft.Series([6, 4, 9])
    assert s.iloc[1] == s.loc[1] == s.iat[1] == s.at[1] == 4
    assert dft.Series.iat is dft.Series.iloc
    assert dft.Series.at is dft.Series.loc


def test_dataframe_kurtosis_follows_the_polars_name():
    pl = pytest.importorskip("polars")
    data = {"x": [1.0, 2.0, 4.0, 8.0, 9.0], "y": [3.0, 1.0, 4.0, 1.0, 5.0]}
    frame = dft.DataFrame(data)
    got = frame.kurtosis().to_dict()
    for name, values in data.items():
        assert got[name][0] == pytest.approx(pl.Series(values).kurtosis())
        assert got[name][0] == pytest.approx(frame[name].kurtosis())
    biased = frame.kurtosis(fisher=False, bias=False).to_dict()
    for name, values in data.items():
        assert biased[name][0] == pytest.approx(
            pl.Series(values).kurtosis(fisher=False, bias=False)
        )
    # kurt is the pandas estimator.
    pandas_frame = pd.DataFrame(data)
    kurt = frame.kurt().to_dict()
    for name in data:
        assert kurt[name][0] == pytest.approx(float(pandas_frame[name].kurt()))
