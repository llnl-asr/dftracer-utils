"""``Series.replace`` and ``DataFrame.replace``: scalar, list and dict forms,
null and NaN rules, dtype rules (openspec change series-replace-list)."""

import math

import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame, Series

INF, NINF, NAN = float("inf"), float("-inf"), float("nan")


def _floats():
    return Series.from_list([1.0, INF, NINF, 4.0])


def _same(got, want):
    """List equality where NaN equals NaN and None equals None."""
    assert len(got) == len(want), (got, want)
    for g, w in zip(got, want):
        if isinstance(w, float) and math.isnan(w):
            assert isinstance(g, float) and math.isnan(g), (got, want)
        else:
            assert g == w, (got, want)


# --- the analyzer repro: replace(0, null) on every column type, frame and Series -----------


def _zero_frame():
    return DataFrame.from_dict(
        {
            "i": [0, 1, 2],  # integers
            "f": [0.0, 1.5, 0.0],  # floats
            "n": [0.0, None, 2.0],  # floats with a null
            "m": [0, None, 3],  # integers with a null
        }
    )


_NULLED = {"i": [None, 1, 2], "f": [None, 1.5, None], "n": [None, None, 2.0], "m": [None, None, 3]}
_NINETY_NINE = {"i": [99, 1, 2], "f": [99.0, 1.5, 99.0], "n": [99.0, None, 2.0], "m": [99, None, 3]}


def _cols(frame):
    return {name: frame[name].to_list() for name in frame.columns}


_NULL_FORMS = {
    "scalar to NA": lambda x: x.replace(0, pd.NA),
    "scalar to None": lambda x: x.replace(0, None),
    "list to NA": lambda x: x.replace([0.0], pd.NA),
    "two-element list to None": lambda x: x.replace([0, 0.0], None),
    "dict to None": lambda x: x.replace({0: None}),
}


@pytest.mark.parametrize("form", list(_NULL_FORMS.values()), ids=list(_NULL_FORMS))
def test_repro_a_null_replacement_changes_every_column_it_matches(form):
    f = _zero_frame()
    assert _cols(form(f)) == _NULLED
    # the same call on each column as a Series
    for name in f.columns:
        got = form(f[name])
        assert got.to_list() == _NULLED[name], name
        assert got.dtype == f[name].dtype  # a null keeps the column type


def test_repro_a_number_replacement_still_works_in_both_forms():
    f = _zero_frame()
    assert _cols(f.replace(0, 99)) == _NINETY_NINE
    assert _cols(f.replace([0.0], 99)) == _NINETY_NINE
    for name in f.columns:
        assert f[name].replace(0, 99).to_list() == _NINETY_NINE[name]


def test_repro_a_form_that_matches_nothing_returns_the_values_unchanged():
    f = DataFrame.from_dict({"i": [1, 2], "f": [1.5, 2.5]})
    assert _cols(f.replace(0, pd.NA)) == {"i": [1, 2], "f": [1.5, 2.5]}


def test_repro_every_unsupported_form_raises_a_named_error_never_a_no_op():
    f = _zero_frame()
    for call, match in (
        (lambda: f.replace([0, 1], [None]), "to_replace has 2 values but value has 1"),
        (lambda: f.replace({0: None}, 1), "omitted"),
        (lambda: f.replace(0), "value"),
        (lambda: f.replace(0, "zero"), "cannot put a string into a"),
        (lambda: f.replace(True, 1), "bool values are not supported"),
        (lambda: f["i"].replace(0, "zero"), "cannot put a string into a int64"),
    ):
        with pytest.raises((TypeError, ValueError), match=match):
            call()


# --- Series: the forms --------------------------------------------------------------


def test_proposal_repro_list_to_null_scalar_and_dict_forms():
    s = _floats()
    _same(s.replace([INF, NINF], pd.NA).to_list(), [1.0, None, None, 4.0])
    _same(s.replace([INF, NINF], None).to_list(), [1.0, None, None, 4.0])
    _same(s.replace([INF, NINF], NAN).to_list(), [1.0, NAN, NAN, 4.0])
    _same(s.replace({INF: 1.0, NINF: -1.0}).to_list(), [1.0, 1.0, -1.0, 4.0])
    _same(s.replace([1.0, 4.0], [10.0, 40.0]).to_list(), [10.0, INF, NINF, 40.0])
    _same(s.replace(INF, pd.NA).to_list(), [1.0, None, NINF, 4.0])


