#!/usr/bin/env python3
"""Ingestion constructors on the Series / DataFrame wrappers: round-trip each
source (numpy, list, pandas, polars, parquet, dict) back to the same values.
Series.from_numpy on a numeric array takes the native pyarrow-free path."""

import pytest

np = pytest.importorskip("numpy")

from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402
from dftracer.utils.dataframe import DataFrame  # noqa: E402
from dftracer.utils.series import Series  # noqa: E402

pytestmark = pytest.mark.skipif(
    not hasattr(_ext, "_series_from_numpy"),
    reason="extension built without the vec column binding (needs Arrow)",
)


def test_series_from_numpy_native_no_pyarrow():
    # The native buffer-protocol path does not touch pyarrow; assert it is what
    # runs by calling the ext function directly, then check values round-trip.
    a = np.array([1, 2, 3, 4], dtype=np.int64)
    s = Series(_ext._series_from_numpy(a))
    assert list(s.to_numpy()) == [1, 2, 3, 4]


@pytest.mark.parametrize(
    "dtype",
    [np.int8, np.int16, np.int32, np.int64, np.uint32, np.float32, np.float64],
)
def test_series_from_numpy_roundtrip(dtype):
    a = np.array([1, 2, 3, 4, 5], dtype=dtype)
    out = Series.from_numpy(a).to_numpy()
    assert np.array_equal(np.asarray(out), a)
    assert np.asarray(out).dtype == a.dtype


def test_series_from_numpy_borrow_survives_source_del():
    a = np.arange(1000, dtype=np.float64) * 1.5
    s = Series.from_numpy(a)
    expected = a.copy()
    del a  # the column must keep the borrowed buffer alive
    assert np.array_equal(np.asarray(s.to_numpy()), expected)


def test_series_from_numpy_noncontiguous_falls_back():
    pytest.importorskip("pyarrow")
    a = np.arange(20, dtype=np.int64)[::2]  # strided, not C-contiguous
    out = Series.from_numpy(a).to_numpy()
    assert list(np.asarray(out)) == list(a)


def test_series_from_numpy_empty():
    a = np.array([], dtype=np.int64)
    out = Series.from_numpy(a).to_numpy()
    assert len(np.asarray(out)) == 0


def test_series_from_list():
    s = Series.from_list([10, 20, 30])
    assert list(s.to_numpy()) == [10, 20, 30]


def test_series_from_arrow():
    pa = pytest.importorskip("pyarrow")
    s = Series.from_arrow(pa.array([1.0, 2.0, 3.0]))
    assert list(s.to_numpy()) == [1.0, 2.0, 3.0]


def test_series_from_pandas():
    pytest.importorskip("pyarrow")
    pd = pytest.importorskip("pandas")
    s = Series.from_pandas(pd.Series([4, 5, 6], dtype="int64"))
    assert list(s.to_numpy()) == [4, 5, 6]


def test_series_from_polars():
    pl = pytest.importorskip("polars")
    s = Series.from_polars(pl.Series([7, 8, 9]))
    assert list(s.to_numpy()) == [7, 8, 9]


def test_dataframe_from_dict():
    df = DataFrame.from_dict({"a": [1, 2, 3], "b": [4.0, 5.0, 6.0]})
    assert set(df.columns) == {"a", "b"}
    assert df["a"].to_list() == [1, 2, 3]
    assert df["b"].to_list() == [4.0, 5.0, 6.0]


def test_dataframe_from_numpy_2d():
    arr = np.array([[1, 2], [3, 4], [5, 6]], dtype=np.int64)
    df = DataFrame.from_numpy(arr, ["x", "y"])
    assert df["x"].to_list() == [1, 3, 5]
    assert df["y"].to_list() == [2, 4, 6]


def test_dataframe_from_numpy_dict():
    df = DataFrame.from_numpy(
        {"p": np.array([1, 2], dtype=np.int64), "q": np.array([3.0, 4.0])}, None
    )
    assert df["p"].to_list() == [1, 2]
    assert df["q"].to_list() == [3.0, 4.0]


def test_dataframe_from_arrow():
    pa = pytest.importorskip("pyarrow")
    tbl = pa.table({"a": [1, 2], "b": [3, 4]})
    df = DataFrame.from_arrow(tbl)
    assert df["a"].to_list() == [1, 2]


def test_dataframe_from_pandas():
    pytest.importorskip("pyarrow")
    pd = pytest.importorskip("pandas")
    src = pd.DataFrame({"a": [1, 2, 3], "b": [4, 5, 6]})
    df = DataFrame.from_pandas(src)
    assert df["a"].to_list() == [1, 2, 3]
    assert df["b"].to_list() == [4, 5, 6]


def test_dataframe_from_polars():
    pl = pytest.importorskip("polars")
    src = pl.DataFrame({"a": [1, 2], "b": [3, 4]})
    df = DataFrame.from_polars(src)
    assert df["a"].to_list() == [1, 2]


def test_dataframe_from_parquet(tmp_path):
    pa = pytest.importorskip("pyarrow")
    pq = pytest.importorskip("pyarrow.parquet")
    path = str(tmp_path / "t.parquet")
    pq.write_table(pa.table({"a": [1, 2, 3], "b": [4.0, 5.0, 6.0]}), path)

    df = DataFrame.from_parquet(path)
    assert df["a"].to_list() == [1, 2, 3]
    assert df["b"].to_list() == [4.0, 5.0, 6.0]

    only_a = DataFrame.from_parquet(path, columns=["a"])
    assert only_a.columns == ["a"]
