import numpy as np
import pytest

from dftracer.utils import DataFrame, DType


def col():
    return DataFrame.from_dict({"i": [1, 2]})["i"]


def test_python_types():
    c = col()
    assert c.astype(int).to_list() == [1, 2]
    assert c.astype(float).to_list() == [1.0, 2.0]
    assert c.astype(str).to_list() == ["1", "2"]
    assert c.astype(bool).to_list() == [True, True]
    assert c.astype(int).type == DType.INT64
    assert c.astype(float).type == DType.FLOAT64


def test_numpy_types():
    c = col()
    assert c.astype(np.int64).type == DType.INT64
    assert c.astype(np.float64).type == DType.FLOAT64
    assert c.astype(np.dtype("float32")).type == DType.FLOAT32
    assert c.astype(np.dtype("int32")).type == DType.INT32
    assert c.astype(np.bool_).type == DType.BOOL
    assert c.astype(np.str_).type == DType.STRING


def test_names_and_codes_still_work():
    c = col()
    assert c.astype("Int64").type == DType.INT64
    assert c.astype(DType.FLOAT64).type == DType.FLOAT64
    assert c.astype(int(DType.INT32)).type == DType.INT32


def test_unknown_type_lists_accepted_forms():
    with pytest.raises(TypeError, match="Python type"):
        col().astype(object)
    with pytest.raises(TypeError, match="Python type"):
        col().astype(None)
    with pytest.raises(ValueError):
        col().astype("nope")