def test_list_to_one_replacement():
    _same(_floats().replace([INF, NINF], 0.0).to_list(), [1.0, 0.0, 0.0, 4.0])


def test_matches_are_found_on_the_original_values_so_a_swap_works():
    s = Series.from_list([1, 2, 3])
    assert s.replace({1: 2, 2: 1}).to_list() == [2, 1, 3]
    assert s.replace([1, 2], [2, 1]).to_list() == [2, 1, 3]


def test_lists_of_different_lengths_name_both():
    with pytest.raises(ValueError, match="to_replace has 3 values but value has 2"):
        Series.from_list([1, 2, 3]).replace([1, 2, 3], [10, 20])


def test_dict_with_a_value_and_a_missing_value_are_errors():
    with pytest.raises(TypeError, match="omitted"):
        Series.from_list([1]).replace({1: 2}, 3)
    with pytest.raises(TypeError, match="value"):
        Series.from_list([1]).replace(1)
    with pytest.raises(TypeError, match="scalar"):
        Series.from_list([1]).replace(1, [2])


# --- Series: null and NaN ------------------------------------------------------------


def test_null_is_not_nan_and_nan_is_not_null():
    s = Series.from_list([1.0, None, NAN])
    _same(s.replace(None, 0.0).to_list(), [1.0, 0.0, NAN])
    _same(s.replace(pd.NA, 0.0).to_list(), [1.0, 0.0, NAN])
    _same(s.replace(NAN, 0.0).to_list(), [1.0, None, 0.0])
    _same(s.replace([None, NAN], 0.0).to_list(), [1.0, 0.0, 0.0])
    # the replacement NaN is a value, not a null
    out = Series.from_list([1.0, INF]).replace(INF, NAN)
    assert out.null_count == 0 and math.isnan(out.to_list()[1])


def test_a_null_replacement_keeps_the_column_type():
    ints = Series.from_list([1, 2, 3])
    out = ints.replace(2, None)
    assert out.to_list() == [1, None, 3] and out.dtype == ints.dtype
    strs = Series.from_list(["a", "b", "c"])
    assert strs.replace("a", None).to_list() == [None, "b", "c"]


# --- Series: dtype rules ---------------------------------------------------------------


def test_float_into_integer_widens_and_string_into_numeric_raises():
    ints = Series.from_list([1, 2, 3])
    out = ints.replace(2, 2.5)
    assert out.to_list() == [1.0, 2.5, 3.0] and out.dtype != ints.dtype
    with pytest.raises(TypeError, match="cannot put a string into a int64 column"):
        ints.replace(2, "two")
    with pytest.raises(TypeError, match="cannot put a number into a string column"):
        Series.from_list(["a"]).replace("a", 1)


def test_an_old_value_the_type_cannot_hold_matches_nothing():
    assert Series.from_list([1, 2, 3]).replace("a", 0).to_list() == [1, 2, 3]
    assert Series.from_list(["a", "b"]).replace(1, 2).to_list() == ["a", "b"]
    # NaN cannot be in an integer column
    assert Series.from_list([1, 2]).replace(NAN, 0).to_list() == [1, 2]


def test_strings():
    s = Series.from_list(["a", "b", "c"])
    assert s.replace(["a", "b"], "z").to_list() == ["z", "z", "c"]
    assert s.replace({"a": "x", "c": "y"}).to_list() == ["x", "b", "y"]
    assert s.replace("a", None).to_list() == [None, "b", "c"]
    assert Series.from_list(["a", None]).replace(None, "n").to_list() == ["a", "n"]


def test_unsupported_columns_raise_and_name_the_type():
    with pytest.raises(TypeError, match="a bool column is not supported"):
        Series.from_list([True, False]).replace(1, 0)
    with pytest.raises(TypeError, match="a list column is not supported"):
        Series.from_list([[1], [2]]).replace(1, 0)
    with pytest.raises(TypeError, match="bool values are not supported"):
        Series.from_list([1, 2]).replace(True, 0)


