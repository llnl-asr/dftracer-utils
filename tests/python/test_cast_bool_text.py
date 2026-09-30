"""Python's astype spells a bool as text the way Python and pandas do."""

import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame


def _frame():
    return DataFrame.from_dict(
        {"b": [True, False, None, True], "i": [1, 2, 3, 4], "f": [1.5, 2.0, 0.0, 3.25]}
    )


def test_series_astype_string_spells_true_and_false_like_python():
    assert _frame()["b"].astype("string").to_list() == ["True", "False", None, "True"]


@pytest.mark.parametrize("target", ["string", "String", str, np.str_])
def test_every_spelling_of_the_string_target_agrees(target):
    assert _frame()["b"].astype(target).to_list() == ["True", "False", None, "True"]


def test_matches_pandas_astype_str_on_a_bool_without_nulls():
    values = [True, False, True, True]
    got = DataFrame.from_dict({"b": values})["b"].astype(str).to_list()
    assert got == pd.Series(values).astype(str).tolist()


def test_frame_wide_astype_string_spells_bool_columns_and_leaves_the_rest():
    out = _frame().astype("string")
    assert out["b"].to_list() == ["True", "False", None, "True"]
    assert out["i"].to_list() == ["1", "2", "3", "4"]
    assert out["f"].to_list() == ["1.5", "2.0", "0.0", "3.25"]


def test_dict_astype_only_changes_the_named_columns():
    out = _frame().astype({"b": "string"})
    assert out["b"].to_list() == ["True", "False", None, "True"]
    assert out["i"].to_list() == [1, 2, 3, 4]


def test_other_casts_of_a_bool_are_unchanged():
    assert _frame()["b"].astype("int64").to_list() == [1, 0, None, 1]
    assert _frame()["b"].astype("float64").to_list() == [1.0, 0.0, None, 1.0]


def test_a_string_column_that_says_true_is_not_touched():
    d = DataFrame.from_dict({"s": ["true", "false", "x"]})
    assert d["s"].astype("string").to_list() == ["true", "false", "x"]


def test_numbers_do_not_change_spelling():
    d = DataFrame.from_dict({"f": [2.0, 1e21, 1.5], "i": [7, 8, 9]})
    assert d["f"].astype(str).to_list() == ["2.0", "1e+21", "1.5"]
    assert d["i"].astype(str).to_list() == ["7", "8", "9"]
