#!/usr/bin/env python3
"""Vectorized column authoring: a column expression evaluates on the native vec
engine (our SIMD columnar format) and returns a native VecColumn. Arrow appears
only when the test asks for it, via VecColumn.to_arrow()."""

import pytest

from dftracer.utils import F, col, columnar, lit, where

pa = pytest.importorskip("pyarrow")
pytest.importorskip("numpy")

from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402
from dftracer.utils.dataframe import _dataframe_from_arrow  # noqa: E402
from dftracer.utils.series import _series_from_arrow  # noqa: E402

pytestmark = pytest.mark.skipif(
    not hasattr(_ext, "_series_from_arrow"),
    reason="extension built without the vec column binding (needs Arrow)",
)


def _table():
    return pa.table(
        {
            "dur": pa.array([1000, 2000, 3000, 4000], pa.int64()),
            "ts": pa.array([10.0, 20.0, 30.0, 40.0], pa.float64()),
            "cnt": pa.array([1, 2, 3, 4], pa.int64()),
        }
    )


def _list(vc):
    return vc.to_arrow().to_pylist()


def _type(vc):
    return vc.to_arrow().type


def test_columnar_unary_math_ops():
    t = pa.table(
        {
            "x": pa.array([-3.2, 4.7, -1.5, 9.9], pa.float64()),
            "n": pa.array([1, 2, 3, 4], pa.int64()),
        }
    )
    assert _list(F.x.floor().apply(t)) == [-4.0, 4.0, -2.0, 9.0]
    assert _list(F.x.ceil().apply(t)) == [-3.0, 5.0, -1.0, 10.0]
    assert _list(F.x.round().apply(t)) == [-3.0, 5.0, -2.0, 10.0]
    assert _list(F.x.abs().apply(t)) == [3.2, 4.7, 1.5, 9.9]
    assert _list(F.x.clip(-2.0, 5.0).apply(t)) == [-2.0, 4.7, -1.5, 5.0]
    # log widens to float; cast changes the column type.
    assert [round(v, 3) for v in _list(F.n.cast("float64").log().apply(t))] == [
        0.0,
        0.693,
        1.099,
        1.386,
    ]
    assert _list(F.n.cast("float64").apply(t)) == [1.0, 2.0, 3.0, 4.0]
    # The unary ops fuse into a single evaluated pass with arithmetic.
    assert _list((F.x.floor() + F.n).apply(t)) == [-3.0, 6.0, 1.0, 13.0]
    # trunc / sign / negate keep the input type.
    assert _list(F.x.trunc().apply(t)) == [-3.0, 4.0, -1.0, 9.0]
    assert _list(F.x.sign().apply(t)) == [-1.0, 1.0, -1.0, 1.0]
    assert _list(F.x.negate().apply(t)) == [3.2, -4.7, 1.5, -9.9]
    # sqrt / exp widen to float.
    p = pa.table({"p": pa.array([1.0, 4.0, 9.0, 16.0], pa.float64())})
    assert _list(F.p.sqrt().apply(p)) == [1.0, 2.0, 3.0, 4.0]
    assert [round(v, 3) for v in _list(F.p.cast("float64").exp().apply(p))][0] == round(
        2.718281828, 3
    )
    # is_between desugars to two compares -> Bool mask.
    assert _list(F.n.is_between(2, 3).apply(t)) == [False, True, True, False]


def test_columnar_is_nan_is_finite():
    inf = float("inf")
    t = pa.table({"x": pa.array([1.0, float("nan"), inf, 4.0], pa.float64())})
    assert _list(F.x.is_nan().apply(t)) == [False, True, False, False]
    assert _list(F.x.is_finite().apply(t)) == [True, False, False, True]
    assert _list(F.x.is_infinite().apply(t)) == [False, False, True, False]


def test_columnar_fillna_null_aware():
    # A nullable column reaches the engine and fillna closes the nulls.
    t = pa.table({"v": pa.array([5, None, 7, None], pa.int64())})
    assert _list(F.v.fillna(-7).apply(t)) == [5, -7, 7, -7]
    # A math op over the nullable column preserves the null positions.
    assert _list(F.v.abs().apply(t)) == [5, None, 7, None]
    # fillna after the op closes them.
    assert _list(F.v.abs().fillna(0).apply(t)) == [5, 0, 7, 0]


def test_columnar_integer_arithmetic():
    out = columnar(F.dur + F.cnt).apply(_table())
    assert _list(out) == [1001, 2002, 3003, 4004]
    assert _type(out) == pa.int64()


def test_columnar_eval_many_matches_individual():
    from dftracer.utils import eval_many

    t = _table()
    e1 = F.dur + F.cnt  # reads dur, cnt
    e2 = (F.dur + F.cnt) * lit(2)  # shares the dur+cnt subexpression (CSE)
    e3 = F.ts * lit(0.5)  # reads a different column
    outs = eval_many([e1, e2, e3], t)
    assert len(outs) == 3
    assert _list(outs[0]) == _list(columnar(e1).apply(t))
    assert _list(outs[1]) == _list(columnar(e2).apply(t))
    assert _list(outs[2]) == _list(columnar(e3).apply(t))
    assert _list(outs[0]) == [1001, 2002, 3003, 4004]
    assert _list(outs[1]) == [2002, 4004, 6006, 8008]


def test_columnar_subtract_columns():
    out = columnar(F.dur - F.cnt).apply(_table())
    assert _list(out) == [999, 1998, 2997, 3996]


def test_columnar_scalar_broadcast():
    out = columnar(F.dur * lit(2)).apply(_table())
    assert _list(out) == [2000, 4000, 6000, 8000]


def test_columnar_scalar_on_left_commutes():
    out = columnar(lit(3) * F.cnt).apply(_table())
    assert _list(out) == [3, 6, 9, 12]


def test_columnar_mixed_promotes_to_float():
    out = columnar(F.dur * lit(0.001) + F.ts).apply(_table())
    assert _list(out) == [11.0, 22.0, 33.0, 44.0]
    assert _type(out) == pa.float64()


def test_columnar_primitive_ilog2():
    out = columnar(F.dur.ilog2()).apply(_table())
    assert _list(out) == [(d).bit_length() - 1 for d in [1000, 2000, 3000, 4000]]
    assert _type(out) == pa.int64()


def test_columnar_primitive_popcount():
    out = columnar(F.cnt.popcount()).apply(_table())
    assert _list(out) == [bin(x).count("1") for x in [1, 2, 3, 4]]


def test_columnar_primitive_over_an_expression():
    # ilog2 of a derived column: cast + prim, still one vec pipeline.
    out = columnar((F.dur + F.cnt).ilog2()).apply(_table())
    assert _list(out) == [
        (d + c).bit_length() - 1 for d, c in zip([1000, 2000, 3000, 4000], [1, 2, 3, 4])
    ]


def test_columnar_col_and_F_are_equivalent():
    a = columnar(col("dur") + lit(1)).apply(_table())
    b = columnar(F.dur + lit(1)).apply(_table())
    assert _list(a) == _list(b)


def test_columnar_reports_input_columns():
    assert columnar(F.dur * lit(2) + F.ts).columns == ["dur", "ts"]


def test_columnar_division_columns():
    out = columnar(F.dur / F.cnt).apply(_table())
    assert _list(out) == [1000.0, 1000.0, 1000.0, 1000.0]
    assert _type(out) == pa.float64()


def test_columnar_division_by_scalar():
    out = columnar(F.dur / lit(2)).apply(_table())
    assert _list(out) == [500.0, 1000.0, 1500.0, 2000.0]
    assert _type(out) == pa.float64()


def test_dataframe_apply_matches_expr_apply():
    # df.apply(expr) is the frame-first spelling of expr.apply(df).
    df = _dataframe_from_arrow(_table())
    expr = F.dur / F.cnt
    out = df.apply(expr)
    assert _list(out) == [1000.0, 1000.0, 1000.0, 1000.0]
    assert _type(out) == pa.float64()
    assert _list(out) == _list(expr.apply(df))


def test_dataframe_apply_rejects_non_expr():
    df = _dataframe_from_arrow(_table())
    with pytest.raises(TypeError):
        df.apply("dur / cnt")


