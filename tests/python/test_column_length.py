# SPDX-License-Identifier: MIT
"""A column whose length differs from the frame's row count is an error."""

import pytest

from dftracer.utils import DataFrame, col, lit

NAN = float("nan")


def test_with_columns_refuses_shorter_column():
    df = DataFrame({"a": [1.0, None, 3.0]})
    with pytest.raises(ValueError, match="length"):
        df.with_columns(col("a").drop_nulls().alias("b"))


def test_assign_refuses_shorter_column():
    df = DataFrame({"a": [1.0, None, 3.0]})
    with pytest.raises(ValueError, match="length"):
        df.assign(b=col("a").drop_nulls())


def test_select_refuses_columns_of_different_lengths():
    df = DataFrame({"a": [1.0, NAN, 3.0], "b": [1, 2, 3]})
    with pytest.raises(ValueError, match="length"):
        df.select(col("a").drop_nans().alias("a"), col("b"))


def test_single_column_select_may_be_shorter_than_the_frame():
    df = DataFrame({"a": [1.0, NAN, 3.0], "b": [1, 2, 3]})
    out = df.select(col("a").drop_nans().alias("a"))
    assert out.columns == ["a"]
    assert len(out) == 2


def test_equal_length_adds_still_work():
    df = DataFrame({"a": [1, 2, 3]})
    out = df.with_columns((col("a") * 2).alias("b")).assign(c=7)
    assert out.columns == ["a", "b", "c"]
    assert len(out) == 3
    assert DataFrame({"a": [1, 2, 3]}).select((col("a") + 1).alias("d"), col("a")).columns == [
        "d",
        "a",
    ]


def test_select_items_all_see_the_input_frame():
    df = DataFrame({"a": [1, 2, 3]})
    out = df.select((col("a") * 2).alias("a"), (col("a") + 1).alias("c"))
    assert out["a"].to_list() == [2, 4, 6]
    assert out["c"].to_list() == [2, 3, 4]


def test_literal_in_a_short_select_takes_the_common_length():
    df = DataFrame({"a": [1.0, NAN, 3.0], "b": [1, 2, 3]})
    out = df.select(col("a").drop_nans().alias("a"), lit(0).alias("z"))
    assert out.columns == ["a", "z"]
    assert out["a"].to_list() == [1.0, 3.0]
    assert out["z"].to_list() == [0, 0]


def test_literal_with_plain_column_takes_the_frame_length():
    df = DataFrame({"a": [1, 2, 3]})
    out = df.select("a", lit(5).alias("z"))
    assert out["z"].to_list() == [5, 5, 5]
