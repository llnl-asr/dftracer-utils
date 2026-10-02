"""to_polars / to_pandas through Arrow equal the native paths; string columns
convert to Python objects without being materialized."""

import numpy as np
import pytest

from dftracer.utils import DataFrame, Series
from dftracer.utils.series import _native_numpy, _polars_native

pa = pytest.importorskip("pyarrow")
pd = pytest.importorskip("pandas")
pl = pytest.importorskip("polars")

LONG = "a string longer than twelve bytes"
VALUES = ["x", None, "y", "x", LONG, "z", None, "x", LONG, ""]
N = len(VALUES)
MORSEL = 3


def _array(layout):
    arr = pa.array(VALUES)
    if layout == "view":
        return arr.cast(pa.string_view())
    if layout == "dictionary":
        return arr.dictionary_encode()
    return arr


def _series(layout, chunked):
    s = Series(_array(layout))
    if not chunked:
        return s
    out = DataFrame({"s": s}).lazy().collect(morsel_rows=MORSEL)["s"]
    assert out.encoding == 5
    return out


def _layouts():
    return [
        pytest.param(layout, chunked, id="%s-%s" % (layout, "chunked" if chunked else "one"))
        for layout in ("flat", "view", "dictionary")
        for chunked in (False, True)
    ]


def _frame(layout, chunked):
    n = N
    base = DataFrame(
        {
            "i": Series(pa.array([None if k == 4 else k for k in range(n)], pa.int64())),
            "f": Series(pa.array([None if k == 2 else k / 4 for k in range(n)], pa.float64())),
            "b": Series(pa.array([k % 2 == 0 for k in range(n)])),
            "s": Series(_array(layout)),
        }
    )
    if not chunked:
        return base
    out = base.lazy().collect(morsel_rows=MORSEL)
    assert out["s"].encoding == 5
    return out


def _needs_capsule(native, name):
    if not hasattr(native, name):
        pytest.skip(
            "extension built without Arrow: %s is absent, to_polars is the native path" % name
        )


@pytest.mark.parametrize("layout,chunked", _layouts())
def test_series_to_polars_matches_native(layout, chunked):
    s = _series(layout, chunked)
    _needs_capsule(s._native, "__arrow_c_array__")
    got = s.to_polars()
    want = _polars_native(s._native, pl)
    assert got.dtype == want.dtype
    assert got.to_list() == want.to_list() == VALUES


@pytest.mark.parametrize("layout,chunked", _layouts())
def test_dataframe_to_polars_matches_native(layout, chunked):
    df = _frame(layout, chunked)
    _needs_capsule(df._native, "__arrow_c_stream__")
    got = df.to_polars()
    want = pl.DataFrame({name: _polars_native(df[name]._native, pl) for name in df.columns})
    assert got.schema == want.schema
    assert got.equals(want)


def test_float16_and_decimal_stay_native():
    import decimal

    f16 = Series(pa.array(np.array([1, 2], dtype=np.float16)))
    assert f16.to_polars().dtype == pl.Float32
    dec = Series(pa.array([decimal.Decimal("1.50"), None], pa.decimal128(10, 2)))
    assert dec.to_polars().to_list() == [decimal.Decimal("1.50"), None]
    mixed = DataFrame(
        {
            "a": Series([1, 2]),
            "d": Series(pa.array([None, decimal.Decimal("2.25")], pa.decimal128(10, 2))),
        }
    )
    assert mixed.to_polars().columns == ["a", "d"]


def _pandas_native_strings(s):
    return pd.Series(_native_numpy(s._native))


def _arrow_pandas_strings():
    string = pd.Series(np.array(["x"], dtype=object)).dtype
    if not (isinstance(string, pd.StringDtype) and string.storage == "pyarrow"):
        pytest.skip(
            "pandas keeps strings in Python objects here (dtype %s); the Arrow branch is not taken"
            % string
        )


@pytest.mark.parametrize("layout,chunked", _layouts())
def test_series_to_pandas_strings_match_native(layout, chunked):
    _arrow_pandas_strings()
    s = _series(layout, chunked)
    _needs_capsule(s._native, "__arrow_c_array__")
    got = s.to_pandas()
    want = _pandas_native_strings(s)
    assert got.dtype == want.dtype
    assert got.equals(want)
    assert got.isna().tolist() == [v is None for v in VALUES]
    assert [v for v in got if isinstance(v, str)] == [v for v in VALUES if v is not None]


@pytest.mark.parametrize("layout,chunked", _layouts())
def test_dataframe_to_pandas_matches_native(layout, chunked):
    _arrow_pandas_strings()
    df = _frame(layout, chunked)
    _needs_capsule(df["s"]._native, "__arrow_c_array__")
    got = df.to_pandas()
    want = pd.DataFrame({name: _native_numpy(df[name]._native) for name in df.columns})
    assert (got.dtypes == want.dtypes).all()
    assert got.equals(want)


@pytest.mark.parametrize("layout,chunked", _layouts())
def test_to_pandas_nullable_strings(layout, chunked):
    s = _series(layout, chunked)
    got = s.to_pandas(nullable=True)
    assert got.tolist() == [pd.NA if v is None else v for v in VALUES]


def _selection():
    base = Series(pa.array(VALUES + ["tail"]))
    mask = Series([k not in (3, 5) for k in range(N + 1)])
    return base.filter(mask), [v for k, v in enumerate(VALUES + ["tail"]) if k not in (3, 5)]


@pytest.mark.parametrize("layout,chunked", _layouts())
def test_to_list_and_numpy_equal_flat(layout, chunked):
    s = _series(layout, chunked)
    flat = Series(pa.array(VALUES))
    assert s.to_list() == flat.to_list() == VALUES
    assert s.to_numpy().tolist() == flat.to_numpy().tolist() == VALUES
    assert s.to_numpy().dtype == object


def test_selection_to_list_and_numpy():
    s, expected = _selection()
    assert s.encoding == 3
    assert s.to_list() == expected
    assert s.to_numpy().tolist() == expected


def test_selection_over_dictionary():
    base = Series(pa.array(VALUES).dictionary_encode())
    s = base.filter(Series([k % 2 == 0 for k in range(N)]))
    expected = VALUES[::2]
    assert s.to_list() == expected
    assert s.to_numpy().tolist() == expected


def test_dictionary_entries_are_shared_objects():
    s = _series("dictionary", False)
    out = s.to_list()
    assert out[0] is out[3] is out[7]
    assert out[4] is out[8]


def test_dictionary_with_null_entry():
    arr = pa.DictionaryArray.from_arrays(
        pa.array([0, 1, 2, 1, None], pa.int32()), pa.array(["a", None, "b"])
    )
    s = Series(arr)
    assert s.to_list() == ["a", None, "b", None, None]
    assert s.to_numpy().tolist() == ["a", None, "b", None, None]


def test_empty_string_columns():
    s = Series(pa.array([], pa.string()))
    assert s.to_list() == []
    assert s.to_numpy().tolist() == []


def test_collected_view_frame_to_pandas_strings():
    _arrow_pandas_strings()
    values = ["a", None, "b", LONG] * 6
    arr = pa.array(values).cast(pa.string_view()).dictionary_encode()
    s = Series(arr)
    assert s.encoding == 2
    got = s.to_pandas()
    assert got.dtype == _pandas_native_strings(s).dtype
    assert got.isna().tolist() == [v is None for v in values]
    assert [v for v in got if isinstance(v, str)] == [v for v in values if v is not None]
