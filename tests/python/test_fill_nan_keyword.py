# SPDX-License-Identifier: MIT
"""``fill_nan`` takes ``value`` as a keyword on a Series, a DataFrame and a
LazyFrame."""

import pytest

from dftracer.utils import DataFrame, Series

NAN = float("nan")
WANT = [1.0, 0.0, 3.0]


def test_series_fill_nan_value_keyword():
    assert Series.from_list([1.0, NAN, 3.0]).fill_nan(value=0.0).to_list() == WANT


def test_dataframe_fill_nan_value_keyword():
    assert DataFrame({"f": [1.0, NAN, 3.0]}).fill_nan(value=0.0).to_dict()["f"] == WANT


def test_lazyframe_fill_nan_value_keyword():
    out = DataFrame({"f": [1.0, NAN, 3.0]}).lazy().fill_nan(value=0.0).collect()
    assert out.to_dict()["f"] == WANT


def test_dataframe_fill_nan_refuses_a_text_fill():
    df = DataFrame({"f": [1.0, NAN], "s": ["a", "b"]})
    with pytest.raises(ValueError):
        df.fill_nan("x")


def test_lazyframe_fill_nan_refuses_a_text_fill():
    df = DataFrame({"f": [1.0, NAN], "s": ["a", "b"]})
    with pytest.raises(ValueError):
        df.lazy().fill_nan("x").collect()


def test_fill_null_still_carries_a_text_fill_over_a_float_column():
    df = DataFrame({"f": [1.0, None], "s": ["a", None]})
    out = df.fill_null("x")
    assert out["f"].to_list() == [1.0, None]
    assert out["s"].to_list() == ["a", "x"]
