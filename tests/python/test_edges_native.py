# SPDX-License-Identifier: MIT
"""The pandas, NumPy and polars edges and the ops that use them run natively:
they work with the pyarrow import blocked, and where a conversion can share
memory it does."""

import subprocess
import sys
import textwrap

import pytest

np = pytest.importorskip("numpy")
pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402

BLOCKED = """
import sys
sys.modules["pyarrow"] = None
import datetime as dt
import numpy as np
import pandas as pd
from dftracer.utils import DataFrame, Series, TraceWriteOptions
from dftracer.utils.trace import TraceViewer
"""


def _run_blocked(body: str, tmp_path=None) -> None:
    code = BLOCKED + textwrap.dedent(body)
    if tmp_path is not None:
        code = code.replace("TMP", str(tmp_path))
    result = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_pandas_round_trips_with_pyarrow_blocked():
    _run_blocked(
        """
        df = pd.DataFrame(
            {
                "i": np.arange(4, dtype="int32"),
                "n": pd.array([1, None, 3, 4], dtype="Int64"),
                "b": pd.array([True, None, False, True], dtype="boolean"),
                "s": ["a", None, "c", "d"],
                "c": pd.Categorical(["x", "y", None, "x"], categories=["y", "x"]),
                "e": pd.Categorical([None, None, None, None], categories=[]),
            }
        )
        out = DataFrame.from_pandas(df)
        assert out["c"].to_list() == ["x", "y", None, "x"]
        assert out["e"].to_list() == [None] * 4
        back = out.to_pandas()
        assert list(back["c"].cat.categories) == ["y", "x"]
        assert back["n"].tolist()[0] == 1 and back["n"].isna().tolist() == [False, True, False, False]
        assert out["n"].to_pandas(nullable=True).dtype == "Int64"
        assert out["c"].to_pandas().cat.codes.tolist() == [1, 0, -1, 1]
        assert DataFrame.from_dict({"a": [1, 2], "b": np.array([1.5, 2.5])}).columns == ["a", "b"]
        assert Series.from_list([1, None], "float64").to_list() == [1.0, None]
        assert Series.from_list([np.int64(3), None]).to_list() == [3, None]
        """
    )


def test_loc_and_reindex_with_pyarrow_blocked():
    _run_blocked(
        """
        when = pd.to_datetime(["2020-01-01", "2020-01-02", "2020-01-04"])
        frame = DataFrame.from_pandas(pd.DataFrame({"v": [1, 2, 3]}, index=when))
        assert frame.loc[pd.Timestamp("2020-01-02")]["v"] == 2
        wanted = pd.to_datetime(["2020-01-02", "2020-01-03"])
        got = frame.reindex(wanted, method="ffill").to_pandas()
        assert got["v"].tolist() == [2, 2]
        """
    )


def test_a_table_writes_and_reads_with_pyarrow_blocked(tmp_path):
    _run_blocked(
        """
        df = pd.DataFrame(
            {
                "u8": np.arange(3, dtype="uint8"),
                "n": pd.array([1, None, 3], dtype="Int16"),
                "s": ["a", None, "c"],
                "c": pd.Categorical(["x", "y", None], categories=["y", "x"], ordered=True),
                "sets": [{1}, set(), None],
            },
            index=pd.MultiIndex.from_arrays([[5, 6, 7], ["p", "q", "p"]], names=["k", "l"]),
        )
        DataFrame.from_pandas(df).write_table(
            "TMP/t", name="t", mode="overwrite", options=TraceWriteOptions(build_index=False)
        )
        pd.testing.assert_frame_equal(TraceViewer("TMP/t").collect().to_pandas(), df)
        native = DataFrame.from_pandas(pd.DataFrame({"i": [1, 2], "s": ["a", "b"]}))
        native.write_table(
            "TMP/u", name="u", mode="overwrite", options=TraceWriteOptions(build_index=False)
        )
        assert TraceViewer("TMP/u").collect().to_pandas()["s"].tolist() == ["a", "b"]
        """,
        tmp_path,
    )


def test_to_numpy_shares_a_flat_column():
    values = np.arange(5, dtype=np.int64)
    col = Series.from_numpy(values)
    assert np.shares_memory(col.to_numpy(), values)


def test_a_default_pandas_column_is_one_writable_copy():
    col = Series.from_list([1, None, 3])
    got = col.to_pandas(nullable=True)
    assert not np.shares_memory(got.array._data, np.asarray(col._values_buffer()))
    got[0] = 7
    assert got.tolist()[0] == 7 and col.to_list()[0] == 1


def test_from_pandas_category_codes_stay_when_pandas_writes_its_own():
    s = pd.Series(pd.Categorical(["x", "y", "x"], categories=["x", "y"]))
    col = Series.from_pandas(s)
    s[0] = None  # pandas writes -1 into its codes in place
    assert col.to_list() == ["x", "y", "x"]


def test_arrow_exports_share_the_values():
    pa = pytest.importorskip("pyarrow")
    col = Series.from_numpy(np.arange(4, dtype=np.int64))
    mine = np.asarray(col._values_buffer())
    exported = pa.array(col)
    assert np.shares_memory(np.frombuffer(exported.buffers()[1], dtype=np.int64), mine)
    arrow_backed = col.to_pandas(arrow=True)
    chunk = arrow_backed.array.__arrow_array__().chunk(0)
    assert np.shares_memory(np.frombuffer(chunk.buffers()[1], dtype=np.int64), mine)
    frame = DataFrame({"x": col}).to_pandas(arrow=True)
    chunk = frame["x"].array.__arrow_array__().chunk(0)
    assert np.shares_memory(np.frombuffer(chunk.buffers()[1], dtype=np.int64), mine)


def test_from_polars_shares_the_values():
    pl = pytest.importorskip("polars")
    s = pl.Series("x", np.arange(6, dtype=np.int64))
    col = Series.from_polars(s)
    assert np.shares_memory(np.asarray(col._values_buffer()), s.to_numpy())
    frame = DataFrame.from_polars(pl.DataFrame({"x": s}))
    assert np.shares_memory(np.asarray(frame["x"]._values_buffer()), s.to_numpy())
