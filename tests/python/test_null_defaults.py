"""where and mask default to null, and None is an all-null String constant column."""

from dftracer.utils import DataFrame, lit
from dftracer.utils.enums import DType


def test_where_without_a_replacement_is_null_where_false():
    d = DataFrame.from_dict({"x": [1.0, 2.0, None]})
    assert d["x"].where(d["x"] > 1).to_list() == [None, 2.0, None]


def test_an_integer_column_keeps_its_type():
    d = DataFrame.from_dict({"i": [1, 2, 3]})
    out = d["i"].where(d["i"] > 1)
    assert out.to_list() == [None, 2, 3]
    assert out.dtype == DType.INT64


def test_mask_without_a_replacement_is_null_where_true():
    d = DataFrame.from_dict({"x": [1.0, 2.0, None]})
    assert d["x"].mask(d["x"] > 1).to_list() == [1.0, None, None]


def test_the_frame_forms_default_to_null():
    d = DataFrame.from_dict({"a": [1.0, 2.0, 3.0], "i": [1, 2, 3]})
    keep = d["i"] == 2
    out = d.where(keep).to_pandas()
    assert out["a"].isna().tolist() == [True, False, True]
    assert out["i"].isna().tolist() == [True, False, True]
    out = d.mask(keep).to_pandas()
    assert out["a"].isna().tolist() == [False, True, False]


def test_an_explicit_replacement_still_works():
    d = DataFrame.from_dict({"x": [1.0, 2.0, 3.0]})
    assert d["x"].where(d["x"] > 1, 0).to_list() == [0.0, 2.0, 3.0]
    assert d["x"].mask(d["x"] > 1, -1).to_list() == [1.0, -1.0, -1.0]


def test_none_and_lit_none_add_an_all_null_string_column():
    d = DataFrame.from_dict({"x": [1.0, 2.0, 3.0]})
    for value in (None, lit(None)):
        out = d.with_columns(z=value)
        assert out["z"].to_list() == [None, None, None]
        assert out["z"].dtype == DType.STRING


def test_the_null_constant_has_the_type_of_a_column_of_only_none():
    only_none = DataFrame.from_dict({"z": [None, None]})
    added = DataFrame.from_dict({"x": [1, 2]}).with_columns(z=None)
    assert only_none["z"].dtype == added["z"].dtype


def test_a_frame_with_no_rows_gets_an_empty_column():
    empty = DataFrame.from_dict({"a": []}).with_columns(z=None)
    assert len(empty["z"].to_list()) == 0
    assert "z" in empty.columns
