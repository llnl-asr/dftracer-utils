#!/usr/bin/env python3
"""Tests for the native analytical / relational DataFrame methods.

These primitives (``window``, ``gap_fill``, ``join``, ``asof``, ``interval``,
``unnest``, ``melt``, ``concat``, ``union``, ``distinct``, ``sample``,
``top_k``, ``sort``) run the SIMD kernels on the native columns and take/return
``DataFrame``s. Arrow is crossed only at the edge (``from_arrow``/``to_arrow``),
never inside a method - one test proves a method runs with pyarrow importing
blocked.
"""

import builtins
import sys

import pytest

pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402

# The analytical kernels only exist when the extension was built with Arrow.
_HAS_ARROW = hasattr(_ext._DataFrame, "window")
pytestmark = pytest.mark.skipif(not _HAS_ARROW, reason="extension built without Arrow support")


def _df(mapping):
    return DataFrame.from_arrow(pa.table(mapping))


def _dict(df):
    return df.to_arrow().to_pydict()


def _col(df, name):
    return df.to_arrow().column(name).to_pylist()


def test_dictionary_column_arrow_roundtrip():
    # A pyarrow dictionary column (int32 indices) must import, not raise
    # "unsupported Arrow column", and preserve its values on re-export.
    vals = ["a", "b", "a", "c", "b", "a"]
    t = pa.table({"s": pa.array(vals).dictionary_encode()})
    assert pa.types.is_dictionary(t.schema.field("s").type)
    df = DataFrame.from_arrow(t)
    assert _col(df, "s") == vals


def test_dictionary_encode_roundtrip_and_from_dict():
    # Series.dictionary_encode() -> to_arrow -> from_dict (a to_arrow/from_arrow
    # round-trip) must not raise and must keep values.
    from dftracer.utils import Series

    base = DataFrame.from_dict({"s": [f"k{v % 4}" for v in range(12)]})
    col = base["s"].dictionary_encode()
    assert col.encoding == 2  # Encoding::Dictionary
    rebuilt = DataFrame.from_dict({"s": col})
    assert _col(rebuilt, "s") == [f"k{v % 4}" for v in range(12)]
    back = Series.from_arrow(col.to_arrow())
    assert back.to_arrow().to_pylist() == [f"k{v % 4}" for v in range(12)]


def test_from_dict_borrowed_series_outlives_source_frame():
    # Series pulled from another frame, fed into from_dict, then the source
    # frame is dropped: the new frame must keep the data alive (was a
    # non-deterministic use-after-free / KeyError before the dict-import fix).
    import gc

    a = DataFrame.from_dict({"name": ["x", "y", "z", "x"], "v": [1, 2, 3, 4]})
    s_name = a["name"]
    s_v = a["v"]
    b = DataFrame.from_dict({"nm": s_name, "vv": s_v})
    del a, s_name, s_v
    gc.collect()
    assert _col(b, "nm") == ["x", "y", "z", "x"]
    assert _col(b, "vv") == [1, 2, 3, 4]


def test_group_by_dynamic_origin():
    # ts start at 5000; every=700 does not divide 5000 so alignment matters.
    df = _df({"ts": [5000 + 100 * i for i in range(20)], "v": list(range(20))})

    def first(**kw):
        out = _dict(df.group_by_dynamic("ts", 700, aggs=["count"], **kw))
        return min(out["ts"])

    assert first() == 4900  # floor(5000/700)*700, the classic grid
    assert first(origin=5000) == 5000  # explicit origin
    assert first(origin="min") == 5000  # aligned to the min timestamp


def test_to_ipc_roundtrips_via_arrow_reader():
    import io

    import pyarrow.ipc as ipc

    t = pa.table(
        {
            "a": pa.array([1, 2, 3], pa.int64()),
            "b": pa.array([1.5, 2.5, 3.5], pa.float64()),
            "c": pa.array(["x", "y", "z"]),
        }
    )
    raw = DataFrame.from_arrow(t).to_ipc()
    assert isinstance(raw, bytes) and len(raw) > 0
    back = ipc.open_stream(io.BytesIO(raw)).read_all()
    assert back.equals(t)


def test_window_row_number_running_sum_lag():
    df = _df({"pid": [1, 1, 1, 2], "ts": [10, 20, 30, 5], "dur": [1, 2, 3, 4]})
    out = df.window(
        partition_by=["pid"],
        order_by=["ts"],
        specs=[
            ("row_number", "rn"),
            ("running_sum", "dur", "cum"),
            ("lag", "dur", 1, "prev"),
        ],
    )
    assert isinstance(out, DataFrame)
    assert out.to_arrow().column_names == ["pid", "ts", "dur", "rn", "cum", "prev"]
    assert _col(out, "rn") == [1, 2, 3, 1]
    assert _col(out, "cum") == [1, 3, 6, 4]
    assert _col(out, "prev") == [None, 1, 2, None]


