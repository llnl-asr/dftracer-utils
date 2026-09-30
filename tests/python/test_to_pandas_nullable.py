import math

import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame


def test_integers_with_nulls_keep_their_type():
    d = DataFrame.from_dict({"i": [1, None, 3]})
    out = d.to_pandas(nullable=True)
    assert str(out["i"].dtype) == "Int64"
    assert out["i"][0] == 1
    assert out["i"][1] is pd.NA
    assert out["i"].isna().tolist() == [False, True, False]


def test_every_integer_width_maps_to_its_pandas_name():
    for dtype, name in [
        ("int8", "Int8"),
        ("int16", "Int16"),
        ("int32", "Int32"),
        ("uint8", "UInt8"),
        ("uint64", "UInt64"),
    ]:
        s = DataFrame.from_dict({"x": [1, None, 3]})["x"].astype(dtype)
        assert str(s.to_pandas(nullable=True).dtype) == name


def test_unsigned_values_above_int64_are_exact():
    s = DataFrame.from_dict({"u": [2**63 + 5, None]})["u"]
    out = s.to_pandas(nullable=True)
    assert str(out.dtype) == "UInt64"
    assert int(out[0]) == 2**63 + 5


def test_bool_and_string_with_nulls():
    d = DataFrame.from_dict({"b": [True, None, False], "s": ["a", None, "c"]})
    out = d.to_pandas(nullable=True)
    assert str(out["b"].dtype) == "boolean"
    assert str(out["s"].dtype) == "string"
    assert out["b"][1] is pd.NA
    assert out["s"][1] is pd.NA
    assert out["b"][0] is np.True_ or out["b"][0] == True  # noqa: E712


def test_float_nan_is_not_a_null():
    d = DataFrame.from_dict({"f": [1.5, float("nan"), None]})
    out = d.to_pandas(nullable=True)
    assert str(out["f"].dtype) == "Float64"
    assert math.isnan(out["f"][1])
    assert out["f"][2] is pd.NA
    assert out["f"].isna().tolist() == [False, False, True]  # only the null is NA


def test_float32_keeps_its_width():
    s = DataFrame.from_dict({"f": [1.5, None]})["f"].astype("float32")
    assert str(s.to_pandas(nullable=True).dtype) == "Float32"


def test_default_is_unchanged():
    out = DataFrame.from_dict({"i": [1, None, 3]}).to_pandas()
    assert str(out["i"].dtype) == "float64"
    assert math.isnan(out["i"][1])


def test_column_without_a_nullable_dtype_converts_as_before():
    d = DataFrame.from_dict({"l": [[1, 2], [3]], "i": [1, 2]})
    out = d.to_pandas(nullable=True)
    assert out["l"].tolist() == [[1, 2], [3]]
    assert str(out["i"].dtype) == "Int64"


def test_series_nullable():
    s = DataFrame.from_dict({"i": [1, None, 3]})["i"]
    out = s.to_pandas(nullable=True)
    assert str(out.dtype) == "Int64"
    assert out.isna().tolist() == [False, True, False]


def test_series_nullable_without_nulls():
    out = DataFrame.from_dict({"i": [1, 2, 3]})["i"].to_pandas(nullable=True)
    assert str(out.dtype) == "Int64"
    assert out.tolist() == [1, 2, 3]


def test_arrow_and_nullable_together_is_an_error():
    d = DataFrame.from_dict({"i": [1]})
    with pytest.raises(ValueError, match="arrow"):
        d.to_pandas(arrow=True, nullable=True)
    with pytest.raises(ValueError, match="arrow"):
        d["i"].to_pandas(arrow=True, nullable=True)


def test_empty_frame_nullable():
    d = DataFrame.from_dict({"i": [1, 2]}).head(0)
    assert str(d.to_pandas(nullable=True)["i"].dtype) == "Int64"