def test_columnar_comparison_makes_a_bool_mask():
    out = columnar(F.dur > lit(2000)).apply(_table())
    assert _list(out) == [False, False, True, True]
    assert _type(out) == pa.bool_()


def test_columnar_logical_and():
    out = columnar((F.dur > lit(1000)) & (F.cnt < lit(4))).apply(_table())
    assert _list(out) == [False, True, True, False]


def test_columnar_invert():
    out = columnar(~(F.dur > lit(2000))).apply(_table())
    assert _list(out) == [True, True, False, False]


def test_columnar_where_filters_arrow_table():
    result = where(_table(), F.dur >= lit(3000))
    assert result.column("dur").to_pylist() == [3000, 4000]
    assert result.column("cnt").to_pylist() == [3, 4]


def test_columnar_over_view_batch_stays_in_vec():
    # collect() returns a native VecBatch; the DSL operates on it directly and
    # Arrow appears only at the explicit .to_arrow() edge, never as an input.
    import dftracer.utils as dft
    from dftracer.utils import DataFrame, TraceViewer

    from .common import Environment

    with Environment(lines=100) as env:
        gz = env.create_test_gzip_file()
        with dft.Indexer(files=[gz]) as ix:
            ix.ensure_indexed()

        batch = (
            TraceViewer(gz).group_by("cat").agg("count", "sum:dur", "mean:dur").collect().collect()
        )
        assert isinstance(batch, DataFrame)
        assert set(["cat", "count", "sum_dur", "mean_dur"]).issubset(batch.keys())

        # A derived metric computed on the vec batch: sum/count == mean.
        avg = columnar(F.sum_dur / F.count).apply(batch)
        mean = batch["mean_dur"].to_arrow()
        for a, m in zip(avg.to_arrow().to_pylist(), mean.to_pylist()):
            assert abs(a - m) <= 1e-6 * max(1.0, abs(m))

        # where() on a VecBatch keeps everything in vec.
        hot = where(batch, F.count > lit(0))
        assert isinstance(hot, DataFrame)
        assert hot.to_arrow().num_rows == batch.num_rows


def test_columnar_batch_to_arrow_and_pandas_roundtrip():
    import dftracer.utils as dft
    from dftracer.utils import TraceViewer

    from .common import Environment

    with Environment(lines=100) as env:
        gz = env.create_test_gzip_file()
        with dft.Indexer(files=[gz]) as ix:
            ix.ensure_indexed()
        batch = TraceViewer(gz).group_by("cat").agg("count").collect().collect()
        tbl = batch.to_arrow()
        assert tbl.num_rows == batch.num_rows
        df = batch.to_pandas()
        assert list(df.columns) == list(batch.keys())
        assert len(df) == batch.num_rows


def test_columnar_batch_and_column_pickle_roundtrip():
    import pickle

    import dftracer.utils as dft
    from dftracer.utils import DataFrame, Series, TraceViewer

    from .common import Environment

    with Environment(lines=100) as env:
        gz = env.create_test_gzip_file()
        with dft.Indexer(files=[gz]) as ix:
            ix.ensure_indexed()
        batch = TraceViewer(gz).group_by("cat").agg("count", "sum:dur").collect().collect()

        b2 = pickle.loads(pickle.dumps(batch))
        assert isinstance(b2, DataFrame)
        assert b2.keys() == batch.keys()
        assert b2.num_rows == batch.num_rows
        assert b2.to_arrow().to_pydict() == batch.to_arrow().to_pydict()

        col = batch["count"]
        c2 = pickle.loads(pickle.dumps(col))
        assert isinstance(c2, Series)
        assert c2.to_arrow().to_pylist() == col.to_arrow().to_pylist()


def test_vecbatch_native_frame_ops():
    # select / rename / with_column / sort_by / head stay entirely in vec.
    import dftracer.utils as dft
    from dftracer.utils import DataFrame, TraceViewer

    from .common import Environment

    with Environment(lines=200) as env:
        gz = env.create_test_gzip_file()
        with dft.Indexer(files=[gz]) as ix:
            ix.ensure_indexed()
        batch = TraceViewer(gz).group_by("cat").agg("count", "sum:dur").collect().collect()

        proj = batch.select("cat", "count")
        assert isinstance(proj, DataFrame)
        assert proj.keys() == ["cat", "count"]

        rn = batch.rename({"count": "n"})
        assert "n" in rn.keys() and "count" not in rn.keys()
        assert rn.num_rows == batch.num_rows

        avg = columnar(F.sum_dur / F.count).apply(batch)
        wc = batch.with_column("avg_dur", avg)
        assert "avg_dur" in wc.keys()
        assert wc["avg_dur"].to_arrow().to_pylist() == avg.to_arrow().to_pylist()

        asc = batch.sort_by("count")
        desc = batch.sort_by("count", descending=True)
        acol = asc["count"].to_arrow().to_pylist()
        dcol = desc["count"].to_arrow().to_pylist()
        assert acol == sorted(acol)
        assert dcol == sorted(dcol, reverse=True)

        h = batch.head(1)
        assert h.num_rows == min(1, batch.num_rows)


def test_veccolumn_stats_and_elementwise():
    col = _series_from_arrow(pa.array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], pa.int64()))
    assert col.median() == pytest.approx(5.5)
    assert col.quantile(0.25) == pytest.approx(3.25)
    assert col.variance() == pytest.approx(9.16666, rel=1e-3)
    assert col.stddev() == pytest.approx(3.02765, rel=1e-3)

    dup = _series_from_arrow(pa.array([3, 1, 3, 2, 1], pa.int64()))
    assert dup.nunique() == 3
    assert dup.unique().to_arrow().to_pylist() == [1, 2, 3]

    signed = _series_from_arrow(pa.array([-3, 5, -7, 2], pa.int64()))
    assert signed.abs().to_arrow().to_pylist() == [3, 5, 7, 2]
    assert signed.clip(-2, 3).to_arrow().to_pylist() == [-2, 3, -2, 2]
    assert signed.cumsum().to_arrow().to_pylist() == [-3, 2, -5, -3]

    fl = _series_from_arrow(pa.array([1.4, 2.5, -1.6], pa.float64()))
    assert fl.round().to_arrow().to_pylist() == [1.0, 2.0, -2.0]


def test_series_numpy_ergonomics():
    import numpy as np

    a = _series_from_arrow(pa.array([1, 2, 3, 4], pa.int64()))
    b = _series_from_arrow(pa.array([10, 20, 30, 40], pa.int64()))

    # elementwise operators against another Series
    assert (a + b).to_arrow().to_pylist() == [11, 22, 33, 44]
    assert (b - a).to_arrow().to_pylist() == [9, 18, 27, 36]
    assert (a * b).to_arrow().to_pylist() == [10, 40, 90, 160]

    # scalar operators, forward and reflected
    assert (a + 10).to_arrow().to_pylist() == [11, 12, 13, 14]
    assert (10 + a).to_arrow().to_pylist() == [11, 12, 13, 14]
    assert (a * 3).to_arrow().to_pylist() == [3, 6, 9, 12]
    assert (3 * a).to_arrow().to_pylist() == [3, 6, 9, 12]
    assert (a - 1).to_arrow().to_pylist() == [0, 1, 2, 3]
    assert (10 - a).to_arrow().to_pylist() == [9, 8, 7, 6]
    assert (b / 10).to_arrow().to_pylist() == [1.0, 2.0, 3.0, 4.0]

    # indexing and slicing
    assert a[0] == 1
    assert a[-1] == 4
    assert a[1:3].to_arrow().to_pylist() == [2, 3]

    # numpy conversion + array protocol
    assert a.to_numpy().tolist() == [1, 2, 3, 4]
    assert np.asarray(a).tolist() == [1, 2, 3, 4]
    assert np.asarray(a, dtype="float64").dtype == np.float64


