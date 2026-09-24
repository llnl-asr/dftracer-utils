#!/usr/bin/env python3
"""Tests for the LazyFrame deferred-query wrapper.

LazyFrame records ops and runs them on collect(), materializing a DataFrame.
The engine is the same SIMD columnar engine as the eager path, so a lazy
pipeline must match the eager result column-for-column. filter/with_column take
a columnar Expr, resolved by position against the frame's schema at that op.
"""

import pytest

pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame, LazyFrame, col, lazy  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402

_HAS_ARROW = hasattr(_ext, "_LazyFrame")
pytestmark = pytest.mark.skipif(not _HAS_ARROW, reason="extension built without Arrow support")


def _df(mapping):
    return DataFrame.from_arrow(pa.table(mapping))


def _dict(df):
    return df.to_arrow().to_pydict()


def test_lazy_entry_points():
    df = _df({"a": [1, 2, 3]})
    assert isinstance(df.lazy(), LazyFrame)
    assert isinstance(lazy(df), LazyFrame)


def test_collect_roundtrip_is_identity():
    df = _df({"a": [1, 2, 3], "b": [4.0, 5.0, 6.0]})
    out = df.lazy().collect()
    assert _dict(out) == {"a": [1, 2, 3], "b": [4.0, 5.0, 6.0]}


def test_filter_matches_eager():
    df = _df({"a": [1, 2, 3, 4], "b": [10, 20, 30, 40]})
    out = df.lazy().filter(col("a") > 2).collect()
    assert _dict(out) == {"a": [3, 4], "b": [30, 40]}


def test_with_column_and_select():
    df = _df({"a": [1, 2, 3], "b": [10, 20, 30]})
    out = df.lazy().with_column("c", col("a") + col("b")).select("a", "c").collect()
    assert _dict(out) == {"a": [1, 2, 3], "c": [11, 22, 33]}


def test_chained_pipeline_matches_eager():
    df = _df({"a": [5, 1, 4, 2, 3], "b": [1, 2, 3, 4, 5]})
    out = df.lazy().filter(col("a") >= 2).with_column("d", col("a") * 2).sort_by("a").collect()
    assert _dict(out) == {"a": [2, 3, 4, 5], "b": [4, 5, 3, 1], "d": [4, 6, 8, 10]}


def test_group_by_matches_eager():
    df = _df({"k": [1, 1, 2, 2, 2], "v": [10, 20, 30, 40, 50]})
    out = df.lazy().group_by("k", "count", "sum:v").sort_by("k").collect()
    d = _dict(out)
    assert d["k"] == [1, 2]
    assert d["count"] == [2, 3]
    assert d["sum_v"] == [30, 120]


def test_head_tail_slice():
    df = _df({"a": list(range(10))})
    assert _dict(df.lazy().head(3).collect()) == {"a": [0, 1, 2]}
    assert _dict(df.lazy().tail(2).collect()) == {"a": [8, 9]}
    assert _dict(df.lazy().slice(4, 3).collect()) == {"a": [4, 5, 6]}


def test_unique():
    df = _df({"a": [1, 1, 2, 3, 3, 3]})
    assert _dict(df.lazy().unique().collect()) == {"a": [1, 2, 3]}


def test_schema_and_explain():
    lf = _df({"a": [1], "b": [2]}).lazy().with_column("c", col("a") + col("b"))
    assert lf.columns == ["a", "b", "c"]
    assert isinstance(lf.explain(), str)
    assert lf.explain()


def test_filter_after_data_dependent_schema_raises():
    lf = _df({"a": [1, 2, 2]}).lazy().to_dummies("a")
    assert lf.columns == []  # data-dependent until collected
    with pytest.raises(ValueError):
        lf.filter(col("a") > 0)


def test_memory_budget_is_chainable():
    df = _df({"a": [3, 1, 2]})
    out = df.lazy().memory_budget(1 << 20).sort_by("a").collect()
    assert _dict(out) == {"a": [1, 2, 3]}


