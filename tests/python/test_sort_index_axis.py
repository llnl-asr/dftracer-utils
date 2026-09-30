import pytest

from dftracer.utils import DataFrame


def frame():
    return DataFrame.from_dict({"c": [3, 1], "a": [9, 8], "b": ["x", "y"]})


def test_columns_sorted_ascending():
    out = frame().sort_index(axis=1)
    assert list(out.columns) == ["a", "b", "c"]
    assert out.to_pandas().to_dict("list") == {"a": [9, 8], "b": ["x", "y"], "c": [3, 1]}


def test_axis_name_columns():
    assert list(frame().sort_index(axis="columns").columns) == ["a", "b", "c"]


def test_descending():
    assert list(frame().sort_index(ascending=False, axis=1).columns) == ["c", "b", "a"]


def test_axis_zero_is_unchanged():
    f = frame()
    assert list(f.sort_index().columns) == ["c", "a", "b"]
    assert f.sort_index(axis=0).to_pandas().equals(f.to_pandas())


def test_values_are_unchanged():
    f = frame()
    sorted_cols = f.sort_index(axis=1)
    for name in f.columns:
        assert sorted_cols[name].to_list() == f[name].to_list()


def test_bad_axis():
    with pytest.raises(ValueError):
        frame().sort_index(axis=2)
