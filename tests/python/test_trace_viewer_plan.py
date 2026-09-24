"""TraceViewer as a LazyFrame: one collect, absorbed ops, lazy indexing,
lazy results, collect_all and sessions."""

import gzip
import json
import os

import pytest

import dftracer.utils as dft
from dftracer.utils import (
    DFTUtilsValueError,
    F,
    LazyFrame,
    LazyResult,
    LazyScalar,
    TraceViewer,
    col,
)
from dftracer.utils.enums import DType

# Two lanes of nested calls: "step" holds "read", which holds "decode"; then a
# sibling "write". 32 events, 16 POSIX.
LANES = (1, 2)
STEPS = 4
EVENTS = (
    ("step", "APP", 0, 900),
    ("read", "POSIX", 10, 400),
    ("decode", "APP", 20, 100),
    ("write", "POSIX", 500, 300),
)


@pytest.fixture
def trace(tmp_path):
    path = str(tmp_path / "nested.pfw.gz")
    with gzip.open(path, "wt") as f:
        for tid in LANES:
            for k in range(STEPS):
                t0 = 1000 + k * 1000
                for name, cat, off, dur in EVENTS:
                    f.write(
                        json.dumps(
                            {
                                "ph": "X",
                                "name": name,
                                "cat": cat,
                                "pid": 1,
                                "tid": tid,
                                "ts": t0 + off,
                                "dur": dur,
                                "args": {"size": dur * 2},
                            }
                        )
                        + "\n"
                    )
    return path


@pytest.fixture
def tv(trace):
    return TraceViewer(trace).metadata(False)


def _rows(frame, cols):
    data = frame.to_dict()
    return sorted(zip(*(data[c] for c in cols)))


class TestOneCollect:
    def test_collect_returns_a_dataframe(self, tv):
        out = tv.filter('cat == "POSIX"').collect()
        assert isinstance(out, dft.DataFrame)
        assert out.height == 16

    def test_trace_group_by_and_agg(self, tv):
        out = tv.group_by("name").agg("count", F.dur.sum()).collect()
        assert _rows(out, ["name", "count", "sum_dur"]) == [
            ("decode", 8, 800),
            ("read", 8, 3200),
            ("step", 8, 7200),
            ("write", 8, 2400),
        ]

    def test_expression_filter_on_events(self, tv):
        assert tv.filter(col("dur") > 350).collect().height == 16

    def test_expression_filter_on_an_aggregation_filters_rows(self, tv):
        out = tv.group_by("name").agg(F.dur.sum()).filter(col("sum_dur") > 3000).collect()
        assert sorted(out.to_dict()["name"]) == ["read", "step"]


class TestTypes:
    def test_absorbed_ops_keep_the_trace_type(self, tv):
        for plan in (
            tv.filter("dur > 1"),
            tv.select("name", "dur"),
            tv.sort_by("ts"),
            tv.head(3),
            tv.offset(2),
            tv.with_column("d2", col("dur") * 2),
            tv[["name"]],
            tv[1:3],
        ):
            assert isinstance(plan, TraceViewer)

    def test_other_ops_are_plain_lazyframes(self, tv):
        joined = tv.join(tv.select("name", "cat"), on="name")
        assert type(joined) is LazyFrame
        assert type(tv.flamegraph()) is LazyFrame
        assert type(tv.unique("name")) is LazyFrame

    def test_trace_builder_after_a_generic_op_names_it(self, tv):
        with pytest.raises(DFTUtilsValueError, match="sort_by"):
            tv.sort_by("ts").phase("events")

    def test_terminal_names_the_op_it_cannot_take(self, tv):
        with pytest.raises(DFTUtilsValueError, match="sort_by"):
            tv.sort_by("ts").flamegraph()
        with pytest.raises(DFTUtilsValueError, match="aggregates"):
            tv.group_by("name").agg("count").flamegraph()
        with pytest.raises(DFTUtilsValueError, match="group_by or agg"):
            tv.aggregate_partial()


class TestColumnsAndSchema:
    def test_columns_and_schema_are_properties(self, tv):
        plan = tv.select("name", "dur")
        assert plan.columns == ["name", "dur"]
        assert plan.schema == {"name": DType.STRING, "dur": DType.UINT64}

    def test_aggregation_schema_without_a_scan(self, tv):
        plan = tv.group_by("name").agg("count")
        assert plan.columns == ["name", "count"]
        assert plan.schema["count"] == DType.INT64

    def test_select_reads_an_arg_the_index_does_not_list(self, tv):
        out = tv.select("name", "size").collect()
        assert out.columns == ["name", "args.size"]
        assert sorted(set(out.to_dict()["args.size"])) == [200, 600, 800, 1800]

    def test_column_info_reads_the_index(self, trace, tv):
        with dft.Indexer(files=[trace]) as indexer:
            indexer.ensure_indexed()
        info = tv.column_info()
        assert info["dur"] == "int64"
        assert "size" in info