def test_window_frame_ntile_first_value():
    df = _df({"g": [1, 1, 1, 1], "ts": [1, 2, 3, 4], "x": [10, 20, 30, 40]})
    out = df.window(
        partition_by=["g"],
        order_by=["ts"],
        specs=[
            ("frame_sum", "x", 1, 1, "fs"),
            ("ntile", 2, "nt"),
            ("first_value", "x", "fv"),
        ],
    )
    assert _col(out, "fs") == [30, 60, 90, 70]
    assert _col(out, "nt") == [1, 1, 2, 2]
    assert _col(out, "fv") == [10, 10, 10, 10]


def test_window_fill_forward_and_frame_min_periods():
    df = _df({"g": [1, 1, 1, 2, 2], "ts": [1, 2, 3, 1, 2], "x": [None, 10, None, 5, None]})
    out = df.window(
        partition_by=["g"],
        order_by=["ts"],
        specs=[
            ("fill_forward", "x", "ff"),
            ("frame_sum", "x", 1, 0, "fs"),
            ("frame_sum", "x", 1, 0, "fs2", 2),
            ("frame_max", "x", 1, 0, "fm2", 2),
            ("frame_mean", "x", None, 0, "avg", 2),
        ],
    )
    assert _col(out, "ff") == [None, 10, 10, 5, 5]
    assert _col(out, "fs") == [None, 10, 10, 5, 5]
    # The frame holds fewer than 2 present values everywhere but nowhere more.
    assert _col(out, "fs2") == [None, None, None, None, None]
    assert _col(out, "fm2") == [None, None, None, None, None]
    assert _col(out, "avg") == [None, None, None, None, None]
    two = _df({"g": [1, 1, 1], "ts": [1, 2, 3], "x": [4, 6, None]}).window(
        partition_by=["g"],
        order_by=["ts"],
        specs=[("frame_mean", "x", 1, 0, "avg", 2), ("frame_min", "x", 1, 0, "lo", 2)],
    )
    assert _col(two, "avg") == [None, 5.0, None]
    assert _col(two, "lo") == [None, 4, None]


def test_window_rank_dense_rank_sessionize():
    df = _df({"g": [1, 1, 1, 1], "ts": [10, 10, 20, 100]})
    out = df.window(
        partition_by=["g"],
        order_by=["ts"],
        specs=[
            ("rank", "rk"),
            ("dense_rank", "dr"),
            ("sessionize", "ts", 15, "sess"),
        ],
    )
    assert _col(out, "rk") == [1, 1, 3, 4]
    assert _col(out, "dr") == [1, 1, 2, 3]
    assert _col(out, "sess") == [1, 1, 1, 2]


def test_gap_fill_modes():
    df = _df({"pid": [1, 1, 1], "ts": [0, 10, 30], "v": [100, 110, 130]})

    none = df.gap_fill(["pid"], "ts", 10, "v", "none")
    assert _col(none, "ts") == [0, 10, 20, 30]
    assert _col(none, "v") == [100, 110, None, 130]

    locf = df.gap_fill(["pid"], "ts", 10, "v", "locf")
    assert _col(locf, "v") == [100, 110, 110, 130]

    linear = df.gap_fill(["pid"], "ts", 10, "v", "linear")
    assert _col(linear, "v") == [100.0, 110.0, 120.0, 130.0]


def test_gap_fill_explicit_range():
    df = _df({"pid": [1, 1], "ts": [10, 20], "v": [1, 2]})
    out = df.gap_fill(["pid"], "ts", 10, "v", "locf", start=0, end=30)
    assert _col(out, "ts") == [0, 10, 20, 30]
    assert _col(out, "v") == [None, 1, 2, 2]


def test_join_inner_left_semi_anti():
    left = _df({"k": [1, 2, 3], "lv": [10, 20, 30]})
    right = _df({"k": [2, 3, 4], "rv": [200, 300, 400]})

    inner = left.join(right, "k", "inner")
    assert isinstance(inner, DataFrame)
    assert inner.to_arrow().column_names == ["k", "lv", "rv"]
    assert _dict(inner) == {"k": [2, 3], "lv": [20, 30], "rv": [200, 300]}

    lj = left.join(right, "k", "left")
    assert _dict(lj) == {"k": [1, 2, 3], "lv": [10, 20, 30], "rv": [None, 200, 300]}

    semi = left.join(right, "k", "semi")
    assert semi.to_arrow().column_names == ["k", "lv"]
    assert _dict(semi) == {"k": [2, 3], "lv": [20, 30]}

    anti = left.join(right, "k", "anti")
    assert semi.to_arrow().column_names == ["k", "lv"]
    assert _dict(anti) == {"k": [1], "lv": [10]}


