# SPDX-License-Identifier: MIT
import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame, Series


def _round(df):
    return DataFrame.from_pandas(df).to_pandas()


def test_category_with_null_round_trips():
    df = pd.DataFrame({"c": pd.Categorical(["a", "b", None, "a", "c"])})
    out = _round(df)
    assert isinstance(out["c"].dtype, pd.CategoricalDtype)
    assert set(out["c"].cat.categories) == {"a", "b", "c"}
    assert out["c"].isna().tolist() == [False, False, True, False, False]
    assert out["c"].astype(object).where(out["c"].notna(), None).tolist() == [
        "a",
        "b",
        None,
        "a",
        "c",
    ]


def test_category_import_is_a_dictionary_column():
    s = Series.from_pandas(pd.Series(pd.Categorical(["x", "y", "x"])))
    assert s.encoding == 2
    assert s.to_list() == ["x", "y", "x"]


def test_unused_categories_are_dropped():
    s = pd.Series(pd.Categorical(["a", "b"], categories=["z", "b", "a"]))
    out = Series.from_pandas(s).to_pandas()
    assert set(out.cat.categories) == {"a", "b"}


def test_category_order_is_kept():
    pytest.importorskip("pyarrow")
    s = pd.Series(pd.Categorical(["a", "c", None, "b"], categories=["c", "b", "a", "z"]))
    out = Series.from_pandas(s).to_pandas()
    assert out.cat.categories.tolist() == ["c", "b", "a"]
    assert out.astype(object).where(out.notna(), None).tolist() == ["a", "c", None, "b"]


def test_non_string_categories_keep_values():
    s = pd.Series(pd.Categorical([1, 2, 1]))
    native = Series.from_pandas(s)
    assert native.encoding != 2
    assert native.to_list() == [1, 2, 1]


@pytest.mark.parametrize("name", ["Int64", "UInt64", "Int8", "UInt32"])
def test_integers_with_nulls_stay_nullable(name):
    df = pd.DataFrame({"i": pd.array([1, None, 3], dtype=name)})
    out = _round(df)
    assert str(out["i"].dtype) == name
    assert out["i"].equals(df["i"])


def test_large_unsigned_with_nulls_is_exact():
    df = pd.DataFrame({"u": pd.array([2**63 + 5, None], dtype="UInt64")})
    out = _round(df)
    assert int(out["u"][0]) == 2**63 + 5
    assert out["u"][1] is pd.NA


def test_boolean_with_nulls():
    df = pd.DataFrame({"b": pd.array([True, None, False], dtype="boolean")})
    out = _round(df)
    assert str(out["b"].dtype) == "boolean"
    assert out["b"].equals(df["b"])


def test_string_with_nulls_is_pandas_string():
    df = pd.DataFrame({"s": pd.array(["a", None, "c"], dtype="string")})
    out = _round(df)
    assert isinstance(out["s"].dtype, pd.StringDtype)
    assert out["s"].isna().tolist() == [False, True, False]
    assert out["s"][0] == "a"


def test_columns_without_nulls_keep_numpy():
    df = pd.DataFrame({"i": np.array([1, 2, 3], dtype="int64"), "b": [True, False, True]})
    out = _round(df)
    assert out["i"].dtype == np.dtype("int64")
    assert out["b"].dtype == np.dtype("bool")


def test_float_with_nan_stays_float():
    df = pd.DataFrame({"f": [1.5, np.nan, 2.5]})
    out = _round(df)
    assert out["f"].dtype == np.dtype("float64")
    assert out["f"].isna().tolist() == [False, True, False]


def test_series_round_trips():
    cat = Series.from_pandas(pd.Series(pd.Categorical(["a", None, "b"]))).to_pandas()
    assert isinstance(cat.dtype, pd.CategoricalDtype)
    assert cat.isna().tolist() == [False, True, False]
    ints = Series.from_pandas(pd.Series(pd.array([1, None], dtype="Int64"))).to_pandas()
    assert str(ints.dtype) == "Int64"
    flags = Series.from_pandas(pd.Series(pd.array([True, None], dtype="boolean"))).to_pandas()
    assert str(flags.dtype) == "boolean"
    assert Series.from_pandas(pd.Series([1, 2])).to_pandas().dtype == np.dtype("int64")


def test_native_dictionary_column_gives_categorical():
    col = DataFrame.from_dict({"s": ["b", "a", "b", None]})["s"].dictionary_encode()
    assert col.encoding == 2
    out = col.to_pandas()
    assert isinstance(out.dtype, pd.CategoricalDtype)
    assert set(out.cat.categories) == {"a", "b"}
    assert out.isna().tolist() == [False, False, False, True]
    assert out[0] == "b"


def test_nullable_true_gives_categorical_for_dictionary():
    col = DataFrame.from_dict({"s": ["b", "a"]})["s"].dictionary_encode()
    assert isinstance(col.to_pandas(nullable=True).dtype, pd.CategoricalDtype)


def test_merge_and_groupby_on_nullable_result():
    df = pd.DataFrame({"k": pd.array([1, None, 1, 2], dtype="Int64"), "v": [1.0, 2.0, 3.0, 4.0]})
    out = _round(df)
    merged = out.merge(pd.DataFrame({"k": pd.array([1, 2], dtype="Int64"), "w": [10, 20]}), on="k")
    assert merged["w"].tolist() == [10, 10, 20]
    grouped = out.groupby("k")["v"].sum()
    assert grouped.loc[1] == 4.0
    assert grouped.loc[2] == 4.0


@pytest.mark.parametrize(
    "dictionary, want",
    [(["a", "b", "a"], ["a", "b", "a", None]), (["a", None, "c"], ["a", None, "c", None])],
)
def test_arrow_dictionary_with_repeats_or_nulls_gives_categorical(dictionary, want):
    pa = pytest.importorskip("pyarrow")
    arr = pa.DictionaryArray.from_arrays(
        pa.array([0, 1, 2, None], type=pa.int8()), pa.array(dictionary)
    )
    for out in (
        Series.from_arrow(arr).to_pandas(),
        Series.from_arrow(arr).to_pandas(nullable=True),
    ):
        assert isinstance(out.dtype, pd.CategoricalDtype)
        assert out.astype(object).where(out.notna(), None).tolist() == want
