# SPDX-License-Identifier: MIT
"""Series.name: stored natively, kept by pandas / polars conversion, by ops as
pandas does, by df["x"], to_frame, repr and pickle."""

import pickle

import pytest

from dftracer.utils import DataFrame, Series

pd = pytest.importorskip("pandas")


def test_default_is_none():
    assert Series([1, 2, 3]).name is None


def test_constructor_name():
    assert Series([1, 2], name="x").name == "x"
    assert Series(Series([1, 2], name="x")).name == "x"
    assert Series(Series([1, 2], name="x"), name="y").name == "y"
    with pytest.raises(TypeError):
        Series([1, 2], name=3)  # type: ignore[arg-type]


def test_setter_and_rename():
    s = Series([1, 2, 3])
    s.name = "x"
    assert s.name == "x"
    s.name = None
    assert s.name is None
    s.name = "x"
    t = s.rename("y")
    assert (s.name, t.name) == ("x", "y")
    assert t.to_list() == [1, 2, 3]
    assert s.rename(None).name is None
    with pytest.raises(TypeError):
        s.name = 5  # type: ignore[assignment]
    with pytest.raises(TypeError):
        s.rename(5)  # type: ignore[arg-type]


def test_pandas_round_trip():
    ps = pd.Series([1, 2, 3], name="lat")
    s = Series.from_pandas(ps)
    assert s.name == "lat"
    assert s.to_pandas().name == "lat"
    assert Series.from_pandas(pd.Series([1, 2])).name is None
    assert Series([1, 2]).to_pandas().name is None
    assert Series.from_pandas(pd.Series([1, 2], name=7)).name is None
    assert s.to_pandas(nullable=True).name == "lat"


def test_polars_round_trip():
    pl = pytest.importorskip("polars")
    s = Series.from_polars(pl.Series("lat", [1, 2, 3]))
    assert s.name == "lat"
    assert s.to_polars().name == "lat"
    assert Series.from_polars(pl.Series([1, 2])).name is None
    assert Series([1, 2]).to_polars().name == ""


def test_unary_ops_keep_the_name():
    s = Series([1, -2, 3], name="x")
    assert s.abs().name == "x"
    assert (s + 1).name == "x"
    assert (s * 2).name == "x"
    assert s.negate().name == "x"
    assert s.cumsum().name == "x"
    assert s.astype("float64").name == "x"
    assert s.fillna(0).name == "x"
    assert s.argsort().name == "x"
    assert s.clip(0, 2).name == "x"
    assert s.rolling(2).sum().name == "x"
    assert s.sort_values().name == "x"
    assert s.head(2).name == "x"
    assert s.shift(1).name == "x"
    assert (s > 0).name == "x"
    assert s.isna().name == "x"
    assert s[0:2].name == "x"
    assert s.reverse().name == "x"
    assert Series(["a", "b"], name="t").str.to_uppercase().name == "t"
    assert Series(["a", "b"], name="t").str.len_bytes().name == "t"
    assert s.diff().name == "x"
    assert s.where(s > 0, s).name == "x"
    assert s.take([0, 1]).name == "x"
    assert s.duplicated().name == "x"
    assert Series([[1, 2], [3]], name="l").list.len().name == "l"


def test_binary_ops_follow_pandas():
    a = Series([1, 2], name="x")
    same = Series([3, 4], name="x")
    other = Series([3, 4], name="y")
    bare = Series([3, 4])
    assert (a + same).name == "x"
    assert (a - same).name == "x"
    assert (a * same).name == "x"
    assert (a / same).name == "x"
    assert (a // same).name == "x"
    assert (a % same).name == "x"
    assert (a**same).name == "x"
    assert (a + other).name is None
    assert (a + bare).name is None
    assert (bare + a).name is None
    mask_a = Series([True, False], name="m")
    assert (mask_a & Series([True, True], name="m")).name == "m"
    assert (mask_a | Series([True, True], name="n")).name is None
    assert (~mask_a).name == "m"


def test_frame_columns_carry_the_column_name():
    s = Series([1, 2, 3], name="other")
    df = DataFrame({"x": s, "y": [4, 5, 6]})
    assert df["x"].name == "x"
    assert df["y"].name == "y"
    assert df.with_column("z", s)["z"].name == "z"
    assert DataFrame.from_dict({"k": s})["k"].name == "k"
    assert DataFrame.from_pandas(pd.DataFrame({"p": [1, 2]}))["p"].name == "p"


def test_to_frame():
    assert Series([1, 2], name="x").to_frame().columns == ["x"]
    assert Series([1, 2], name="x").to_frame("y").columns == ["y"]
    assert Series([1, 2]).to_frame().columns == ["0"]
    assert Series([1, 2], name="x").reset_index().columns == ["index", "x"]


def test_repr_footer():
    assert repr(Series([1, 2], name="x")).endswith("Name: x, dtype: int64")
    assert repr(Series([1, 2])).endswith("\ndtype: int64")
    assert "Name" not in repr(Series([1, 2]))
    long = Series(list(range(100)), name="x")
    assert repr(long).endswith("Name: x, Length: 100, dtype: int64")
    assert repr(Series([], name="x")) == "Series([], Name: x, dtype: string)"


def test_pickle_keeps_the_name():
    s = Series([1, 2, 3], name="x")
    back = pickle.loads(pickle.dumps(s))
    assert back.name == "x"
    assert back.to_list() == [1, 2, 3]
    assert pickle.loads(pickle.dumps(Series([1]))).name is None


def test_share_and_slice_keep_the_name():
    s = Series([1, 2, 3], name="x")
    assert s.share().name == "x"
    assert s.slice(1, 2).name == "x"
    assert s.rename("y").name == "y"
    assert s.name == "x"
