import pytest

from dftracer.utils import DataFrame, col


def frame():
    return DataFrame.from_dict({"a": [1, 5, 3], "b": [2, 4, 3]})


def rows(df):
    return df.to_pandas().to_dict("list")


def test_keep_rows_where_a_is_at_least_b():
    assert rows(frame().filter(col("a") >= col("b"))) == {"a": [5, 3], "b": [4, 3]}


def test_the_series_mask_gives_the_same_rows():
    f = frame()
    assert rows(f.filter(col("a") >= col("b"))) == rows(f[f["a"] >= f["b"]])


@pytest.mark.parametrize(
    "build, keep",
    [
        (lambda: col("a") == col("b"), [3]),
        (lambda: col("a") != col("b"), [1, 5]),
        (lambda: col("a") < col("b"), [1]),
        (lambda: col("a") <= col("b"), [1, 3]),
        (lambda: col("a") > col("b"), [5]),
        (lambda: col("a") >= col("b"), [3, 5]),
    ],
)
def test_all_six_comparisons(build, keep):
    assert sorted(rows(frame().filter(build()))["a"]) == keep


def test_null_on_either_side_is_left_out():
    d = DataFrame.from_dict({"a": [1, None, 3], "b": [2, 4, None]})
    assert rows(d.filter(col("a") < col("b"))) == {"a": [1], "b": [2]}


def test_numeric_columns_of_different_types_compare():
    d = DataFrame.from_dict({"i": [1, 2, 3], "f": [1.5, 2.0, 2.5]})
    assert rows(d.filter(col("i") > col("f")))["i"] == [3]


def test_string_columns_compare_bytewise():
    d = DataFrame.from_dict({"x": ["a", "c"], "y": ["b", "b"]})
    assert rows(d.filter(col("x") < col("y")))["x"] == ["a"]


def test_columns_that_cannot_compare_name_both_types():
    d = DataFrame.from_dict({"s": ["a", "b"], "i": [1, 2]})
    with pytest.raises(Exception) as e:
        d.filter(col("s") < col("i"))
    msg = str(e.value).lower()
    assert "string" in msg and "int" in msg


def test_lazy_frame_equals_eager():
    f = frame()
    lazy = f.lazy().filter(col("a") >= col("b")).collect()
    assert rows(lazy) == rows(f.filter(col("a") >= col("b")))


def test_with_a_scalar_the_old_form_still_works():
    assert rows(frame().filter(col("a") > 2))["a"] == [5, 3]


def test_a_column_comparison_is_not_pushable_to_duql():
    with pytest.raises(TypeError):
        (col("a") >= col("b")).to_duql()


def test_two_expressions_compare():
    d = DataFrame.from_dict({"a": [1, 5, 3], "b": [2, 4, 3]})
    assert rows(d.filter(col("a") + 1 > col("b") * 1))["a"] == [5, 3]