def test_join_multi_key():
    left = _df({"a": [1, 1], "b": [1, 2], "lv": [10, 20]})
    right = _df({"a": [1], "b": [2], "rv": [99]})
    out = left.join(right, ["a", "b"], "inner")
    assert _dict(out) == {"a": [1], "b": [2], "lv": [20], "rv": [99]}


def test_join_int_on_uses_native_leading_keys():
    # An int `on` resolves to the first N shared column names, then joins on them.
    left = _df({"k": [1, 2, 3], "lv": [10, 20, 30]})
    right = _df({"k": [2, 3, 4], "rv": [200, 300, 400]})
    out = left.join(right, on=1, how="inner")
    assert _dict(out) == {"k": [2, 3], "lv": [20, 30], "rv": [200, 300]}


def test_join_right_outer_cross():
    left = _df({"k": [1, 2, 3], "lv": [10, 20, 30]})
    right = _df({"k": [2, 3, 4], "rv": [200, 300, 400]})

    rj = left.join(right, "k", "right")
    assert _dict(rj) == {"k": [2, 3, 4], "lv": [20, 30, None], "rv": [200, 300, 400]}

    outer = left.join(right, "k", "outer")
    assert _dict(outer) == {
        "k": [1, 2, 3, 4],
        "lv": [10, 20, 30, None],
        "rv": [None, 200, 300, 400],
    }
    assert _dict(left.join(right, "k", "full")) == _dict(outer)

    cross = left.join(right, how="cross")
    assert cross.to_arrow().column_names == ["k", "lv", "k_right", "rv"]
    assert cross.num_rows == 9
    assert _col(cross, "k")[:3] == [1, 1, 1]
    assert _col(cross, "k_right")[:3] == [2, 3, 4]
    with pytest.raises(ValueError):
        left.join(right, "k", "cross")


def test_join_left_on_right_on_suffix_and_merge():
    left = _df({"a": [1, 2, 2], "v": [10, 20, 21]})
    right = _df({"b": [2, 3], "v": [200, 300]})

    out = left.join(right, left_on="a", right_on="b")
    assert _dict(out) == {"a": [2, 2], "v": [20, 21], "b": [2, 2], "v_right": [200, 200]}

    sfx = left.join(right, left_on="a", right_on="b", suffix="_r")
    assert sfx.to_arrow().column_names == ["a", "v", "b", "v_r"]

    # merge is pandas: a colliding non-key column is suffixed on both sides.
    merged = left.merge(right, how="left", left_on="a", right_on="b")
    assert _dict(merged) == {
        "a": [1, 2, 2],
        "v_x": [10, 20, 21],
        "b": [None, 2, 2],
        "v_y": [None, 200, 200],
    }

    with pytest.raises(ValueError):
        left.join(right, on="a", left_on="a", right_on="b")
    with pytest.raises(ValueError):
        left.join(right, left_on="a")
    with pytest.raises(ValueError):
        left.join(right, left_on=["a"], right_on=["b", "v"])


def test_join_null_keys_never_match():
    left = _df({"k": [1, None, 2], "lv": [10, 11, 20]})
    right = _df({"k": [None, 2], "rv": [99, 200]})
    inner = left.join(right, "k")
    assert _dict(inner) == {"k": [2], "lv": [20], "rv": [200]}
    outer = left.join(right, "k", "outer")
    assert _dict(outer) == {
        "k": [1, None, 2, None],
        "lv": [10, 11, 20, None],
        "rv": [None, None, 200, 99],
    }


def test_join_string_keys_and_bool_values():
    left = _df({"k": ["a", "b", "c"], "flag": [True, False, True]})
    right = _df({"k": ["c", "a"], "n": [3, 1]})
    out = left.join(right, "k", "left")
    assert _dict(out) == {
        "k": ["a", "b", "c"],
        "flag": [True, False, True],
        "n": [1, None, 3],
    }