def test_veccolumn_rank_rolling_cum():
    col = _series_from_arrow(pa.array([3, 1, 2, 1], pa.int64()))
    assert col.rank(method="average").to_arrow().to_pylist() == [4.0, 1.5, 3.0, 1.5]
    assert col.rank(method="dense").to_arrow().to_pylist() == [3.0, 1.0, 2.0, 1.0]
    assert col.rank(method="min").to_arrow().to_pylist() == [4.0, 1.0, 3.0, 1.0]
    assert col.rank(method="max").to_arrow().to_pylist() == [4.0, 2.0, 3.0, 2.0]
    assert col.rank(method="first").to_arrow().to_pylist() == [4.0, 1.0, 3.0, 2.0]
    with pytest.raises(ValueError, match="average, min, max, dense or first"):
        col.rank(method="median")

    seq = _series_from_arrow(pa.array([1, 2, 3, 4, 5], pa.int64()))
    rs = seq.rolling(3, op="sum").to_arrow()
    assert rs.to_pylist() == [None, None, 6.0, 9.0, 12.0]
    assert seq.rolling(3, op="mean").to_arrow().to_pylist()[-1] == 4.0

    cv = _series_from_arrow(pa.array([3, 1, 4, 1, 5], pa.int64()))
    assert cv.cummax().to_arrow().to_pylist() == [3, 3, 4, 4, 5]
    assert cv.cummin().to_arrow().to_pylist() == [3, 1, 1, 1, 1]


def test_series_d1_rolling_var_std_median_quantile():
    seq = _series_from_arrow(pa.array([1, 2, 3, 4, 5], pa.int64()))
    # windows {1,2,3},{2,3,4},{3,4,5} each have sample variance 1.0.
    rv = seq.rolling_var(3).to_arrow().to_pylist()
    assert rv[:2] == [None, None]
    assert rv[2] == pytest.approx(1.0)
    assert rv[4] == pytest.approx(1.0)
    rs = seq.rolling_std(3).to_arrow().to_pylist()
    assert rs[2] == pytest.approx(1.0)
    rm = seq.rolling_median(3).to_arrow().to_pylist()
    assert rm[:2] == [None, None]
    assert rm[2] == pytest.approx(2.0)
    assert rm[4] == pytest.approx(4.0)
    rq = seq.rolling_quantile(3, 1.0).to_arrow().to_pylist()
    assert rq[2] == pytest.approx(3.0)  # window max


def test_series_d1_ewm():
    x = _series_from_arrow(pa.array([1.0, 2.0, 3.0, 4.0], pa.float64()))
    em = x.ewm_mean(0.5).to_arrow().to_pylist()
    # pandas adjust=True: the weighted average of every observation so far.
    assert em == pytest.approx([1.0, 5 / 3, 2.4285714285714284, 3.2666666666666666])
    es = x.ewm_std(0.5).to_arrow().to_pylist()
    assert es[0] is None  # sample std of one point is undefined
    assert es[1] == pytest.approx(0.5**0.5)


def test_series_d1_cut_qcut():
    v = _series_from_arrow(pa.array([5.0, 15.0, 25.0, 35.0], pa.float64()))
    breaks = _series_from_arrow(pa.array([10.0, 20.0, 30.0], pa.float64()))
    assert v.cut(breaks).to_arrow().to_pylist() == [0, 1, 2, 3]

    q = _series_from_arrow(pa.array([1, 2, 3, 4, 5, 6, 7, 8], pa.int64()))
    binned = q.qcut(4).to_arrow().to_pylist()
    assert binned[0] == 0
    assert binned[-1] == 3

    nullable = _series_from_arrow(pa.array([5.0, None, 35.0], pa.float64()))
    assert nullable.cut(breaks).to_arrow().to_pylist() == [0, None, 3]


def test_series_d1_search_sorted_interpolate():
    sorted_col = _series_from_arrow(pa.array([1, 3, 5, 7], pa.int64()))
    queries = _series_from_arrow(pa.array([0, 3, 4, 8], pa.int64()))
    assert sorted_col.search_sorted(queries).to_arrow().to_pylist() == [0, 1, 2, 4]

    gapped = _series_from_arrow(pa.array([None, 10.0, None, None, 40.0, None], pa.float64()))
    assert gapped.interpolate().to_arrow().to_pylist() == [
        None,
        10.0,
        20.0,
        30.0,
        40.0,
        None,
    ]


def test_series_d1_is_between_dot():
    v = _series_from_arrow(pa.array([1, 5, 10, 15, 20], pa.int64()))
    assert v.is_between(5, 15).to_arrow().to_pylist() == [
        False,
        True,
        True,
        True,
        False,
    ]

    a = _series_from_arrow(pa.array([1.0, 2.0, 3.0, 4.0], pa.float64()))
    b = _series_from_arrow(pa.array([10.0, 20.0, 30.0, 40.0], pa.float64()))
    assert a.dot(b) == pytest.approx(300.0)


def test_series_a1_reducers():
    col = _series_from_arrow(pa.array([2, 3, 4], pa.int64()))
    assert col.product() == 24

    w = _series_from_arrow(pa.array([5, 1, 9, 1, 9], pa.int64()))
    assert w.arg_min() == 1
    assert w.arg_max() == 2
    assert w.mode().to_list() == [1]

    mask_all = w > 0
    assert mask_all.all() is True
    assert mask_all.any() is True
    mask_some = w > 8
    assert mask_some.all() is False
    assert mask_some.any() is True


def test_series_a1_rounding_and_elementwise():
    fl = _series_from_arrow(pa.array([1.2, -1.7, 2.5, -2.5], pa.float64()))
    assert fl.ceil().to_arrow().to_pylist() == [2.0, -1.0, 3.0, -2.0]
    assert fl.floor().to_arrow().to_pylist() == [1.0, -2.0, 2.0, -3.0]
    assert fl.trunc().to_arrow().to_pylist() == [1.0, -1.0, 2.0, -2.0]

    s = _series_from_arrow(pa.array([-4, 0, 7], pa.int64()))
    assert s.sign().to_arrow().to_pylist() == [-1, 0, 1]
    assert s.negate().to_arrow().to_pylist() == [4, 0, -7]


def test_series_a1_diff_scans_and_math():
    v = _series_from_arrow(pa.array([10, 13, 9, 12], pa.int64()))
    d = v.diff().to_arrow().to_pylist()
    assert d[0] is None
    assert d[1:] == [3, -4, 3]

    pc = v.pct_change().to_arrow()
    assert pc.type == pa.float64()
    pcl = pc.to_pylist()
    assert pcl[0] is None
    assert pcl[1] == pytest.approx(0.3)

    p = _series_from_arrow(pa.array([1, 2, 3, 4], pa.int64()))
    assert p.cum_prod().to_arrow().to_pylist() == [1, 2, 6, 24]
    cc = p.cum_count().to_arrow()
    assert cc.type == pa.int64()
    assert cc.to_pylist() == [1, 2, 3, 4]

    q = _series_from_arrow(pa.array([4.0, 9.0, 16.0], pa.float64()))
    assert q.sqrt().to_arrow().to_pylist() == [2.0, 3.0, 4.0]
    roundtrip = q.exp().log().to_arrow().to_pylist()
    for a, b in zip(roundtrip, [4.0, 9.0, 16.0]):
        assert a == pytest.approx(b)


def test_series_a2_ieee_predicates():
    f = _series_from_arrow(
        pa.array([1.0, float("nan"), float("inf"), float("-inf"), 2.5], pa.float64())
    )
    assert f.is_nan().to_arrow().to_pylist() == [False, True, False, False, False]
    assert f.is_finite().to_arrow().to_pylist() == [True, False, False, False, True]
    assert f.is_infinite().to_arrow().to_pylist() == [False, False, True, True, False]

    i = _series_from_arrow(pa.array([1, 2, 3], pa.int64()))
    assert i.is_finite().to_arrow().to_pylist() == [True, True, True]
    assert i.is_nan().to_arrow().to_pylist() == [False, False, False]


def test_series_a2_unique_duplicated_sorted():
    c = _series_from_arrow(pa.array([3, 1, 3, 2, 1], pa.int64()))
    assert c.unique_mask().to_arrow().to_pylist() == [False, False, False, True, False]
    assert c.is_unique is False
    assert _series_from_arrow(pa.array([1, 2, 3], pa.int64())).is_unique is True
    assert c.is_duplicated().to_arrow().to_pylist() == [True, True, True, False, True]

    asc = _series_from_arrow(pa.array([1, 2, 2, 3], pa.int64()))
    assert asc.is_sorted() is True
    assert asc.is_sorted(descending=True) is False
    desc = _series_from_arrow(pa.array([5, 4, 4, 1], pa.int64()))
    assert desc.is_sorted(descending=True) is True