def test_numpy_scalars_are_numbers():
    s = Series.from_list([1, 2, 3])
    assert s.replace(np.int64(2), np.int64(20)).to_list() == [1, 20, 3]
    assert Series.from_list([1.0, 2.0]).replace(np.float64(2.0), np.float32(0.5)).to_list() == [
        1.0,
        0.5,
    ]


# --- DataFrame --------------------------------------------------------------------------


def test_frame_null_replacement_is_no_longer_a_silent_no_op():
    f = DataFrame.from_dict({"a": [1.0, INF], "b": [NINF, 2.0]})
    got = f.replace([INF, NINF], pd.NA)
    _same(got["a"].to_list(), [1.0, None])
    _same(got["b"].to_list(), [None, 2.0])
    got = f.replace(INF, pd.NA)
    _same(got["a"].to_list(), [1.0, None])
    _same(got["b"].to_list(), [NINF, 2.0])


def test_frame_mixed_columns_follow_the_series_rules_per_column():
    f = DataFrame.from_dict({"n": [0, 5], "s": ["x", "y"], "b": [True, False]})
    got = f.replace(0, None).to_pandas().to_dict("list")
    assert got["n"][0] is None or math.isnan(got["n"][0])
    assert got["n"][1] == 5 and got["s"] == ["x", "y"] and got["b"] == [True, False]
    got = f.replace({"x": "X"}).to_pandas().to_dict("list")
    assert got["s"] == ["X", "y"] and got["n"] == [0, 5]


def test_frame_fitting_old_value_with_a_replacement_the_column_cannot_hold_names_the_column():
    f = DataFrame.from_dict({"n": [0, 5], "s": ["x", "y"]})
    with pytest.raises(TypeError, match=r"cannot put a string into a int64 column \(column 'n'\)"):
        f.replace(0, "zero")


def test_frame_pairs_dict_and_lists():
    f = DataFrame.from_dict({"a": [1, 2], "b": [2, 1]})
    got = f.replace({1: 2, 2: 1}).to_pandas().to_dict("list")
    assert got == {"a": [2, 1], "b": [1, 2]}
    got = f.replace([1, 2], [10, 20]).to_pandas().to_dict("list")
    assert got == {"a": [10, 20], "b": [20, 10]}


# --- pandas parity on random data ---------------------------------------------------------


def test_pandas_parity_on_random_floats_with_nan_and_infinities():
    rng = np.random.default_rng(5)
    x = rng.normal(size=300)
    x[rng.integers(0, 300, 20)] = np.inf
    x[rng.integers(0, 300, 20)] = -np.inf
    x[rng.integers(0, 300, 20)] = np.nan
    p = pd.Series(x)
    s = Series.from_list(x.tolist())
    for to_replace, value in (
        ([np.inf, -np.inf], np.nan),
        ([np.inf, -np.inf], 0.0),
        ({np.inf: 1.0, -np.inf: -1.0}, None),
        ([np.inf, -np.inf], [100.0, -100.0]),
        (np.nan, 0.0),
    ):
        want = p.replace(to_replace, value) if value is not None else p.replace(to_replace)
        got = s.replace(to_replace, value) if value is not None else s.replace(to_replace)
        _same(got.to_list(), want.tolist())


def test_pandas_parity_on_integers_and_strings():
    p = pd.Series([1, 2, 3, 2, 1])
    s = Series.from_list([1, 2, 3, 2, 1])
    for args in (({1: 10, 3: 30},), ([1, 2], 0), ([1, 2], [7, 8]), (2, 20)):
        want = (
            p.replace(*args)
            if len(args) == 2 or not isinstance(args[0], dict)
            else p.replace(args[0])
        )
        got = s.replace(*args)
        assert got.to_list() == want.tolist()
    q = pd.Series(["a", "b", "c", "a"])
    t = Series.from_list(["a", "b", "c", "a"])
    assert t.replace(["a", "b"], ["x", "y"]).to_list() == q.replace(["a", "b"], ["x", "y"]).tolist()
    assert t.replace({"c": "z"}).to_list() == q.replace({"c": "z"}).tolist()
