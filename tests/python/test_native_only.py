"""The engine ops and lazy plans on native columns, with no pyarrow use.

Runs in every build; in one built with DFTRACER_UTILS_ENABLE_ARROW=OFF, and no
pyarrow installed, it proves Series, DataFrame and LazyFrame work without Arrow.
"""

import numpy as np
import pytest

import dftracer.utils as dftu
from dftracer.utils import col
from dftracer.utils import dftracer_utils_ext as _ext

pytestmark = pytest.mark.skipif(
    not hasattr(_ext, "_dataframe_from_columns"), reason="extension predates native frames"
)


def _frame():
    return dftu.DataFrame.from_numpy(
        {
            "k": np.array([1, 1, 2, 2, 2], dtype=np.int64),
            "ts": np.array([10, 20, 5, 15, 25], dtype=np.int64),
            "v": np.array([1.0, 2.0, 3.0, 4.0, 5.0]),
        },
        [],
    )


def _col(df, name):
    return df[name].to_numpy().tolist()


def test_window_gap_fill_asof_interval_run_native():
    df = _frame()
    w = df.window(["k"], ["ts"], [("row_number", "rn"), ("running_sum", "v", "s")])
    assert _col(w, "rn") == [1, 2, 1, 2, 3]
    assert _col(w, "s") == [1.0, 3.0, 3.0, 7.0, 12.0]

    g = df.gap_fill(["k"], "ts", 10, ["v"], "locf")
    assert _col(g, "ts") == [10, 20, 5, 15, 25]

    right = dftu.DataFrame.from_numpy(
        {
            "k": np.array([1, 2], dtype=np.int64),
            "ts": np.array([12, 12], dtype=np.int64),
            "w": np.array([7.0, 8.0]),
        },
        [],
    )
    a = df.asof(right, "ts", by="k", direction="nearest")
    assert _col(a, "w") == [7.0, 7.0, 8.0, 8.0, 8.0]

    spans = dftu.DataFrame.from_numpy(
        {
            "k": np.array([2], dtype=np.int64),
            "lo": np.array([0], dtype=np.int64),
            "hi": np.array([16], dtype=np.int64),
        },
        [],
    )
    i = df.interval(spans, "ts", "lo", "hi", by="k")
    assert sorted(_col(i, "ts")) == [5, 15]


def test_lazy_plan_collects_native_columns():
    df = _frame()
    out = df.lazy().filter(col("ts") > 12).collect()
    assert _col(out, "ts") == [20, 15, 25]


def test_python_conversions_are_native():
    s = dftu.Series.from_list([1, None, 3])
    assert s.to_list() == [1, None, 3]
    assert s[0] == 1 and s[-1] == 3 and s[1] is None
    assert s[1:3].to_list() == [None, 3]
    assert s.to_numpy().tolist()[0] == 1.0 and np.isnan(s.to_numpy()[1])

    words = dftu.Series.from_list(["a", None, "ccc"])
    assert words.to_list() == ["a", None, "ccc"]
    assert words.to_numpy().tolist() == ["a", None, "ccc"]
    assert words[1:].to_list() == [None, "ccc"]

    flags = dftu.Series.from_list([True, False, True])
    assert flags.to_numpy().tolist() == [True, False, True]

    mixed = dftu.Series.from_list([1, 2.5])
    assert mixed.to_list() == [1.0, 2.5]

    frame = dftu.DataFrame.from_dict({"a": [1, 2, 3], "b": ["x", "y", "z"]})
    assert frame.to_dict() == {"a": [1, 2, 3], "b": ["x", "y", "z"]}
    assert frame["a"].to_numpy().tolist() == [1, 2, 3]


def test_pandas_and_polars_without_arrow():
    pd = pytest.importorskip("pandas")
    frame = dftu.DataFrame.from_dict({"a": [1, 2, 3], "b": ["x", "y", None]})
    out = frame.to_pandas()
    assert out["a"].tolist() == [1, 2, 3]
    assert out["b"].tolist()[:2] == ["x", "y"] and out["b"].isna().tolist() == [False, False, True]
    assert isinstance(out, pd.DataFrame)
    pl = pytest.importorskip("polars")
    out = frame.to_polars()
    assert out["a"].to_list() == [1, 2, 3] and isinstance(out, pl.DataFrame)
    assert out["b"].to_list() == ["x", "y", None]
