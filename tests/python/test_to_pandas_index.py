import pandas as pd
import pytest

from dftracer.utils import DataFrame


def frame():
    return DataFrame.from_dict(
        {"name": ["a", "a", "b"], "hash": ["h1", "h2", "h1"], "x": [1.0, 2.0, 3.0]}
    )


def test_two_level_index():
    out = frame().to_pandas(index=["name", "hash"])
    assert isinstance(out.index, pd.MultiIndex)
    assert list(out.index.names) == ["name", "hash"]
    assert list(out.columns) == ["x"]
    assert out.index.get_level_values("name").tolist() == ["a", "a", "b"]
    assert out["x"].tolist() == [1.0, 2.0, 3.0]


def test_one_name_gives_a_flat_index():
    out = frame().to_pandas(index=["name"])
    assert not isinstance(out.index, pd.MultiIndex)
    assert out.index.name == "name"
    assert list(out.columns) == ["hash", "x"]


def test_a_bare_string_is_one_name():
    assert frame().to_pandas(index="hash").index.name == "hash"


def test_order_of_the_names_is_the_order_of_the_levels():
    assert list(frame().to_pandas(index=["hash", "name"]).index.names) == ["hash", "name"]


def test_unknown_name():
    with pytest.raises(KeyError, match="nope"):
        frame().to_pandas(index=["nope"])


def test_repeated_name():
    with pytest.raises(ValueError, match="name"):
        frame().to_pandas(index=["name", "name"])


def test_empty_index_list():
    with pytest.raises(ValueError):
        frame().to_pandas(index=[])


def test_null_in_an_index_column_stays_null():
    d = DataFrame.from_dict({"k": ["a", None], "x": [1, 2]})
    out = d.to_pandas(index=["k"])
    assert out.index.isna().tolist() == [False, True]


def test_index_with_nullable_dtypes():
    out = frame().to_pandas(nullable=True, index=["name", "hash"])
    assert str(out["x"].dtype) == "Float64"
    assert list(out.index.names) == ["name", "hash"]


def test_round_trip_through_from_pandas_of_a_multiindex_frame():
    out = frame().to_pandas(index=["name", "hash"])
    back = DataFrame.from_pandas(out.reset_index())
    assert back.to_pandas().equals(frame().to_pandas())