def test_lazy_join_matches_eager():
    left = _df({"k": [1, 2, 3, 2], "lv": [10, 20, 30, 21]})
    right = _df({"k": [2, 3, 4], "rv": [200, 300, 400]})
    for how in ("inner", "left", "right", "outer", "semi", "anti"):
        eager = left.join(right, "k", how)
        plan = left.lazy().join(right.lazy(), "k", how)
        assert plan.columns == eager.to_arrow().column_names
        assert _dict(plan.collect()) == _dict(eager)
        assert _dict(plan.collect(morsel_rows=1)) == _dict(eager)
    assert "join left [k] = [k]" in left.lazy().join(right.lazy(), "k", "left").explain()
    with pytest.raises(ValueError):
        left.lazy().join(right.lazy(), "nope").columns


def test_lazy_concat_and_union_match_eager():
    a = _df({"k": [1, 2, None], "s": ["x", "y", "z"]})
    b = _df({"k": [2, 4], "s": ["y", "w"]})
    eager = a.concat(b)
    plan = a.lazy().concat(b.lazy())
    assert plan.columns == ["k", "s"]
    assert "concat" in plan.explain()
    assert _dict(plan.collect()) == _dict(eager)
    assert _dict(plan.collect(morsel_rows=1)) == _dict(eager)
    assert _dict(a.lazy().union(b.lazy()).collect()) == _dict(a.union(b))
    assert _dict(a.lazy().concat(b.lazy(), a.lazy()).collect()) == _dict(a.concat(b, a))
    # A filter after the concat applies to both sides.
    from dftracer.utils.columnar import col

    filtered = plan.filter(col("k") > 1).collect()
    assert _dict(filtered) == {"k": [2, 2, 4], "s": ["y", "y", "w"]}
    with pytest.raises(ValueError, match="concat"):
        a.lazy().concat(b.lazy().select(["k"]))
    with pytest.raises(TypeError):
        a.lazy().concat(b)


def test_lazy_relational_ops_match_eager():
    ev = _df({"pid": [1, 1, 1, 2], "ts": [10, 20, 30, 5], "dur": [1, 2, 3, 4]})
    specs = [("row_number", "rn"), ("running_sum", "dur", "cum"), ("lag", "dur", 1, "prev")]
    lazy = ev.lazy().window(["pid"], ["ts"], specs)
    # The window states its names (input, then one per spec), so a later op
    # resolves them before collect.
    assert lazy.columns == ["pid", "ts", "dur", "rn", "cum", "prev"]
    assert "frame_op dftu.frame.window" in lazy.explain()
    assert _dict(lazy.collect()) == _dict(ev.window(["pid"], ["ts"], specs))
    assert _dict(lazy.collect(morsel_rows=1)) == _dict(ev.window(["pid"], ["ts"], specs))
    assert _dict(lazy.sort_by("cum", descending=True).select(["cum"]).collect()) == {
        "cum": [6, 4, 3, 1]
    }

    gf = _df({"pid": [1, 1, 1], "ts": [0, 10, 30], "v": [100, 110, 130]})
    for mode in ("none", "locf", "linear"):
        assert _dict(gf.lazy().gap_fill(["pid"], "ts", 10, "v", mode).collect()) == _dict(
            gf.gap_fill(["pid"], "ts", 10, "v", mode)
        )
    assert _dict(
        gf.lazy().gap_fill(["pid"], "ts", 10, "v", "locf", start=0, end=40).collect()
    ) == _dict(gf.gap_fill(["pid"], "ts", 10, "v", "locf", start=0, end=40))

    left = _df({"pid": [1, 1, 2], "ts": [10, 25, 10], "x": [1, 2, 3]})
    right = _df({"pid": [1, 1, 2], "ts": [5, 20, 50], "y": [50, 200, 500]})
    for direction in ("backward", "forward", "nearest"):
        assert _dict(left.lazy().asof(right.lazy(), "ts", ["pid"], direction).collect()) == _dict(
            left.asof(right, "ts", ["pid"], direction)
        )
    assert _dict(left.lazy().asof(right.lazy(), "ts", ["pid"], "backward", 3).collect()) == _dict(
        left.asof(right, "ts", ["pid"], "backward", 3)
    )
    with pytest.raises(ValueError, match="direction"):
        left.lazy().asof(right.lazy(), "ts", ["pid"], "sideways")
    # asof / interval state their names too: left, then the right's value
    # columns, a collision suffixed _right.
    assert left.lazy().asof(right.lazy(), "ts", ["pid"], "backward").columns == [
        "pid",
        "ts",
        "x",
        "y",
    ]
    assert left.lazy().asof(left.lazy(), "ts", ["pid"], "backward").columns == [
        "pid",
        "ts",
        "x",
        "x_right",
    ]

    pts = _df({"p": [5, 15, 99]})
    spans = _df({"lo": [0, 10], "hi": [10, 20], "tag": [1, 2]})
    for outer in (False, True):
        assert _dict(
            pts.lazy().interval(spans.lazy(), "p", "lo", "hi", None, outer).collect()
        ) == _dict(pts.interval(spans, "p", "lo", "hi", None, outer))

    base = _df({"k": [2, 1], "n": [10, 20]})
    variant = _df({"k": [1, 3], "n": [15, 7]})
    cmp_lazy = base.lazy().compare_agg(variant.lazy(), "k")
    assert "frame_op dftu.frame.compare_agg" in cmp_lazy.explain()
    assert _dict(cmp_lazy.collect()) == _dict(base.compare_agg(variant, "k"))
    with pytest.raises(ValueError):
        base.lazy().compare_agg(variant.lazy(), 0)

    tk_type = pa.list_(pa.struct([("value", pa.string()), ("count", pa.int64())]))
    nested = _df(
        {
            "pid": [1, 2],
            "tk": pa.array(
                [[{"value": "read", "count": 5}, {"value": "write", "count": 7}], []], tk_type
            ),
        }
    )
    un = nested.lazy().unnest("tk")
    assert un.columns == ["pid", "value", "count"]
    assert "unnest tk" in un.explain()
    assert _dict(un.collect()) == _dict(nested.unnest("tk"))
    assert _dict(nested.lazy().unnest("tk", keep_empty=True).collect(morsel_rows=1)) == _dict(
        nested.unnest("tk", keep_empty=True)
    )
    # The flattened field is addressable by name in a later filter.
    from dftracer.utils.columnar import col

    assert _dict(un.filter(col("count") > 6).collect()) == {
        "pid": [1],
        "value": ["write"],
        "count": [7],
    }
    with pytest.raises(ValueError):
        nested.lazy().unnest("pid")
    ops = set(_ext.op_list())
    assert {"dftu.lazy.unnest", "dftu.lazy.compare_agg"} <= ops


