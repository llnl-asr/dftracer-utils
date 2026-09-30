import pandas as pd
import pytest

from dftracer.utils import DataFrame, DType, col, lit


def frame():
    return DataFrame.from_dict({"g": ["a", "a", "a", "b"], "x": [1.0, None, 3.0, 5.0]})


# ---- where with a null replacement -------------------------------------------


def test_where_with_none():
    d = frame()
    assert d["x"].where(d["x"] > 2, None).to_list() == [None, None, 3.0, 5.0]


def test_where_with_pd_na():
    d = frame()
    assert d["x"].where(d["x"] > 2, pd.NA).to_list() == [None, None, 3.0, 5.0]


def test_where_null_keeps_an_integer_column_integer():
    s = DataFrame.from_dict({"i": [1, 2, 3]})["i"]
    out = s.where(s > 1, None)
    assert out.to_list() == [None, 2, 3]
    assert out.type == DType.INT64


def test_where_null_on_a_string_column():
    s = DataFrame.from_dict({"s": ["a", "b", "c"]})["s"]
    assert s.where(s != "b", None).to_list() == ["a", None, "c"]


def test_mask_with_none():
    d = frame()
    assert d["x"].mask(d["x"] > 2, None).to_list() == [1.0, None, None, None]


def test_where_other_still_takes_numbers_and_series():
    d = frame()
    assert d["x"].where(d["x"] > 2, 0.0).to_list() == [0.0, 0.0, 3.0, 5.0]
    with pytest.raises(TypeError, match="Series, a number or null"):
        d["x"].where(d["x"] > 2, "text")


# ---- one-sided clip ------------------------------------------------------------


def test_clip_lower_only():
    s = DataFrame.from_dict({"x": [-2, 0, 3, None]})["x"]
    assert s.clip(lower=0).to_list() == [0, 0, 3, None]
    assert s.clip(lower=0).type == DType.INT64


def test_clip_upper_only():
    s = DataFrame.from_dict({"x": [-2.5, 0.0, 3.5, None]})["x"]
    assert s.clip(upper=1).to_list() == [-2.5, 0.0, 1.0, None]


def test_clip_positional_one_bound():
    s = DataFrame.from_dict({"x": [-2, 5]})["x"]
    assert s.clip(0).to_list() == [0, 5]
    assert s.clip(None, 3).to_list() == [-2, 3]


def test_clip_both_bounds_unchanged():
    s = DataFrame.from_dict({"x": [-2, 5, 9]})["x"]
    assert s.clip(0, 6).to_list() == [0, 5, 6]


@pytest.mark.parametrize(
    "dtype",
    [
        "int8",
        "int16",
        "int32",
        "int64",
        "uint8",
        "uint16",
        "uint32",
        "uint64",
        "float32",
        "float64",
    ],
)
def test_one_sided_clip_for_every_numeric_width(dtype):
    s = DataFrame.from_dict({"x": [1, 5, 9]})["x"].astype(dtype)
    out = s.clip(lower=3)
    assert out.type == s.type
    assert out.to_list() == [3, 5, 9]
    assert s.clip(upper=6).to_list() == [1, 5, 6]


def test_clip_with_no_bound():
    with pytest.raises(TypeError, match="bound"):
        DataFrame.from_dict({"x": [1]})["x"].clip()


def test_clip_one_bound_on_a_string_column():
    with pytest.raises(TypeError, match="numeric"):
        DataFrame.from_dict({"s": ["a"]})["s"].clip(lower=0)


def test_frame_clip_one_bound():
    d = DataFrame.from_dict({"a": [-1, 4], "b": [-2.0, 3.0]})
    assert d.clip(lower=0).to_pandas().to_dict("list") == {"a": [0, 4], "b": [0.0, 3.0]}


# ---- frame-wide astype ---------------------------------------------------------


def test_astype_one_dtype_for_every_column():
    d = DataFrame.from_dict({"a": [1, 2], "b": [3, 4]})
    out = d.astype("float64")
    assert out["a"].type == DType.FLOAT64 and out["b"].type == DType.FLOAT64
    assert out.to_pandas().to_dict("list") == {"a": [1.0, 2.0], "b": [3.0, 4.0]}


def test_astype_one_python_type():
    out = DataFrame.from_dict({"a": [1, 2], "b": [3, 4]}).astype(str)
    assert out["a"].to_list() == ["1", "2"]


def test_astype_names_the_column_that_cannot_be_cast():
    d = DataFrame.from_dict({"a": [1, 2], "s": ["x", "y"]})
    with pytest.raises(TypeError) as e:
        d.astype("bool")
    msg = str(e.value)
    assert "'s'" in msg and "string" in msg and "bool" in msg