class TestLazyIndexing:
    def test_getitem_forms(self, tv):
        assert tv[["name", "dur"]].columns == ["name", "dur"]
        assert tv[tv["dur"] > 350].collect().height == 16
        assert tv["dur"].name == "dur"
        assert tv.sort_by("ts")[2:5].collect().height == 3
        with pytest.raises(KeyError):
            tv["nope"]
        with pytest.raises(ValueError):
            tv[::2]

    def test_bound_column_reductions_are_lazy_scalars(self, tv):
        mean = tv["dur"].mean()
        assert isinstance(mean, LazyScalar)
        assert mean.collect() == 425.0
        assert tv.filter('cat == "APP"')["dur"].max().collect() == 900
        assert tv["dur"].count().collect() == 32


class TestTerminals:
    def test_flamegraph_is_a_plan(self, tv):
        top = tv.flamegraph().filter(col("level") == 1).collect()
        assert _rows(top, ["name", "total"]) == [("step", 7200.0)]

    def test_containment_collects_both_frames(self, tv):
        both = tv.containment().collect()
        assert both.call_tree.height == 32
        assert _rows(both.flamegraph, ["name", "level"]) == _rows(
            tv.flamegraph().collect(), ["name", "level"]
        )

    def test_partials_merge_to_the_result(self, tv):
        part = tv.flamegraph_partial().collect()
        assert isinstance(part, bytes)
        merged = TraceViewer.merge_flamegraph_partials([part, part])
        assert _rows(merged, ["name", "level"]) == _rows(
            tv.flamegraph().collect(), ["name", "level"]
        )
        agg = tv.group_by("name").agg("count")
        out = agg.merge_partials([agg.aggregate_partial().collect()])
        assert _rows(out, ["name", "count"]) == _rows(agg.collect(), ["name", "count"])

    def test_sink_json_eager_and_lazy(self, tv, tmp_path):
        path = str(tmp_path / "posix.json")
        stats = tv.filter('cat == "POSIX"').sink_json(path)
        assert stats["events_matched"] == 16
        with open(path) as f:
            assert len(f.readlines()) == 16
        lazy = tv.filter('cat == "APP"').sink_json(str(tmp_path / "app.json"), lazy=True)
        assert isinstance(lazy, LazyResult)
        assert lazy.collect()["events_matched"] == 16

    def test_statistics(self, tv):
        stats = tv.statistics()
        assert stats["duration_count"] == 32
        assert stats["duration_mean_us"] == 425.0

    def test_resolution_on_occupancy(self, tv):
        out = tv.time_range(0, 10000).group_by("name").agg(F.dur.busy(resolution="1ms")).collect()
        assert set(out.columns) >= {"name", "busy"}
        with pytest.raises(ValueError, match="one resolution"):
            tv.group_by("name").agg(F.dur.busy(resolution=1), F.dur.active(resolution=2))

    def test_compare(self, tv):
        base = tv.group_by("name").agg("count")
        out = base.compare(tv.filter('cat == "POSIX"')).collect()
        assert _rows(out, ["name", "l_count", "r_count"]) == [
            ("decode", 8, None),
            ("read", 8, 8),
            ("step", 8, None),
            ("write", 8, 8),
        ]


class TestCollectAll:
    def test_mixed_roots_in_order(self, tv, tmp_path):
        path = str(tmp_path / "app.json")
        fg, both, stats, rows, mean = dft.collect_all(
            [
                tv.flamegraph(),
                tv.containment(),
                tv.filter('cat == "APP"').sink_json(path, lazy=True),
                tv.filter('cat == "POSIX"'),
                tv["dur"].mean(),
            ]
        )
        assert _rows(fg, ["name", "level"]) == _rows(both.flamegraph, ["name", "level"])
        assert both.call_tree.height == 32
        assert stats["events_matched"] == 16
        assert rows.height == 16
        assert mean == 425.0

    def test_session(self, tv, tmp_path):
        path = str(tmp_path / "posix.json")
        with tv.session() as s:
            by_name = s.collect(tv.group_by("name").agg("count"))
            both = s.collect(tv.containment())
            written = s.sink_json(tv.filter('cat == "POSIX"'), path)
        assert by_name.result().height == 4
        assert both.result().call_tree.height == 32
        assert written.result()["events_matched"] == 16
        with open(path) as f:
            assert len(f.readlines()) == 16
        with pytest.raises(RuntimeError):
            s.collect(tv)

    def test_session_materialize_serves_later_reads(self, tv, tmp_path):
        agg = tv.rollup_root(str(tmp_path / "rollups")).group_by("name").agg("count")
        assert agg.mv_source() == []
        s = tv.session()
        built = s.materialize(agg)
        s.execute()
        assert isinstance(built.result(), dict)
        assert _rows(agg.collect(), ["name", "count"]) == _rows(
            tv.group_by("name").agg("count").collect(), ["name", "count"]
        )


class TestRuntimeAndStream:
    def test_stream_chunks(self, tv):
        sizes = [c.height for c in tv.stream(10)]
        assert sum(sizes) == 32

    def test_runtime_runs_the_plan(self, trace):
        rt = dft.Runtime(threads=2)
        try:
            viewer = TraceViewer(trace, runtime=rt).metadata(False)
            assert viewer.filter('cat == "POSIX"').collect().height == 16
            assert viewer.group_by("name").agg("count").sort_by("name").collect().height == 4
        finally:
            rt.shutdown()
        assert rt.get_progress()["completed"] >= 2

    def test_directory_input(self, trace):
        viewer = TraceViewer(os.path.dirname(trace)).metadata(False)
        assert viewer.collect().height == 32