def test_concat_and_union_are_registry_ops():
    names = set(_ext.op_list())
    assert {"dftu.frame.concat", "dftu.frame.union", "dftu.lazy.concat"} <= names


def test_compare_agg_delta_pct():
    # FULL-join two aggregation results on the key, append delta_/pct_ per metric.
    # Group keys are string columns, as an aggregation always emits them.
    base = _df({"k": ["a", "b"], "count": [10, 20]})
    variant = _df({"k": ["a", "b"], "count": [15, 20]})
    out = base.compare_agg(variant, on="k")
    assert {"k", "l_count", "r_count", "delta_count", "pct_count"} <= set(
        out.to_arrow().column_names
    )
    d = _dict(out)
    assert d["l_count"] == [10, 20]
    assert d["r_count"] == [15, 20]
    assert d["delta_count"] == [5, 0]  # r - l
    assert d["pct_count"] == [50.0, 0.0]  # 100 * delta / l
    # an int `on` count is equivalent to naming the one leading key column.
    assert _dict(base.compare_agg(variant, on=1)) == d


def test_compare_agg_int_keys_and_unmatched_rows():
    base = _df({"k": [2, 1], "count": [10, 20], "tag": ["a", "b"]})
    variant = _df({"k": [1, 3], "count": [15, 7], "tag": ["b", "c"]})
    out = base.compare_agg(variant, on="k")
    assert out.to_arrow().column_names == [
        "k",
        "l_count",
        "l_tag",
        "r_count",
        "r_tag",
        "delta_count",
        "pct_count",
    ]
    d = _dict(out)
    assert d["k"] == [1, 2, 3]
    assert d["l_count"] == [20, 10, None]
    assert d["r_count"] == [15, None, 7]
    assert d["delta_count"] == [-5, None, None]
    assert d["pct_count"] == [-25.0, None, None]
    assert "dftu.frame.compare_agg" in set(_ext.op_list())
    with pytest.raises(ValueError, match="key column"):
        base.compare_agg(_df({"j": [1], "count": [1]}), on="k")


def test_asof_backward():
    left = _df({"ts": [10, 20, 30], "ev": ["a", "b", "c"]})
    right = _df({"ts": [5, 25], "val": [100, 200]})
    out = left.asof(right, "ts", direction="backward")
    assert out.to_arrow().column_names == ["ts", "ev", "val"]
    assert _col(out, "val") == [100, 100, 200]


def test_asof_by_and_tolerance():
    left = _df({"k": [1, 1, 2], "ts": [10, 20, 10], "ev": ["a", "b", "c"]})
    right = _df({"k": [1, 2], "ts": [5, 5], "val": [1, 2]})
    out = left.asof(right, "ts", by="k")
    assert _col(out, "val") == [1, 1, 2]

    tol = left.asof(right, "ts", by="k", tolerance=3)
    assert _col(tol, "val") == [None, None, None]


