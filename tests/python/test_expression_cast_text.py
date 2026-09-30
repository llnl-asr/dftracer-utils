"""A column expression can cast to text, as the eager astype can."""

import pytest

from dftracer.utils import DataFrame, col


def _frame():
    return DataFrame.from_dict(
        {
            "b": [True, False, None],
            "i": [1, 2, 3],
            "f": [1.5, 2.0, 0.0],
            "s": ["7", "x", "9"],
        }
    )


def _collect(expr):
    return _frame().lazy().with_columns(t=expr).collect()["t"].to_list()


def test_integer_to_string():
    assert _collect(col("i").cast("string")) == ["1", "2", "3"]


def test_float_to_string_shows_a_point_or_exponent():
    assert _collect(col("f").cast("string")) == ["1.5", "2.0", "0.0"]


def test_bool_to_string_uses_the_engine_spelling_and_keeps_nulls():
    # The eager Series.astype says True / False; the engine spells a bool true / false.
    assert _collect(col("b").cast("string")) == ["true", "false", None]


def test_number_to_bool_and_string_to_number_still_work():
    assert _collect(col("i").cast("bool")) == [True, True, True]
    assert _collect(col("s").cast("int64")) == [7, None, 9]


def test_eager_with_columns_takes_the_same_expression():
    assert _frame().with_columns(t=col("i").cast("string"))["t"].to_list() == ["1", "2", "3"]


def test_the_cast_text_can_be_filtered_on():
    out = _frame().lazy().with_columns(t=col("i").cast("string")).filter(col("t") == "2").collect()
    assert out["i"].to_list() == [2]


def test_an_unknown_target_is_still_a_key_error():
    with pytest.raises(KeyError):
        col("i").cast("decimal")