def test_take_filter_mask_reverse_sort_by_multi_match_eager():
    df = _df({"a": [3, 1, 2, 1], "b": [10, 20, 30, 40]})

    assert _dict(df.lazy().take([3, 0, 0]).collect()) == _dict(df.take([3, 0, 0]))
    assert _dict(df.lazy().take([3, 0, 0]).collect()) == {"a": [1, 3, 3], "b": [40, 10, 10]}

    mask = df["a"] > 1
    assert _dict(df.lazy().filter_mask(mask).collect()) == _dict(df.filter(mask))
    assert _dict(df.lazy().filter_mask(mask).collect()) == {"a": [3, 2], "b": [10, 30]}

    assert _dict(df.lazy().reverse().collect()) == {"a": [1, 2, 1, 3], "b": [40, 30, 20, 10]}
    assert _dict(df.lazy().reverse().collect()) == _dict(df.reverse())

    out = df.lazy().sort_by_multi(["a", "b"], descending=[False, True]).collect()
    assert _dict(out) == {"a": [1, 1, 2, 3], "b": [40, 20, 30, 10]}
    assert _dict(out) == _dict(df.sort_by_multi(["a", "b"], descending=[False, True]))
    assert "sort_by_multi [a, b]" in df.lazy().sort_by_multi(["a", "b"]).explain()


def test_string_expressions_in_a_lazy_plan():
    df = _df({"cat": ["io", "cpu", "IO", "Disk io"], "n": [1, 2, 3, 4]})
    out = df.lazy().filter(col("cat").contains("io")).collect()
    assert _dict(out) == {"cat": ["io", "Disk io"], "n": [1, 4]}
    out = df.lazy().filter(col("cat").is_in(["cpu", "IO"])).select("n").collect()
    assert _dict(out) == {"n": [2, 3]}
    out = (
        df.lazy()
        .with_column("u", col("cat").upper())
        .with_column("len", col("cat").len_bytes())
        .filter(col("len") > 2)
        .collect()
    )
    assert _dict(out) == {
        "cat": ["cpu", "Disk io"],
        "n": [2, 4],
        "u": ["CPU", "DISK IO"],
        "len": [3, 7],
    }
    out = df.lazy().filter(col("cat") == "io").collect()
    assert _dict(out) == {"cat": ["io"], "n": [1]}


def test_select_named_expr_with_columns_and_nulls_first_sort():
    df = _df({"k": [1, None, 3, 2], "v": [1.0, 2.0, 3.0, 4.0]})
    out = df.lazy().select("k", (col("v") * 2).alias("w")).collect()
    assert _dict(out) == {"k": [1, None, 3, 2], "w": [2.0, 4.0, 6.0, 8.0]}
    out = df.lazy().with_columns((col("v") + 1).alias("v1"), v2=col("v") * 2).collect()
    assert out.columns == ["k", "v", "v1", "v2"]
    assert _dict(out)["v1"] == [2.0, 3.0, 4.0, 5.0]
    assert _dict(out)["v2"] == [2.0, 4.0, 6.0, 8.0]
    with pytest.raises(TypeError, match="alias"):
        df.lazy().select(col("v") * 2)

    assert _dict(df.lazy().sort("k").collect())["k"] == [1, 2, 3, None]
    assert _dict(df.lazy().sort("k", nulls_last=False).collect())["k"] == [None, 1, 2, 3]
    assert _dict(df.lazy().sort("k", descending=True, nulls_last=False).collect())["k"] == [
        None,
        3,
        2,
        1,
    ]
    assert _dict(df.lazy().sort_values("k", na_position="first").collect())["v"] == [
        2.0,
        1.0,
        4.0,
        3.0,
    ]
    two = df.lazy().sort(["k", "v"], descending=[True, False], nulls_last=False).collect()
    assert _dict(two)["k"] == [None, 3, 2, 1]
    assert two.columns == ["k", "v"]


def test_is_null_expr_lazy_and_eager():
    df = _df({"k": [1, None, 3, 2], "v": [1.0, 2.0, 3.0, 4.0]})
    assert _dict(df.lazy().filter(col("k").is_null()).collect())["v"] == [2.0]
    assert _dict(df.lazy().filter(col("k").is_not_null()).collect())["v"] == [1.0, 3.0, 4.0]
    assert _dict(df.filter(col("k").isna()))["v"] == [2.0]
    assert _dict(df.filter(col("k").notna()))["v"] == [1.0, 3.0, 4.0]
    out = df.lazy().with_column("m", col("k").is_null()).collect()
    assert _dict(out)["m"] == [False, True, False, False]