def test_interval_points_in_spans():
    pts = _df({"p": [5, 15, 25], "name": ["x", "y", "z"]})
    spans = _df({"lo": [0, 10], "hi": [9, 30], "span": ["A", "B"]})
    out = pts.interval(spans, "p", "lo", "hi")
    assert out.to_arrow().column_names == ["p", "name", "span"]
    assert _col(out, "span") == ["A", "B", "B"]


def test_interval_outer_keeps_unmatched():
    pts = _df({"p": [5, 100], "name": ["x", "y"]})
    spans = _df({"lo": [0], "hi": [9], "span": ["A"]})
    inner = pts.interval(spans, "p", "lo", "hi")
    assert _col(inner, "name") == ["x"]
    outer = pts.interval(spans, "p", "lo", "hi", outer=True)
    assert _col(outer, "name") == ["x", "y"]
    assert _col(outer, "span") == ["A", None]


def test_unnest_explodes_list_and_passes_second_list_through():
    df = _df(
        {
            "pid": [1, 2],
            "files": [["a", "b"], ["x"]],
            "tags": [["t1", "t2"], ["t3"]],
        }
    )
    out = df.unnest("files")
    assert out.to_arrow().column_names == ["pid", "files", "tags"]
    assert _col(out, "pid") == [1, 1, 2]
    assert _col(out, "files") == ["a", "b", "x"]
    assert _col(out, "tags") == [["t1", "t2"], ["t1", "t2"], ["t3"]]


def test_unnest_keep_empty():
    df = _df({"pid": [1, 2, 3], "files": [["a"], [], ["b"]]})
    assert _col(df.unnest("files"), "pid") == [1, 3]
    out = df.unnest("files", keep_empty=True)
    assert _col(out, "pid") == [1, 2, 3]
    assert _col(out, "files") == ["a", None, "b"]


def test_unnest_struct_list_flattens_fields():
    tk_type = pa.list_(pa.struct([("value", pa.string()), ("count", pa.int64())]))
    df = _df(
        {
            "pid": [1, 2],
            "tk": pa.array(
                [[{"value": "read", "count": 5}, {"value": "write", "count": 7}], []], tk_type
            ),
        }
    )
    out = df.unnest("tk")
    assert out.to_arrow().column_names == ["pid", "value", "count"]
    assert _col(out, "pid") == [1, 1]
    assert _col(out, "value") == ["read", "write"]
    assert _col(out, "count") == [5, 7]
    kept = df.unnest("tk", keep_empty=True)
    assert _col(kept, "pid") == [1, 1, 2]
    assert _col(kept, "count") == [5, 7, None]
    assert "dftu.frame.unnest" in set(_ext.op_list())


def test_melt_concat_union_distinct_sample_topk_sort():
    df = _df({"id": [1, 2], "a": [10, 30], "b": [20, 40]})
    melted = df.melt(["id"], ["a", "b"])
    assert set(melted.to_arrow().column_names) == {"id", "variable", "value"}
    assert melted.num_rows == 4

    a = _df({"k": [1, 2]})
    b = _df({"k": [2, 3]})
    assert sorted(_col(a.concat(b), "k")) == [1, 2, 2, 3]
    assert sorted(_col(a.union(b), "k")) == [1, 2, 3]

    # Diagonal concat unions columns; the absent one is null-filled.
    da = _df({"k": [1, 2], "n": [10, 20]})
    db = _df({"k": [3], "r": [1.5]})
    dg = da.concat(db, how="diagonal")
    d = _dict(dg)
    assert set(d) == {"k", "n", "r"}
    assert d["k"] == [1, 2, 3]
    assert d["n"] == [10, 20, None]  # absent in db
    assert d["r"] == [None, None, 1.5]  # absent in da
    with pytest.raises(ValueError):
        da.concat(db, how="sideways")

    dup = _df({"k": [1, 1, 2]})
    assert sorted(_col(dup.distinct(), "k")) == [1, 2]

    big = _df({"k": list(range(100))})
    assert big.sample(5, seed=1).num_rows == 5

    scores = _df({"name": ["a", "b", "c"], "v": [3, 1, 2]})
    assert _col(scores.top_k("v", 2), "v") == [3, 2]
    assert _col(scores.sort("v"), "v") == [1, 2, 3]
    assert _col(scores.sort("v", descending=True), "v") == [3, 2, 1]