def test_series_a2_selection_and_reshape():
    c = _series_from_arrow(pa.array([3, 1, 2], pa.int64()))
    assert c.sort().to_arrow().to_pylist() == [1, 2, 3]
    assert c.sort(descending=True).to_arrow().to_pylist() == [3, 2, 1]
    assert c.head(2).to_arrow().to_pylist() == [3, 1]
    assert c.tail(2).to_arrow().to_pylist() == [1, 2]
    assert c.head(10).to_arrow().to_pylist() == [3, 1, 2]  # clamp
    assert c.reverse().to_arrow().to_pylist() == [2, 1, 3]

    nullable = _series_from_arrow(pa.array([9, None, 7, None], pa.int64()))
    assert nullable.drop_nulls().to_arrow().to_pylist() == [9, 7]

    v = _series_from_arrow(pa.array([1, 2, 3, 4, 5], pa.int64()))
    vals = _series_from_arrow(pa.array([2, 4, 6], pa.int64()))
    assert v.is_in(vals).to_arrow().to_pylist() == [False, True, False, True, False]


def test_series_a2_shift_topk_sample():
    v = _series_from_arrow(pa.array([10, 20, 30, 40], pa.int64()))
    assert v.shift(1).to_arrow().to_pylist() == [None, 10, 20, 30]
    assert v.shift(-1).to_arrow().to_pylist() == [20, 30, 40, None]

    d = _series_from_arrow(pa.array([50, 10, 90, 30, 70], pa.int64()))
    assert d.top_k(2).to_arrow().to_pylist() == [90, 70]
    assert d.bottom_k(2).to_arrow().to_pylist() == [10, 30]

    seq = _series_from_arrow(pa.array(list(range(10)), pa.int64()))
    s1 = seq.sample(4, seed=42).to_arrow().to_pylist()
    s2 = seq.sample(4, seed=42).to_arrow().to_pylist()
    assert s1 == s2  # deterministic
    assert len(s1) == 4
    assert s1 == sorted(s1)  # ascending row order preserved
    assert seq.sample(100, seed=1).to_arrow().to_pylist() == list(range(10))  # clamp


def test_vecbatch_topk_and_concat():
    import dftracer.utils as dft
    from dftracer.utils import DataFrame, TraceViewer

    from .common import Environment

    with Environment(lines=200) as env:
        gz = env.create_test_gzip_file()
        with dft.Indexer(files=[gz]) as ix:
            ix.ensure_indexed()
        batch = TraceViewer(gz).group_by("cat").agg("count", "sum:dur").collect().collect()

        top1 = batch.topk("count", 1, largest=True)
        assert isinstance(top1, DataFrame)
        assert top1.num_rows == min(1, batch.num_rows)
        counts = pa.array(batch["count"]).to_pylist()
        assert pa.array(top1["count"]).to_pylist()[0] == max(counts)

        # concat a batch with itself doubles the rows.
        doubled = batch.concat(batch)
        assert doubled.num_rows == 2 * batch.num_rows

        # re-aggregate the doubled batch back by cat: summing the partial counts
        # recovers 2x the original per-cat count (the partial-merge pattern).
        regrouped = doubled.group_by("cat", "sum:count", "sum:sum_dur")
        assert set(regrouped.keys()) == {"cat", "sum_count", "sum_sum_dur"}
        assert regrouped.num_rows == batch.num_rows
        base = dict(zip(pa.array(batch["cat"]).to_pylist(), pa.array(batch["count"]).to_pylist()))
        got = dict(
            zip(
                pa.array(regrouped["cat"]).to_pylist(),
                pa.array(regrouped["sum_count"]).to_pylist(),
            )
        )
        for k, c in base.items():
            assert got[k] == 2 * c


