"""Parquet round trips with pandas and Dask: nullable dtypes come back, a Dask
directory reads without its index marker, and its parts concatenate."""

import os

import numpy as np
import pandas as pd
import pytest

pa = pytest.importorskip("pyarrow")
pq = pytest.importorskip("pyarrow.parquet")

from dftracer.utils import DataFrame  # noqa: E402


def nullable_frame():
    return pd.DataFrame(
        {
            "i": pd.array([1, None, 3], dtype="Int64"),
            "b": pd.array([True, None, False], dtype="boolean"),
            "s": pd.array(["a", None, "c"], dtype="string"),
            "f": [1.5, np.nan, 3.0],
            "n": [1, 2, 3],
        }
    )


def test_nullable_columns_keep_their_type(tmp_path):
    path = str(tmp_path / "u.parquet")
    DataFrame.from_pandas(nullable_frame()).to_parquet(path)
    got = pd.read_parquet(path)
    assert str(got["i"].dtype) == "Int64"
    assert str(got["b"].dtype) == "boolean"
    assert str(got["s"].dtype) == "string"
    assert got["i"].tolist()[0] == 1 and got["i"].isna().tolist() == [False, True, False]
    assert got["b"].isna().tolist() == [False, True, False]
    assert got["s"].isna().tolist() == [False, True, False]


def test_columns_without_nulls_stay_numpy_and_floats_stay_float(tmp_path):
    path = str(tmp_path / "u.parquet")
    DataFrame.from_pandas(nullable_frame()).to_parquet(path)
    got = pd.read_parquet(path)
    assert got["n"].dtype == np.int64
    assert got["f"].dtype == np.float64 and np.isnan(got["f"][1])
    frame = pd.DataFrame({"i": [1, 2, 3], "b": [True, False, True]})
    path2 = str(tmp_path / "v.parquet")
    DataFrame.from_pandas(frame).to_parquet(path2)
    back = pd.read_parquet(path2)
    assert back["i"].dtype == np.int64 and back["b"].dtype == np.bool_


@pytest.mark.parametrize(
    "width", ["Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32", "UInt64"]
)
def test_integer_widths(tmp_path, width):
    frame = pd.DataFrame({"x": pd.array([1, None, 3], dtype=width)})
    path = str(tmp_path / "w.parquet")
    DataFrame.from_pandas(frame).to_parquet(path)
    assert str(pd.read_parquet(path)["x"].dtype) == width


def test_write_parquet_does_the_same(tmp_path):
    path = str(tmp_path / "w.parquet")
    DataFrame.from_pandas(nullable_frame()).write_parquet(path)
    assert str(pd.read_parquet(path)["i"].dtype) == "Int64"


def test_round_trip_through_utils(tmp_path):
    first = str(tmp_path / "a.parquet")
    second = str(tmp_path / "b.parquet")
    nullable_frame().to_parquet(first)
    DataFrame.from_parquet(first).to_parquet(second)
    back = pd.read_parquet(second)
    want = pd.read_parquet(first)
    for name in ("i", "b", "s"):
        assert str(back[name].dtype) == str(want[name].dtype)
        assert back[name].isna().tolist() == want[name].isna().tolist()
    assert back["i"].dropna().tolist() == want["i"].dropna().tolist()


def test_an_empty_frame(tmp_path):
    path = str(tmp_path / "e.parquet")
    empty = DataFrame.from_pandas(nullable_frame().iloc[:0])
    empty.to_parquet(path)
    got = pd.read_parquet(path)
    assert len(got) == 0 and list(got.columns) == ["i", "b", "s", "f", "n"]


dd = pytest.importorskip("dask.dataframe")


def dask_dir(tmp_path, name, **kwargs):
    frame = pd.DataFrame({"s": list("abcd"), "v": [1, 2, 3, 4]})
    path = str(tmp_path / name)
    dd.from_pandas(frame, npartitions=2).to_parquet(path, **kwargs)
    return path, frame


def test_a_dask_directory_reads_without_its_index_marker(tmp_path):
    path, frame = dask_dir(tmp_path, "idx")
    got = DataFrame.from_parquet(path)
    assert got.columns == ["s", "v"]
    assert len(got) == 4
    assert sorted(got.to_pandas()["s"]) == list(frame["s"])


def test_the_marker_can_be_asked_for_by_name(tmp_path):
    path, _ = dask_dir(tmp_path, "idx2")
    got = DataFrame.from_parquet(path, columns=["s", "__null_dask_index__"])
    assert "__null_dask_index__" in got.columns


def test_a_named_index_stays_a_column(tmp_path):
    frame = pd.DataFrame({"v": [1, 2, 3]}, index=pd.Index([10, 20, 30], name="id"))
    path = str(tmp_path / "named.parquet")
    frame.to_parquet(path)
    assert DataFrame.from_parquet(path).columns == ["v", "id"] or set(
        DataFrame.from_parquet(path).columns
    ) == {"v", "id"}


def test_the_parts_read_and_concatenate(tmp_path):
    path, _ = dask_dir(tmp_path, "parts", write_index=False)
    parts = sorted(f for f in os.listdir(path) if f.endswith(".parquet"))
    assert pq.read_table(os.path.join(path, parts[0])).schema.field("s").type in (
        pa.large_string(),
        pa.string(),
    )
    frames = [DataFrame.from_parquet(os.path.join(path, p)) for p in parts]
    joined = frames[0].concat(frames[1])  # a large_string column: the repro of the proposal
    whole = DataFrame.from_parquet(path)
    assert (
        joined.to_pandas().reset_index(drop=True).equals(whole.to_pandas().reset_index(drop=True))
    )


def test_a_string_and_a_large_string_column_concatenate():
    small = DataFrame.from_arrow(pa.table({"s": pa.array(["a"], pa.string()), "v": [1]}))
    large = DataFrame.from_arrow(
        pa.table({"s": pa.array(["b", None], pa.large_string()), "v": [2, 3]})
    )
    got = small.concat(large).to_pandas()
    assert got["s"].tolist()[0:2] == ["a", "b"] and pd.isna(got["s"].tolist()[2])
    assert got["v"].tolist() == [1, 2, 3]
    assert large.concat(small).to_pandas()["s"].tolist()[0] == "b"


def test_a_string_next_to_an_integer_still_fails():
    strings = DataFrame.from_arrow(pa.table({"x": pa.array(["a"], pa.large_string())}))
    ints = DataFrame.from_arrow(pa.table({"x": pa.array([1], pa.int64())}))
    with pytest.raises(ValueError):
        strings.concat(ints)