def test_sort_values_per_column_ascending():
    # Regression: sort_by_multi(names, descending=[False, False]) used to sort
    # descending on every call, because a non-empty Python list is always
    # truthy and the old native arg parser coerced any object to bool.
    df = _df({"a": [2, 1, 2, 1], "b": [9, 8, 7, 6]})

    both_asc = df.sort_values(["a", "b"], ascending=[True, True])
    assert _col(both_asc, "a") == [1, 1, 2, 2]
    assert _col(both_asc, "b") == [6, 8, 7, 9]

    mixed = df.sort_values(["a", "b"], ascending=[True, False])
    assert _col(mixed, "a") == [1, 1, 2, 2]
    assert _col(mixed, "b") == [8, 6, 9, 7]
    assert _col(mixed, "b") != _col(both_asc, "b")

    # A scalar ascending still works, and inverts to the native `descending`.
    assert _col(df.sort_values("a", ascending=True), "a") == [1, 1, 2, 2]
    assert _col(df.sort_values("a", ascending=False), "a") == [2, 2, 1, 1]

    # The old bug: sort_by_multi's own list-of-bool `descending` must not
    # truthy-coerce either.
    fixed = df.sort_by_multi(["a", "b"], descending=[False, False])
    assert _col(fixed, "a") == [1, 1, 2, 2]
    assert _col(fixed, "b") == [6, 8, 7, 9]


def test_bad_column_name_raises_keyerror():
    df = _df({"pid": [1], "ts": [1], "dur": [1]})
    with pytest.raises(KeyError):
        df.window(["nope"], ["ts"], [("row_number", "rn")])
    with pytest.raises(KeyError):
        df.join(df, "missing")
    with pytest.raises(KeyError):
        df.unnest("nope")


def test_bad_how_mode_direction_raise_valueerror():
    df = _df({"pid": [1], "ts": [1], "dur": [1]})
    with pytest.raises(ValueError):
        df.join(df, "pid", "sideways")
    with pytest.raises(ValueError):
        df.gap_fill(["pid"], "ts", 10, "dur", "bogus")
    with pytest.raises(ValueError):
        df.asof(df, "ts", direction="sideways")


def test_bad_spec_raises_valueerror():
    df = _df({"pid": [1], "ts": [1], "dur": [1]})
    with pytest.raises(ValueError):
        df.window(["pid"], ["ts"], [("lag", "dur", "rn")])  # missing offset
    with pytest.raises(ValueError):
        df.window(["pid"], ["ts"], [("bogus", "rn")])  # unknown function
    with pytest.raises(ValueError):
        df.join(df, [], "inner")  # empty key list
    with pytest.raises(ValueError):
        df.gap_fill(["pid"], "ts", 10, "dur", "none", start=0)  # start w/o end


def test_series_analytical_ops_are_native():
    """Per-column analytical ops are native Series methods: Series in, Series
    out, and they run with pyarrow importing blocked."""
    from dftracer.utils import Series

    df = _df({"v": [30, 10, 20, 10]})
    col = df["v"]
    asc = Series.from_arrow(pa.array([10, 20, 30]))
    needles = Series.from_arrow(pa.array([15, 25]))

    real_import = builtins.__import__

    def blocked(name, *args, **kwargs):
        if name == "pyarrow" or name.startswith("pyarrow."):
            raise ImportError("pyarrow blocked for this test")
        return real_import(name, *args, **kwargs)

    saved = {k: v for k, v in sys.modules.items() if k == "pyarrow" or k.startswith("pyarrow.")}
    for k in saved:
        del sys.modules[k]
    builtins.__import__ = blocked
    try:
        srt = col.sort()
        assert isinstance(srt, Series)
        assert list(srt.to_numpy()) == [10, 10, 20, 30]

        order = col.argsort()
        assert isinstance(order, Series)
        assert list(order.to_numpy()) == [1, 3, 2, 0]

        rk = col.rank(method="dense")
        assert list(rk.to_numpy()) == [3.0, 1.0, 2.0, 1.0]

        roll = col.rolling(window=2, op="sum")
        # rolling leaves row 0 null (window-1), so to_numpy would need pyarrow;
        # verify the pyarrow-free op via reductions instead: values [_, 40, 30, 30].
        assert roll.null_count == 1
        assert roll.sum() == 100.0
        assert roll.max() == 40.0
        assert roll.min() == 30.0

        idx = asc.search_sorted(needles)
        assert list(idx.to_numpy()) == [1, 2]

        assert col.is_sorted() is False
        assert asc.is_sorted() is True
    finally:
        builtins.__import__ = real_import
        sys.modules.update(saved)


