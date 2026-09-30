"""The native one-pass conversions behind to_pandas, to_numpy and to_list:
values, nulls and dtypes equal what pandas holds, for every width, with nulls
at the byte edges of the validity bitmap, and repeated strings share objects."""

import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame, Series

N = 1003  # not a multiple of 8, so the last validity byte is partial


def with_nulls(values, every=7):
    s = pd.Series(values)
    s = s.astype(
        {
            np.dtype("int8"): "Int8",
            np.dtype("int16"): "Int16",
            np.dtype("int32"): "Int32",
            np.dtype("int64"): "Int64",
            np.dtype("uint8"): "UInt8",
            np.dtype("uint16"): "UInt16",
            np.dtype("uint32"): "UInt32",
            np.dtype("uint64"): "UInt64",
            np.dtype("float32"): "Float32",
            np.dtype("float64"): "Float64",
        }[s.dtype]
    )
    s.iloc[::every] = pd.NA
    s.iloc[-1] = pd.NA  # the partial byte
    return s


@pytest.mark.parametrize(
    "dt",
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
def test_numeric_with_nulls_nullable_and_default(dt):
    rng = np.random.default_rng(1)
    raw = rng.integers(0, 100, N).astype(dt) if "int" in dt else (rng.random(N) * 100).astype(dt)
    want = with_nulls(raw)
    s = Series.from_pandas(want)
    got = s.to_pandas(nullable=True)
    assert str(got.dtype) == str(want.dtype)
    assert got.equals(want)
    plain = s.to_pandas()
    expect = want.astype("float32" if dt == "float32" else "float64")
    assert plain.isna().equals(want.isna())
    assert np.allclose(plain.dropna().to_numpy(), expect.dropna().to_numpy())
    assert s.to_list() == [
        None if v is pd.NA else v.item() if hasattr(v, "item") else v for v in want.tolist()
    ]


@pytest.mark.parametrize("dt", ["int64", "float64"])
def test_numeric_without_nulls_is_exact(dt):
    raw = np.arange(N).astype(dt)
    s = Series.from_pandas(pd.Series(raw))
    assert np.array_equal(s.to_numpy(), raw)
    assert s.to_pandas().equals(pd.Series(raw))
    assert s.to_list() == raw.tolist()


def test_bool_with_and_without_nulls():
    rng = np.random.default_rng(2)
    flags = pd.array(rng.random(N) < 0.5, dtype="boolean")
    flags[::5] = pd.NA
    flags[-1] = pd.NA
    s = Series.from_pandas(pd.Series(flags))
    got = s.to_pandas(nullable=True)
    assert str(got.dtype) == "boolean" and got.equals(pd.Series(flags))
    assert s.to_list() == [None if v is pd.NA else bool(v) for v in flags]
    solid = Series.from_pandas(pd.Series(rng.random(N) < 0.5))
    assert solid.to_numpy().dtype == np.bool_


def test_strings_with_nulls_unicode_and_sharing():
    names = ["app#host#1#1", "", "café", "日本語", "a" * 40]
    values = [names[i % len(names)] for i in range(N)]
    s = pd.Series(pd.array(values, dtype="string"))
    s.iloc[::9] = pd.NA
    s.iloc[-1] = pd.NA
    col = Series.from_pandas(s)
    got = col.to_pandas(nullable=True)
    assert got.equals(s)
    obj = col.to_numpy()
    assert obj.dtype == object
    assert [None if v is pd.NA else v for v in s.tolist()] == [
        None if v is None else v for v in obj.tolist()
    ]
    lst = col.to_list()
    assert lst == [None if v is pd.NA else v for v in s.tolist()]
    # repeated text is one object, not one per row
    firsts = [v for v in lst if v == "app#host#1#1"]
    assert len({id(v) for v in firsts}) == 1


def test_dictionary_encoded_strings_materialize():
    s = pd.Series(["x", "y", "x", None, "z", "y"] * 50)
    col = DataFrame.from_pandas(pd.DataFrame({"s": s}))["s"].dictionary_encode()
    assert col.to_list() == [None if pd.isna(v) else v for v in s.tolist()]
    assert col.to_pandas(nullable=True).equals(pd.Series(pd.array(s.tolist(), dtype="string")))


def test_frame_round_trips_every_column_kind():
    rng = np.random.default_rng(3)
    df = pd.DataFrame(
        {
            "i": with_nulls(rng.integers(0, 50, N).astype("int64")),
            "f": with_nulls((rng.random(N) * 9).astype("float64")),
            "b": pd.array(rng.random(N) < 0.3, dtype="boolean"),
            "s": pd.array([f"n{i % 17}" for i in range(N)], dtype="string"),
        }
    )
    df.loc[::11, "b"] = pd.NA
    df.loc[::13, "s"] = pd.NA
    back = DataFrame.from_pandas(df).to_pandas(nullable=True)
    for c in df.columns:
        assert str(back[c].dtype) == str(df[c].dtype), c
        assert back[c].equals(df[c]), c


def test_str_into_checks_its_destination():
    col = DataFrame.from_dict({"s": ["a", "b", "c"]})["s"]
    with pytest.raises(ValueError):
        col._native.str_into(np.empty(2, dtype=object), None)  # wrong length
    with pytest.raises(ValueError):
        col._native.str_into(np.empty(3, dtype=np.int64), None)  # not an object array
    with pytest.raises(TypeError):
        DataFrame.from_dict({"x": [1, 2]})["x"]._native.str_into(np.empty(2, dtype=object), None)
