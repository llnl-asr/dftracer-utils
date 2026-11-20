# SPDX-License-Identifier: MIT
"""DataFrame, LazyFrame and TraceViewer are the native types, subclassed in
Python: the native calls return the public classes, a frame carries its state
in slots, and rebinding a frame in place does not touch other frames."""

import gc
import gzip
import json
import sys
import weakref

import pytest

import dftracer.utils as d
from dftracer.utils import DataFrame, LazyFrame, Series, TraceViewer, col
from dftracer.utils import dftracer_utils_ext as _ext


@pytest.fixture
def trace(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    with gzip.open(path, "wt") as f:
        for i in range(20):
            f.write(
                json.dumps(
                    {
                        "ph": 1,
                        "name": "n%d" % (i % 3),
                        "cat": "APP",
                        "pid": 1,
                        "tid": 1,
                        "ts": 1000 + i * 10,
                        "dur": 5 + i,
                        "args": {},
                    }
                )
                + "\n"
            )
    return path


def _frame():
    return DataFrame({"a": [3, 1, 2], "b": [10, 20, 30]})


def test_the_public_frames_subclass_the_native_types_with_no_wrapper():
    assert issubclass(DataFrame, _ext.DataFrame)
    assert issubclass(LazyFrame, _ext.LazyFrame)
    assert issubclass(TraceViewer, _ext.TraceViewer)
    df = _frame()
    for obj in (df, df.lazy()):
        assert not hasattr(obj, "_native")
        assert not hasattr(obj, "__dict__")
    for cls in (DataFrame, LazyFrame, TraceViewer):
        assert "_native" not in dir(cls)


def test_native_calls_build_the_public_classes():
    df = _frame()
    assert type(df.head(2)) is DataFrame
    assert type(df["a"]) is Series
    assert type(df.lazy()) is LazyFrame
    assert type(df.lazy().head(1)) is LazyFrame
    assert type(df.lazy().collect()) is DataFrame
    assert type(_ext.DataFrame.select(df, "a")) is DataFrame
    assert type(_ext.DataFrame.lazy(df)) is LazyFrame
    assert type(d.scan_file("/no/such/file.pfw.gz")) is LazyFrame


def test_trace_viewer_is_not_a_lazyframe_and_hands_out_its_plan(trace):
    tv = TraceViewer(trace)
    assert type(tv) is TraceViewer
    assert not isinstance(tv, LazyFrame)
    plan = tv.lazy()
    assert type(plan) is LazyFrame
    assert type(tv.head(3)) is TraceViewer
    assert type(tv.filter("dur > 1")) is TraceViewer
    assert type(tv.sort("ts")) is LazyFrame
    assert type(tv.collect()) is DataFrame
    assert tv.collect().height == 20
    assert tv.head(3).collect().height == 3
    assert tv.columns == plan.columns


def test_trace_viewer_keeps_its_runtime_through_builders(trace):
    rt = d.Runtime(threads=2)
    tv = TraceViewer(trace, runtime=rt)
    assert tv._runtime is rt
    assert tv.head(2)._runtime is rt
    assert tv.phase("events")._runtime is rt
    assert tv.lazy()._runtime is rt
    assert tv.collect().height == 20


def test_a_frame_carries_its_index_through_native_ops():
    df = _frame().set_index("a")
    assert df.index_names() == ["a"]
    assert df.head(2).index_names() == ["a"]
    assert df.sort_by("b").index_names() == ["a"]
    assert _frame().index_names() == []
    lf = _frame().lazy().set_index("a")
    assert lf.head(1).index_names() == ["a"]


def test_setitem_rebinds_this_frame_only():
    df = _frame()
    twin = df.copy()
    view = df.head(2)
    df["c"] = df["a"] * 2
    assert df.columns == ["a", "b", "c"]
    assert twin.columns == ["a", "b"]
    assert view.columns == ["a", "b"]
    assert df["c"].to_list() == [6, 2, 4]
    df.pop("c")
    assert df.columns == ["a", "b"]
    df.insert(0, "z", [7, 8, 9])
    assert df.columns == ["z", "a", "b"] and twin.columns == ["a", "b"]


def test_replace_is_a_native_in_place_swap_that_shares_the_columns():
    one, two = _frame(), DataFrame({"x": [1.5]})
    _ext.DataFrame._replace(one, two)
    assert one.columns == ["x"] and one.num_rows == 1
    assert two.columns == ["x"]
    _ext.DataFrame._replace(one, one)
    assert one.columns == ["x"]
    with pytest.raises(TypeError):
        _ext.DataFrame._replace(one, 5)
    lf, other = _frame().lazy(), DataFrame({"y": [1, 2]}).lazy()
    _ext.LazyFrame._replace(lf, other)
    assert lf.columns == ["y"] and lf.collect().num_rows == 2


def test_share_makes_a_distinct_object_over_the_same_columns():
    df = _frame()
    same = _ext.DataFrame.share(df)
    assert same is not df and type(same) is DataFrame
    assert same["a"].to_list() == df["a"].to_list() and same.index_names() == []
    lf = df.lazy()
    again = _ext.LazyFrame.share(lf)
    assert again is not lf and type(again) is LazyFrame
    assert again.collect().to_dict() == lf.collect().to_dict()


def test_constructors_return_the_public_classes():
    df = _frame()
    assert DataFrame(df) is df
    assert type(DataFrame({"a": [1]})) is DataFrame
    lf = LazyFrame(df.lazy(), d.Runtime(threads=1))
    assert type(lf) is LazyFrame and lf._runtime is not None


@pytest.mark.parametrize("cls", [DataFrame, LazyFrame, TraceViewer])
def test_a_user_subclass_is_refused(cls):
    class Mine(cls):  # type: ignore[valid-type,misc]
        __slots__ = ()

    with pytest.raises(TypeError, match="cannot be subclassed"):
        if cls is DataFrame:
            Mine({"a": [1]})
        elif cls is LazyFrame:
            Mine(_frame().lazy())
        else:
            Mine("x.pfw.gz")


def test_the_native_frame_types_have_no_constructor_state_of_their_own():
    with pytest.raises(TypeError):
        _ext.LazyFrame()
    with pytest.raises(TypeError, match="already initialized"):
        tv = _ext.TraceViewer.__new__(_ext.TraceViewer)
        _ext.TraceViewer.__init__(tv, "x.pfw.gz")
        _ext.TraceViewer.__init__(tv, "y.pfw.gz")


def test_frames_free_without_leaking_the_class_reference():
    def cycle(n):
        for _ in range(n):
            f = _frame()
            f.head(1).lazy().head(1).collect()
            del f
        gc.collect()

    cycle(10)
    base = (sys.getrefcount(DataFrame), sys.getrefcount(LazyFrame))
    cycle(5000)
    assert (sys.getrefcount(DataFrame), sys.getrefcount(LazyFrame)) == base


def test_frames_take_weak_references_and_pickle():
    import pickle

    df = _frame()
    ref = weakref.ref(df)
    assert ref() is df
    assert pickle.loads(pickle.dumps(df)).to_dict() == df.to_dict()
    del df
    gc.collect()
    assert ref() is None


def test_collect_all_takes_plans_and_viewers(trace):
    tv = TraceViewer(trace)
    frames = d.collect_all([tv, tv.head(2), _frame().lazy().filter(col("a") > 1)])
    assert [f.num_rows for f in frames] == [20, 2, 2]


def test_trace_viewer_aggregate_is_agg(trace):
    tv = TraceViewer(trace)
    assert TraceViewer.aggregate is TraceViewer.agg
    assert type(tv.group_by("name").aggregate("count")) is TraceViewer
    got = tv.group_by("name").aggregate("count").collect().sort_by("name")
    want = tv.group_by("name").agg("count").collect().sort_by("name")
    assert got.to_dict() == want.to_dict() and got.num_rows == 3


def test_copy_and_deepcopy_of_frames(trace):
    import copy

    df = _frame().set_index("a")
    for dup in (copy.copy(df), copy.deepcopy(df)):
        assert type(dup) is DataFrame and dup is not df
        assert dup.to_dict() == df.to_dict() and dup.index_names() == ["a"]
    rt = d.Runtime(threads=1)
    lf = LazyFrame(_frame().lazy(), rt).set_index("a")
    for dup in (copy.copy(lf), copy.deepcopy(lf)):
        assert type(dup) is LazyFrame and dup is not lf
        assert dup._runtime is rt and dup.index_names() == ["a"]
        assert dup.collect().to_dict() == lf.collect().to_dict()
    _ext.LazyFrame._replace(dup, DataFrame({"z": [1]}).lazy())
    assert lf.columns == ["a", "b"]
    tv = TraceViewer(trace)
    for dup in (copy.copy(tv), copy.deepcopy(tv)):
        assert type(dup) is TraceViewer
        assert dup.collect().to_dict() == tv.collect().to_dict()