def test_methods_need_no_pyarrow():
    """The method path never imports pyarrow: build frames first, then block
    pyarrow and prove window/join/sort still run on the native columns."""
    left = _df({"pid": [1, 1, 2], "ts": [10, 20, 5], "dur": [1, 2, 4]})
    right = _df({"pid": [1, 2], "rv": [100, 200]})

    real_import = builtins.__import__

    def blocked(name, *args, **kwargs):
        if name == "pyarrow" or name.startswith("pyarrow."):
            raise ImportError("pyarrow blocked for this test")
        return real_import(name, *args, **kwargs)

    saved = {k: v for k, v in sys.modules.items() if k == "pyarrow" or k.startswith("pyarrow.")}
    for k in saved:
        del sys.modules[k]
    builtins.__import__ = blocked
    try:
        with pytest.raises(ImportError):
            __import__("pyarrow")

        win = left.window(["pid"], ["ts"], [("row_number", "rn"), ("running_sum", "dur", "cum")])
        assert win.column_names == ["pid", "ts", "dur", "rn", "cum"]
        # to_numpy on a flat non-null numeric column reads the native buffer
        # directly, no pyarrow.
        assert list(win["rn"].to_numpy()) == [1, 2, 1]
        assert list(win["cum"].to_numpy()) == [1, 3, 4]

        joined = left.join(right, "pid", "inner")
        assert joined.num_rows == 3

        srt = left.sort("ts")
        assert list(srt["ts"].to_numpy()) == [5, 10, 20]

        # a per-column primitive is equally pyarrow-free.
        assert left["dur"].is_sorted() is True  # dur == [1, 2, 4]
        assert left["ts"].is_sorted() is False  # ts == [10, 20, 5]
    finally:
        builtins.__import__ = real_import
        sys.modules.update(saved)


def test_sort_nulls_first_select_expr_and_with_columns():
    from dftracer.utils import col

    df = _df({"k": [1, None, 3, 2], "v": [1.0, 2.0, 3.0, 4.0]})
    assert _col(df.sort("k"), "k") == [1, 2, 3, None]
    assert _col(df.sort("k", nulls_last=False), "k") == [None, 1, 2, 3]
    assert _col(df.sort("k", descending=True, nulls_last=False), "v") == [2.0, 3.0, 4.0, 1.0]
    assert _col(df.sort_values("k", na_position="first"), "v") == [2.0, 1.0, 4.0, 3.0]
    assert df.sort("k", nulls_last=False).columns == ["k", "v"]

    # Several keys: nulls first per key, the pandas na_position="first" order.
    pd = pytest.importorskip("pandas")
    two = _df({"a": [1, None, 1, None, 2], "b": [None, 2, 1, None, 1]})
    for ascending in (True, [False, True], [True, False]):
        ours = two.sort_values(["a", "b"], ascending=ascending, na_position="first")
        ref = two.to_pandas().sort_values(["a", "b"], ascending=ascending, na_position="first")
        exp = [[None if pd.isna(x) else x for x in row] for row in ref.values.tolist()]
        assert [list(r) for r in zip(_col(ours, "a"), _col(ours, "b"))] == exp, ascending
        assert ours.columns == ["a", "b"]
        lazy = two.lazy().sort_values(["a", "b"], ascending=ascending, na_position="first")
        assert _dict(lazy.collect()) == _dict(ours)
    with pytest.raises(ValueError, match="one direction per key"):
        two.sort(["a", "b"], descending=[True, False, True], nulls_last=False)

    out = df.select("k", (col("v") * 2).alias("w"))
    assert out.columns == ["k", "w"]
    assert _col(out, "w") == [2.0, 4.0, 6.0, 8.0]
    with pytest.raises(TypeError, match="alias"):
        df.select(col("v") * 2)
    # Aggregates in select: an unaliased column aggregate keeps the column's
    # name (polars); a second output under one name is refused.
    agg = df.select(col("v").sum(), col("k").max(), col("v").mean().alias("m"))
    assert agg.columns == ["v", "k", "m"]
    assert _col(agg, "v") == [10.0]
    assert _col(agg, "k") == [3]
    assert _dict(df.lazy().select(col("v").sum()).collect()) == {"v": [10.0]}
    with pytest.raises(ValueError, match="two outputs named 'v'"):
        df.select(col("v").sum(), col("v").max())
    out = df.with_columns((col("v") + 1).alias("v1"), v2=col("v") * 2)
    assert out.columns == ["k", "v", "v1", "v2"]
    assert _col(out, "v1") == [2.0, 3.0, 4.0, 5.0]
    assert _col(out, "v2") == [2.0, 4.0, 6.0, 8.0]