def test_failed_astype_returns_no_partial_frame():
    d = DataFrame.from_dict({"a": [1, 2], "s": ["x", "y"]})
    with pytest.raises(TypeError):
        d.astype("bool")
    assert d["a"].type == DType.INT64  # the original frame is untouched


def test_astype_dict_still_casts_only_the_named_columns():
    d = DataFrame.from_dict({"a": [1, 2], "b": [3, 4]})
    out = d.astype({"a": "float64"})
    assert out["a"].type == DType.FLOAT64 and out["b"].type == DType.INT64


# ---- constant columns -----------------------------------------------------------


def test_scalar_and_lit_columns():
    d = DataFrame.from_dict({"x": [1, 2, 3, 4]})
    assert d.with_columns(z=1).to_pandas()["z"].tolist() == [1, 1, 1, 1]
    assert d.with_columns(z=lit(1)).to_pandas()["z"].tolist() == [1, 1, 1, 1]


def test_float_string_and_bool_constants():
    d = DataFrame.from_dict({"x": [1, 2]})
    out = d.with_columns(f=2.5, s="k", b=True).to_pandas()
    assert out["f"].tolist() == [2.5, 2.5]
    assert out["s"].tolist() == ["k", "k"]
    assert out["b"].tolist() == [True, True]


def test_constant_column_in_a_frame_with_no_rows():
    d = DataFrame.from_dict({"x": [1, 2]}).head(0)
    out = d.with_columns(z=1)
    assert len(out) == 0
    assert out["z"].type == DType.INT64
    out2 = d.with_columns(z=lit(1))
    assert len(out2) == 0 and out2["z"].type == DType.INT64


def test_assign_a_scalar_keeps_existing_columns():
    d = DataFrame.from_dict({"x": [1, 2]})
    out = d.assign(y=7)
    assert list(out.columns) == ["x", "y"]
    assert out["x"].to_list() == [1, 2]


def test_constant_expression_expression_still_works_with_a_column():
    d = DataFrame.from_dict({"x": [1, 2]})
    assert d.with_columns(z=col("x") * 0 + 3).to_pandas()["z"].tolist() == [3, 3]


# ---- a constant column never reads another column's values (defect N1) -------


def test_constant_with_a_null_in_the_first_column():
    d = DataFrame.from_dict({"x": [1.0, None, 3.0]})
    for z in (1, lit(1)):
        out = d.with_columns(z=z)
        assert out["z"].to_list() == [1, 1, 1]
        assert out["z"].type == DType.INT64
        assert out["z"].null_count == 0


def test_constant_in_a_string_first_frame_with_nulls():
    d = DataFrame.from_dict({"s": ["a", None, "c"], "x": [1, 2, 3]})
    for z in (1, lit(1)):
        out = d.with_columns(z=z)
        assert out["z"].to_list() == [1, 1, 1]
        assert out["z"].null_count == 0


def test_constant_with_nan_and_infinity_in_another_column():
    d = DataFrame.from_dict({"x": [1.0, float("nan"), float("inf")]})
    for z in (2.5, lit(2.5)):
        out = d.with_columns(z=z)
        assert out["z"].to_list() == [2.5, 2.5, 2.5]
        assert out["z"].type == DType.FLOAT64
        assert out["z"].null_count == 0


def test_constant_bool_and_string_with_nulls_in_the_first_column():
    d = DataFrame.from_dict({"x": [None, 2.0, None]})
    out = d.with_columns(b=True, s="k")
    assert out["b"].to_list() == [True, True, True]
    assert out["s"].to_list() == ["k", "k", "k"]


def test_constant_does_not_change_the_other_columns():
    d = DataFrame.from_dict({"x": [1.0, None, 3.0]})
    out = d.with_columns(z=7)
    assert out["x"].to_list() == [1.0, None, 3.0]


def test_constant_large_frame_has_the_value_in_every_row():
    n = 1_000_000
    d = DataFrame.from_dict({"x": [None] * 10 + [1.0] * (n - 10)})
    out = d.with_columns(z=3)
    assert len(out) == n and out["z"].null_count == 0 and out["z"].sum() == 3 * n


def test_constant_lazy_equals_eager_with_a_null_first_column():
    d = DataFrame.from_dict({"x": [1.0, None, 3.0]})
    lazy = d.lazy().with_columns(z=lit(1)).collect()
    assert lazy["z"].to_list() == [1, 1, 1]
    assert lazy["z"].to_list() == d.with_columns(z=lit(1))["z"].to_list()


def test_constant_expression_mixed_with_a_null_column_is_unchanged():
    d = DataFrame.from_dict({"x": [1.0, None, 3.0]})
    assert d.with_columns(w=col("x") + 1)["w"].to_list() == [2.0, None, 4.0]