def test_vecbatch_hash_partition_shuffle():
    # The distributed pattern: hash-partition by key, then group each part
    # locally - equal keys co-locate, so per-part group_by + concat == direct.
    tbl = pa.table(
        {
            "cat": ["io", "cpu", "io", "net", "cpu", "io", "net"],
            "n": pa.array([1, 2, 3, 4, 5, 6, 7], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    parts = batch.hash_partition(["cat"], 4)
    assert len(parts) == 4
    assert sum(p.num_rows for p in parts) == batch.num_rows

    # partition_id is the per-row part; hash_partition is its fold.
    ids = batch.partition_id("cat", 4).to_list()
    assert len(ids) == batch.num_rows
    assert ids[0] == ids[2] == ids[5]
    assert ids[1] == ids[4]
    assert ids[3] == ids[6]
    for i, p in enumerate(parts):
        assert p.num_rows == ids.count(i)
    with pytest.raises(KeyError):
        batch.partition_id("nope", 4)
    with pytest.raises(ValueError):
        batch.partition_id("cat", 0)

    # each cat lands in exactly one part
    homes = {}
    for i, p in enumerate(parts):
        for c in pa.array(p["cat"]).to_pylist():
            assert homes.setdefault(c, i) == i

    # per-part sum then merge equals the direct grouped sum
    grouped = [p.group_by("cat", "sum:n") for p in parts if p.num_rows > 0]
    merged = grouped[0].concat(*grouped[1:]) if len(grouped) > 1 else grouped[0]
    got = dict(
        zip(
            pa.array(merged["cat"]).to_pylist(),
            pa.array(merged["sum_n"]).to_pylist(),
        )
    )
    assert got == {"io": 1 + 3 + 6, "cpu": 2 + 5, "net": 4 + 7}


def test_vecbatch_join_on_key():
    left = _dataframe_from_arrow(
        pa.table({"k": ["a", "b", "c"], "l": pa.array([1, 2, 3], pa.int64())})
    )
    right = _dataframe_from_arrow(
        pa.table({"k": ["b", "c", "d"], "r": pa.array([20, 30, 40], pa.int64())})
    )
    inner = left.join(right, how="inner", on=1).to_arrow()
    rows = {row["k"]: row for row in inner.to_pylist()}
    assert set(rows) == {"b", "c"}
    assert rows["b"]["l"] == 2 and rows["b"]["r"] == 20

    outer = left.join(right, how="left", on=1)
    assert outer.num_rows == 3  # a, b, c


def test_columnar_fused_parallel_large_input():
    # > the evaluator's GRAIN (1<<16), so this exercises the chunked parallel
    # path (multiple chunks + concat) and must match the elementwise result.
    import numpy as np

    n = 200_000
    a = np.arange(n, dtype=np.int64)
    b = (np.arange(n, dtype=np.int64) * 3) % 7
    tbl = pa.table({"a": pa.array(a), "b": pa.array(b)})

    out = columnar((F.a + F.b) * lit(2)).apply(tbl).to_arrow()
    assert out.to_pylist() == ((a + b) * 2).tolist()

    fout = columnar(F.a / lit(2) + F.b).apply(tbl).to_arrow()
    assert np.allclose(fout.to_numpy(), a / 2 + b)

    mask = columnar(F.a > lit(100000)).apply(tbl).to_arrow()
    assert mask.to_pylist() == (a > 100000).tolist()


def test_vecbatch_group_by_moment_aggs():
    # var/std/skew/kurt on the fused engine must match pandas groupby, which
    # uses the same sample-variance / population-skew-kurt conventions.
    pd = pytest.importorskip("pandas")
    import numpy as np

    rng = np.random.default_rng(7)
    cats = rng.integers(0, 3, size=5000)
    dur = rng.exponential(scale=100.0, size=5000)
    tbl = pa.table({"cat": pa.array(cats), "dur": pa.array(dur)})
    batch = _dataframe_from_arrow(tbl)

    got = batch.group_by("cat", "var:dur", "std:dur", "skew:dur", "kurt:dur").to_pandas()
    got = got.set_index("cat").sort_index()

    ref = pd.DataFrame({"cat": cats, "dur": dur}).groupby("cat")["dur"]
    exp = pd.DataFrame({"var_dur": ref.var(), "std_dur": ref.std()}).sort_index()

    assert np.allclose(got["var_dur"], exp["var_dur"], rtol=1e-6)
    assert np.allclose(got["std_dur"], exp["std_dur"], rtol=1e-6)
    # Our skew/kurt are the population (biased) moments; compare against the
    # population moment directly rather than pandas' bias-corrected version.
    for c in sorted(set(cats)):
        s = dur[cats == c]
        m = s - s.mean()
        var_p = (m**2).mean()
        skew_p = (m**3).mean() / var_p**1.5
        kurt_p = (m**4).mean() / var_p**2 - 3.0
        row = got.loc[c]
        assert abs(row["skew_dur"] - skew_p) < 1e-6
        assert abs(row["kurt_dur"] - kurt_p) < 1e-6


def test_group_by_first_last():
    # first/last take the group's first / last value in row order; the merge is
    # order-independent (min/max global row index), so the parallel path is
    # exact. Cover both the string-spec form and the F-expression form.
    from dftracer.utils.columnar import F

    tbl = pa.table(
        {
            "cat": ["x", "y", "x", "y", "x"],
            "v": pa.array([10, 5, 20, 7, 30], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)

    got = batch.group_by("cat", "first:v", "last:v").to_pandas().set_index("cat")
    assert got.loc["x", "first_v"] == 10
    assert got.loc["x", "last_v"] == 30
    assert got.loc["y", "first_v"] == 5
    assert got.loc["y", "last_v"] == 7

    # F-expression form via the two-step .agg().
    out = (
        batch.group_by("cat")
        .agg(F.v.first().alias("f"), F.v.last().alias("l"))
        .to_pandas()
        .set_index("cat")
    )
    assert out.loc["x", "f"] == 10
    assert out.loc["x", "l"] == 30


def test_group_by_quantile_percentile():
    # Per-group DDSketch quantiles: F.x.quantile(q) and F.x.percentile(p) land
    # within the sketch's relative accuracy of the true value. Both the string
    # spec and the F-expression forms are covered.
    import numpy as np

    from dftracer.utils.columnar import F

    n = 5000
    cats = np.array([0, 1] * (n // 2))
    dur = np.where(cats == 0, np.arange(n) + 1.0, 10.0 * (np.arange(n) + 1.0))
    tbl = pa.table({"cat": pa.array(cats), "dur": pa.array(dur)})
    batch = _dataframe_from_arrow(tbl)

    out = (
        batch.group_by("cat")
        .agg(F.dur.quantile(0.5).alias("p50"), F.dur.percentile(90).alias("p90"))
        .to_pandas()
        .set_index("cat")
        .sort_index()
    )
    for c in (0, 1):
        s = np.sort(dur[cats == c])
        true_p50 = s[int(0.5 * len(s))]
        true_p90 = s[int(0.9 * len(s))]
        assert out.loc[c, "p50"] == pytest.approx(true_p50, rel=0.03)
        assert out.loc[c, "p90"] == pytest.approx(true_p90, rel=0.03)

    # percentile(p) == quantile(p/100).
    a = batch.group_by("cat").agg(F.dur.percentile(99).alias("x")).to_pandas()
    b = batch.group_by("cat").agg(F.dur.quantile(0.99).alias("x")).to_pandas()
    assert a["x"].to_list() == pytest.approx(b["x"].to_list())


def test_group_by_hist():
    # F.x.hist() emits a per-group DDSketch histogram as a list<struct> column;
    # each group's bin counts sum to its row count.
    from dftracer.utils.columnar import F

    tbl = pa.table(
        {
            "cat": ["x"] * 300 + ["y"] * 200,
            "dur": pa.array(
                [float(1 + i % 50) for i in range(300)]
                + [float(1000 + i % 50) for i in range(200)],
                pa.float64(),
            ),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    out = batch.group_by("cat").agg(F.dur.hist().alias("h")).to_arrow()
    rows = {r["cat"]: r["h"] for r in out.to_pylist()}
    assert set(rows) == {"x", "y"}
    for cat, bins in rows.items():
        assert len(bins) > 0
        assert set(bins[0].keys()) == {"lo", "hi", "count"}
        assert sum(b["count"] for b in bins) == (300 if cat == "x" else 200)


def test_group_by_sumsq():
    # sumsq is a straight finalize of FieldStat::sumsq: sum(x**2) per group.
    from dftracer.utils.columnar import F

    tbl = pa.table(
        {
            "cat": ["x", "y", "x", "y", "x"],
            "v": pa.array([10, 5, 20, 7, 30], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    out = batch.group_by("cat").agg(F.v.sumsq().alias("ssq")).to_pandas()
    out = out.set_index("cat")
    assert out.loc["x", "ssq"] == pytest.approx(10.0**2 + 20.0**2 + 30.0**2)
    assert out.loc["y", "ssq"] == pytest.approx(5.0**2 + 7.0**2)

    # String-spec form via the legacy "sumsq:col" surface.
    legacy = batch.group_by("cat", "sumsq:v").to_pandas().set_index("cat")
    assert legacy.loc["x", "sumsq_v"] == pytest.approx(out.loc["x", "ssq"])


def test_group_by_argmax():
    # argmax(value, by) is the String repr of `value` at the row maximizing
    # `by`, matching pandas' idxmax-then-lookup.
    pd = pytest.importorskip("pandas")
    from dftracer.utils.columnar import F

    tbl = pa.table(
        {
            "cat": ["x", "x", "x", "y", "y"],
            "name": ["a", "b", "c", "p", "q"],
            "dur": pa.array([10, 30, 20, 5, 8], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    out = batch.group_by("cat").agg(F.name.argmax(F.dur).alias("am")).to_pandas()
    out = out.set_index("cat")

    df = pd.DataFrame(
        {
            "cat": ["x", "x", "x", "y", "y"],
            "name": ["a", "b", "c", "p", "q"],
            "dur": [10, 30, 20, 5, 8],
        }
    )
    exp = df.loc[df.groupby("cat")["dur"].idxmax()].set_index("cat")["name"]
    assert out.loc["x", "am"] == exp.loc["x"]
    assert out.loc["y", "am"] == exp.loc["y"]


def test_group_by_set_union():
    # set_union is the sorted, distinct String values of a field, joined by the
    # engine's separator (matching a sorted-unique-join reference).
    from dftracer.utils.columnar import F

    tbl = pa.table(
        {
            "cat": ["x", "x", "x", "y"],
            "tag": ["posix", "stdio", "posix", "mpi"],
        }
    )
    batch = _dataframe_from_arrow(tbl)
    out = batch.group_by("cat").agg(F.tag.set_union().alias("tags")).to_pandas()
    out = out.set_index("cat")
    sep = "\x1e"
    assert out.loc["x", "tags"] == sep.join(sorted({"posix", "stdio"}))
    assert out.loc["y", "tags"] == "mpi"


def test_group_by_agg_expressions():
    # Aggregate over expressions (Polars-style), both the two-step .agg() and the
    # one-shot form, plus legacy strings - all through the CSE group_agg_expr.
    from dftracer.utils.columnar import F, count

    tbl = pa.table(
        {
            "cat": ["x", "y", "x", "y", "x"],
            "a": pa.array([1, 2, 3, 4, 5], pa.int64()),
            "b": pa.array([10, 20, 30, 40, 50], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)

    # Two-step: sum(a+b) and mean(a+b) share `a+b` (computed once via CSE).
    two = batch.group_by("cat").agg(
        (F.a + F.b).sum().alias("sum_ab"),
        (F.a + F.b).mean().alias("mean_ab"),
        count().alias("n"),
    )
    d = dict(zip(pa.array(two["cat"]).to_pylist(), range(two.num_rows)))
    got = two.to_arrow().to_pydict()
    xi = d["x"]
    assert got["sum_ab"][xi] == 99  # 11 + 33 + 55
    assert got["mean_ab"][xi] == 33.0
    assert got["n"][xi] == 3

    # One-shot with an expression aggregate.
    one = batch.group_by("cat", (F.a + F.b).sum().alias("sum_ab"), count())
    assert set(one.keys()) == {"cat", "sum_ab", "count"}

    # No aggregates returns a GroupBy for chaining.
    from dftracer.utils.columnar import GroupBy

    assert isinstance(batch.group_by("cat"), GroupBy)

    # Legacy string specs still work and agree with the expression form.
    legacy = batch.group_by("cat", "sum:a")
    expr = batch.group_by("cat").agg(F.a.sum().alias("sum_a"))
    assert legacy.to_arrow().to_pydict() == expr.to_arrow().to_pydict()


def test_group_by_multi_key_matches_pandas():
    # Native multi-key group_by: composite (cat, pid) key, each column keeping
    # its own type (cat stays String, pid stays Int64), matching
    # pandas.groupby([k1, k2]).
    pd = pytest.importorskip("pandas")
    from dftracer.utils.columnar import F

    cat = ["io", "cpu", "io", "cpu", "io", "cpu", "io"]
    pid = pa.array([1, 1, 2, 1, 1, 2, 2], pa.int64())
    dur = pa.array([10, 20, 30, 40, 50, 60, 70], pa.int64())
    tbl = pa.table({"cat": cat, "pid": pid, "dur": dur})
    batch = _dataframe_from_arrow(tbl)

    # Two-step multi-key form: batch.group_by("cat", "pid").agg(...).
    got = batch.group_by("cat", "pid").agg(F.dur.sum().alias("s"), F.dur.mean().alias("m"))
    got_df = got.to_pandas().set_index(["cat", "pid"]).sort_index()

    ref = pd.DataFrame({"cat": cat, "pid": pid.to_pylist(), "dur": dur.to_pylist()})
    exp = ref.groupby(["cat", "pid"])["dur"].agg(s="sum", m="mean").sort_index()

    assert list(got_df.index) == list(exp.index)
    assert got_df["s"].tolist() == exp["s"].tolist()
    assert got_df["m"].tolist() == exp["m"].tolist()
    # Each key column keeps its own dtype: cat stays String, pid stays Int64.
    got_arrow = got.to_arrow()
    assert got_arrow.schema.field("cat").type == pa.string()
    assert got_arrow.schema.field("pid").type in (pa.int64(),)

    # Legacy inline multi-key form: trailing "op:col" specs after N key names.
    legacy = batch.group_by("cat", "pid", "sum:dur")
    legacy_df = legacy.to_pandas().set_index(["cat", "pid"]).sort_index()
    assert legacy_df["sum_dur"].tolist() == exp["s"].tolist()


def test_query_string_filter():
    # A query DSL string filters a VecBatch via the SIMD mask backend
    # (pandas.query-style), and where() routes strings through it.
    from dftracer.utils.columnar import where
    from dftracer.utils.dftracer_utils_ext import DFTUtilsQueryError

    tbl = pa.table(
        {
            "dur": pa.array([50, 150, 200, 80, 300], pa.int64()),
            "cat": ["io", "cpu", "io", "io", "net"],
        }
    )
    batch = _dataframe_from_arrow(tbl)

    got = batch.query("dur > 100 and cat == 'io'").to_arrow().to_pydict()
    assert got == {"dur": [200], "cat": ["io"]}

    # in-list
    got2 = batch.query("cat in ['io', 'net']").to_arrow().to_pydict()
    assert got2["cat"] == ["io", "io", "io", "net"]

    # where() accepts the string form on an arrow table.
    wr = where(tbl, "dur >= 150").to_pydict()
    assert wr["dur"] == [150, 200, 300]

    # A predicate with no columnar lowering raises the typed query error.
    with pytest.raises(DFTUtilsQueryError):
        batch.query("cat ~ '^i'")  # regex

    # Query plan: where + select + order by + limit in one call.
    plan = (
        batch.query("dur > 60", select=["dur", "cat"], order_by="dur", descending=True, limit=2)
        .to_arrow()
        .to_pydict()
    )
    assert plan == {"dur": [300, 200], "cat": ["net", "io"]}

    # No predicate: projection + limit only.
    proj = batch.query(select=["cat"], limit=3).to_arrow().to_pydict()
    assert proj == {"cat": ["io", "cpu", "io"]}

    # Full query: where + group by/agg + order by. A whole SQL-ish query.
    g = (
        batch.query(
            "dur > 60", group_by="cat", aggs=["count", "sum:dur"], order_by="count", descending=True
        )
        .to_arrow()
        .to_pydict()
    )
    # dur>60 keeps cpu/150, io/200, io/80, net/300 -> io=2, cpu=1, net=1.
    assert dict(zip(g["cat"], g["count"])) == {"io": 2, "cpu": 1, "net": 1}
    assert dict(zip(g["cat"], g["sum_dur"])) == {"io": 280, "cpu": 150, "net": 300}


def test_series_native_reducers():
    col = _series_from_arrow(pa.array([1, 2, 3, 4], pa.int64()))
    assert col.sum() == 10
    assert col.min() == 1
    assert col.max() == 4
    assert col.mean() == pytest.approx(2.5)
    assert col.count() == 4


def test_series_take_filter_argsort():
    col = _series_from_arrow(pa.array([10, 20, 30, 40], pa.int64()))

    assert col.take([3, 0, 2]).to_arrow().to_pylist() == [40, 10, 30]

    mask = col > 20
    assert col.filter(mask).to_arrow().to_pylist() == [30, 40]

    unsorted = _series_from_arrow(pa.array([3, 1, 2], pa.int64()))
    assert unsorted.argsort().to_arrow().to_pylist() == [1, 2, 0]
    assert unsorted.argsort(descending=True).to_arrow().to_pylist() == [0, 2, 1]


def test_series_str_predicates():
    col = _series_from_arrow(pa.array(["read", "reader", "write", "open"], pa.string()))
    assert col.str_contains("read").to_arrow().to_pylist() == [True, True, False, False]
    assert col.str_eq("write").to_arrow().to_pylist() == [False, False, True, False]
    assert col.str_starts_with("re").to_arrow().to_pylist() == [True, True, False, False]


def test_series_str_like():
    col = _series_from_arrow(
        pa.array(["apple", "apricot", "banana", "pineapple", None], pa.string())
    )
    # exact
    assert col.str_like("apple").to_arrow().to_pylist() == [
        True,
        False,
        False,
        False,
        None,
    ]
    # trailing % -> starts_with
    assert col.str_like("ap%").to_arrow().to_pylist() == [
        True,
        True,
        False,
        False,
        None,
    ]
    # leading % -> ends_with
    assert col.str_like("%apple").to_arrow().to_pylist() == [
        True,
        False,
        False,
        True,
        None,
    ]
    # %text% -> contains (SIMD substring)
    assert col.str_like("%an%").to_arrow().to_pylist() == [
        False,
        False,
        True,
        False,
        None,
    ]
    # underscore matches exactly one char
    assert col.str_like("a_ple").to_arrow().to_pylist() == [
        True,
        False,
        False,
        False,
        None,
    ]
    # escaped literal %/_
    esc = _series_from_arrow(pa.array(["50%_off", "50xoff"], pa.string()))
    assert esc.str_like("50\\%\\_off").to_arrow().to_pylist() == [True, False]


def test_series_str_contains_simd_parity():
    # Rows crossing a vector block; needle at start/middle/end/absent/empty.
    vals = [
        "the quick brown fox jumps over the lazy dog",
        "brownies for everyone in the room today ok",
        "no match here",
        "xxbrownxx",
        "",
        None,
    ]
    col = _series_from_arrow(pa.array(vals, pa.string()))
    expected_contains = [(v.find("brown") >= 0) if v is not None else None for v in vals]
    assert col.str_contains("brown").to_arrow().to_pylist() == expected_contains
    expected_find = [v.find("brown") if v is not None else None for v in vals]
    assert col.str_find("brown").to_arrow().to_pylist() == expected_find


def test_series_str_ops():
    col = _series_from_arrow(pa.array(["ab", "cd", None], pa.string()))

    # predicates
    assert col.str_ends_with("b").to_arrow().to_pylist() == [True, False, None]
    assert col.str_matches("[a-z]+").to_arrow().to_pylist() == [True, True, None]
    assert col.str_matches("a.").to_arrow().to_pylist() == [True, False, None]

    # numeric
    lens = _series_from_arrow(pa.array(["a", "bbb", "", None], pa.string()))
    assert lens.str_len_bytes().to_arrow().to_pylist() == [1, 3, 0, None]
    # 3 bytes / 2 codepoints for the accented char
    chars = _series_from_arrow(pa.array(["abc", "éc"], pa.string()))
    assert chars.str_len_chars().to_arrow().to_pylist() == [3, 2]
    finds = _series_from_arrow(pa.array(["a/b", "x", None], pa.string()))
    assert finds.str_find("/").to_arrow().to_pylist() == [1, -1, None]

    hashed = _series_from_arrow(pa.array(["POSIX", "read"], pa.string()))
    # FNV-1a 64 of the same bytes the host hashes with.
    assert hashed.fnv1a().to_arrow().to_pylist() == [
        0x4CB5D98F4B3E12C8,
        0x4CE6531FBFDDD605,
    ]
    # hex64 is dftracer's fhash/hhash form: exactly 16 hex digits, else null.
    hexes = _series_from_arrow(pa.array(["00000000deadbeef", "deadbeef", None], pa.string()))
    assert hexes.hex64_parse().to_arrow().to_pylist() == [0xDEADBEEF, None, None]
    # hex64_format is its inverse and writes the same 16-lowercase-digit form.
    assert hexes.hex64_parse().hex64_format().to_arrow().to_pylist() == [
        "00000000deadbeef",
        None,
        None,
    ]

    # transforms
    cased = _series_from_arrow(pa.array(["Abc", "XY", None], pa.string()))
    assert cased.to_lowercase().to_arrow().to_pylist() == ["abc", "xy", None]
    assert cased.to_uppercase().to_arrow().to_pylist() == ["ABC", "XY", None]

    ws = _series_from_arrow(pa.array(["  hi  ", "\tx\n", None], pa.string()))
    assert ws.str_strip().to_arrow().to_pylist() == ["hi", "x", None]
    assert ws.str_lstrip().to_arrow().to_pylist() == ["hi  ", "x\n", None]
    assert ws.str_rstrip().to_arrow().to_pylist() == ["  hi", "\tx", None]

    rep = _series_from_arrow(pa.array(["a.b.c", None], pa.string()))
    assert rep.str_replace(".", "-").to_arrow().to_pylist() == ["a-b.c", None]
    assert rep.str_replace_all(".", "-").to_arrow().to_pylist() == ["a-b-c", None]

    sl = _series_from_arrow(pa.array(["hello", None], pa.string()))
    assert sl.str_slice(1, 3).to_arrow().to_pylist() == ["ell", None]
    assert sl.str_slice(-2).to_arrow().to_pylist() == ["lo", None]

    pad = _series_from_arrow(pa.array(["7", "abcd", None], pa.string()))
    assert pad.str_pad_start(3, "*").to_arrow().to_pylist() == ["**7", "abcd", None]
    assert pad.str_pad_end(3).to_arrow().to_pylist() == ["7  ", "abcd", None]
    zf = _series_from_arrow(pa.array(["42", "-5", None], pa.string()))
    assert zf.str_zfill(4).to_arrow().to_pylist() == ["0042", "-005", None]

    # list
    sp = _series_from_arrow(pa.array(["a/b/c", "x", None], pa.string()))
    assert sp.str_split("/").to_arrow().to_pylist() == [["a", "b", "c"], ["x"], None]


def test_series_null_metadata():
    plain = _series_from_arrow(pa.array([1, 2, 3], pa.int64()))
    assert plain.null_count == 0
    assert plain.encoding == 0

    nullable = _series_from_arrow(pa.array([1, None, 3], pa.int64()))
    assert nullable.null_count == 1
    assert nullable.is_null(1) is True
    assert nullable.is_null(0) is False


def test_series_comparison_operators():
    col = _series_from_arrow(pa.array([1, 2, 3, 4], pa.int64()))
    assert (col < 3).to_arrow().to_pylist() == [True, True, False, False]
    assert (col > 2).to_arrow().to_pylist() == [False, False, True, True]
    assert col.gt(2).to_arrow().to_pylist() == [False, False, True, True]
    assert col.ge(2).to_arrow().to_pylist() == [False, True, True, True]
    assert col.le(2).to_arrow().to_pylist() == [True, True, False, False]
    assert col.eq(2).to_arrow().to_pylist() == [False, True, False, False]
    assert col.ne(2).to_arrow().to_pylist() == [True, False, True, True]


def test_dataframe_take_and_column_index():
    tbl = pa.table(
        {
            "cat": ["io", "cpu", "io", "net"],
            "n": pa.array([1, 2, 3, 4], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    assert batch.column_index("n") == 1
    assert batch.column_index("missing") == -1

    taken = batch.take([3, 0])
    assert pa.array(taken["n"]).to_pylist() == [4, 1]
    assert pa.array(taken["cat"]).to_pylist() == ["net", "io"]


def test_columnar_rejects_string_literal():
    with pytest.raises(TypeError):
        F.dur + "x"


def test_columnar_null_aware():
    # A null-bearing column flows through the engine; the op preserves the null
    # position, and fillna can close it.
    tbl = pa.table({"dur": pa.array([1, None, 3], pa.int64())})
    assert _list(columnar(F.dur + lit(1)).apply(tbl)) == [2, None, 4]
    assert _list(columnar((F.dur + lit(1)).fillna(0)).apply(tbl)) == [2, 0, 4]


def test_dataframe_tail_reverse_row_index_sample():
    tbl = pa.table(
        {
            "id": pa.array([1, 2, 3, 4, 5], pa.int64()),
            "val": pa.array([10, 20, 30, 40, 50], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)

    tail = batch.tail(2)
    assert pa.array(tail["id"]).to_pylist() == [4, 5]

    rev = batch.reverse()
    assert pa.array(rev["id"]).to_pylist() == [5, 4, 3, 2, 1]

    wi = batch.with_row_index("row")
    assert wi.column_names[0] == "row"
    assert pa.array(wi["row"]).to_pylist() == [0, 1, 2, 3, 4]

    s1 = batch.sample(3, 7)
    s2 = batch.sample(3, 7)
    assert s1.num_rows == 3
    ids1 = pa.array(s1["id"]).to_pylist()
    assert ids1 == pa.array(s2["id"]).to_pylist()
    # the sampled rows stay aligned (val == id * 10)
    assert pa.array(s1["val"]).to_pylist() == [i * 10 for i in ids1]


def test_dataframe_sort_by_multi():
    tbl = pa.table(
        {
            "a": pa.array([2, 1, 2, 1], pa.int64()),
            "b": pa.array([9, 8, 7, 6], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    s = batch.sort_by_multi(["a", "b"])
    assert pa.array(s["a"]).to_pylist() == [1, 1, 2, 2]
    assert pa.array(s["b"]).to_pylist() == [6, 8, 7, 9]
    d = batch.sort_by_multi(["a", "b"], descending=True)
    assert pa.array(d["a"]).to_pylist() == [2, 2, 1, 1]
    assert pa.array(d["b"]).to_pylist() == [9, 7, 8, 6]


def test_dataframe_unique_and_row_masks():
    tbl = pa.table(
        {
            "k": pa.array([1, 2, 1, 3, 2], pa.int64()),
            "v": pa.array([7, 8, 7, 9, 8], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    u = batch.unique()
    assert u.num_rows == 3
    assert pa.array(u["k"]).to_pylist() == [1, 2, 3]
    assert pa.array(batch.drop_duplicates()["k"]).to_pylist() == [1, 2, 3]

    dup = batch.is_duplicated()
    assert pa.array(dup).to_pylist() == [True, True, True, False, True]
    uniq = batch.is_unique()
    assert pa.array(uniq).to_pylist() == [False, False, False, True, False]


def test_dataframe_drop_fill_null_count():
    tbl = pa.table(
        {
            "a": pa.array([1, None, 3, 4], pa.int64()),
            "b": pa.array([5, 6, 7, None], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)

    nc = batch.null_count()
    assert nc.num_rows == 1
    assert pa.array(nc["a"]).to_pylist() == [1]
    assert pa.array(nc["b"]).to_pylist() == [1]

    dn = batch.drop_nulls()
    assert dn.num_rows == 2
    assert pa.array(dn["a"]).to_pylist() == [1, 3]

    filled = batch.fill_null(-1)
    assert pa.array(filled["a"]).to_pylist() == [1, -1, 3, 4]
    assert pa.array(filled["b"]).to_pylist() == [5, 6, 7, -1]


def test_dataframe_describe():
    tbl = pa.table({"x": pa.array([2, 4, 6, 8], pa.int64())})
    batch = _dataframe_from_arrow(tbl)
    d = batch.describe()
    stats = pa.array(d["statistic"]).to_pylist()
    assert stats == ["count", "null_count", "mean", "std", "min", "max"]
    x = dict(zip(stats, pa.array(d["x"]).to_pylist()))
    assert x["count"] == 4.0
    assert x["null_count"] == 0.0
    assert x["mean"] == 5.0
    assert x["min"] == 2.0
    assert x["max"] == 8.0


def test_dataframe_unpivot_melt():
    tbl = pa.table(
        {
            "id": pa.array([1, 2], pa.int64()),
            "a": pa.array([10, 20], pa.int64()),
            "b": pa.array([30, 40], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    long = batch.unpivot(["id"], ["a", "b"])
    assert long.column_names == ["id", "variable", "value"]
    assert long.num_rows == 4
    d = long.to_arrow().to_pydict()
    assert d["id"] == [1, 2, 1, 2]
    assert d["variable"] == ["a", "a", "b", "b"]
    assert d["value"] == [10, 20, 30, 40]

    # melt is an alias; a single str id/value is accepted.
    melted = batch.melt("id", ["a", "b"]).to_arrow().to_pydict()
    assert melted == d


def test_dataframe_unpivot_mixed_numeric_promotes():
    tbl = pa.table(
        {
            "id": pa.array([1, 2], pa.int64()),
            "i": pa.array([10, 20], pa.int64()),
            "f": pa.array([1.5, 2.5], pa.float64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    long = batch.unpivot(["id"], ["i", "f"])
    v = long["value"].to_arrow()
    assert v.type == pa.float64()
    assert v.to_pylist() == [10.0, 20.0, 1.5, 2.5]


def test_dataframe_explode_list_column():
    # Build a List<String> column in-engine via str_split (the Arrow importer
    # does not import list columns directly).
    tbl = pa.table(
        {
            "id": pa.array([1, 2], pa.int64()),
            "s": pa.array(["a/b/c", "x"], pa.string()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    batch = batch.with_column("vals", batch["s"].str_split("/"))
    e = batch.explode("vals")
    assert e.num_rows == 4
    d = e.to_arrow().to_pydict()
    assert d["id"] == [1, 1, 1, 2]
    assert d["vals"] == ["a", "b", "c", "x"]


def test_dataframe_to_dummies():
    tbl = pa.table(
        {
            "id": pa.array([1, 2, 3, 4], pa.int64()),
            "k": pa.array(["io", "cpu", "io", "net"], pa.string()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    oh = batch.to_dummies("k")
    # distinct sorted: cpu, io, net
    assert oh.column_names == ["id", "k_cpu", "k_io", "k_net"]
    d = oh.to_arrow().to_pydict()
    assert d["k_io"] == [1, 0, 1, 0]
    assert d["k_cpu"] == [0, 1, 0, 0]
    assert d["k_net"] == [0, 0, 0, 1]


def test_dataframe_pivot():
    tbl = pa.table(
        {
            "idx": pa.array([1, 1, 2, 1], pa.int64()),
            "col": pa.array(["a", "b", "a", "a"], pa.string()),
            "val": pa.array([10, 20, 30, 40], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    # (idx=1, col=a) collides across rows 0 (10) and 3 (40).
    p = batch.pivot("idx", "col", "val", agg="first")
    assert p.column_names == ["idx", "a", "b"]
    d = p.to_arrow().to_pydict()
    assert d["idx"] == [1, 2]
    assert d["a"] == [10, 30]  # first of (10,40) for idx1
    assert d["b"] == [20, None]  # idx2 has no col b

    # mean collapses the (1,a) collision (10,40) -> 25.0, promoting to Float64.
    m = batch.pivot("idx", "col", "val", agg="mean").to_arrow().to_pydict()
    assert m["a"] == [25.0, 30.0]


def test_dataframe_group_by_dynamic():
    tbl = pa.table(
        {
            "t": pa.array([0, 1, 2, 3, 4, 5], pa.int64()),
            "v": pa.array([10, 20, 30, 40, 50, 60], pa.int64()),
        }
    )
    batch = _dataframe_from_arrow(tbl)
    # tumbling: every=2, period defaults to every.
    g = batch.group_by_dynamic("t", 2, aggs=["sum:v", "count"]).to_arrow().to_pydict()
    assert g["t"] == [0, 2, 4]
    assert g["sum_v"] == [30, 70, 110]
    assert g["count"] == [2, 2, 2]

    # sliding: every=2, period=4 overlaps.
    s = batch.group_by_dynamic("t", 2, period=4, aggs=["sum:v"]).to_arrow().to_pydict()
    assert s["t"] == [0, 2, 4]
    assert s["sum_v"] == [100, 180, 110]


def test_series_value_counts():
    col = _series_from_arrow(pa.array([1, 2, 1, 3, 1, 2], pa.int64()))
    vc = col.value_counts()
    values = pa.array(vc["value"]).to_pylist()
    counts = pa.array(vc["count"]).to_pylist()
    assert values[0] == 1 and counts[0] == 3  # most frequent first
    assert dict(zip(values, counts)) == {1: 3, 2: 2, 3: 1}


def test_columnar_expr_apply_direct_no_wrapper():
    # (expr).apply(source) works without the columnar() wrapper.
    t = _table()
    out = (F.dur + F.cnt).apply(t)
    assert _list(out) == [1001, 2002, 3003, 4004]


def test_query_F_shorthand_and_callable():
    from dftracer.utils.query import F, Field

    assert str(F.dur > 100) == str(Field("dur") > 100)
    assert str(F("args.level") == 3) == str(Field("args.level") == 3)
    assert str(F["args.level"] == 3) == str(Field("args.level") == 3)


def test_host_ops_registered_by_name(tmp_path):
    """The utilities a plugin reaches by name through dftu.svc.ops, run through
    the same registry the plugin uses."""
    names = set(_ext.op_list())
    assert {
        "dftu.hex.parse64",
        "dftu.hex.format64",
        "dftu.fs.scan_dir",
        "dftu.fs.scan_dir_pattern",
        "dftu.file.compress",
        "dftu.file.decompress",
        "dftu.text.line_filter",
    } <= names

    assert _ext.op_info("dftu.fs.scan_dir")["signature"] == "(str) -> series"
    assert _ext.op_info("dftu.file.compress")["signature"] == "(str, str) -> bool"

    (tmp_path / "a.log").write_text("x")
    (tmp_path / "b.txt").write_text("y")

    listed = _ext.op_run("dftu.fs.scan_dir", str(tmp_path))
    assert sorted(pa.array(listed).to_pylist()) == [
        str(tmp_path / "a.log"),
        str(tmp_path / "b.txt"),
    ]

    matched = _ext.op_run("dftu.fs.scan_dir_pattern", str(tmp_path), ".log")
    assert pa.array(matched).to_pylist() == [str(tmp_path / "a.log")]

    src = tmp_path / "payload.txt"
    src.write_text("hello " * 4096)
    gz = tmp_path / "payload.txt.gz"
    back = tmp_path / "roundtrip.txt"
    assert _ext.op_run("dftu.file.compress", str(src), str(gz)) == 1
    assert gz.exists()
    assert _ext.op_run("dftu.file.decompress", str(gz), str(back)) == 1
    assert back.read_text() == src.read_text()

    # A missing input is reported, not raised.
    assert _ext.op_run("dftu.file.compress", str(tmp_path / "absent"), str(gz)) == 0

    lines = _series_from_arrow(pa.array(["ERROR: full", "INFO: ok", "ERROR: slow"], pa.string()))
    kept = _ext.op_run("dftu.text.line_filter", lines._native, "ERROR")
    assert pa.array(kept).to_pylist() == ["ERROR: full", "ERROR: slow"]
