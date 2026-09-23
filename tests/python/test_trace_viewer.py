#!/usr/bin/env python3
"""Tests for the arrow-first TraceViewer."""

import gzip
import json

import pyarrow as pa

import dftracer.utils as dftu_utils
from dftracer.utils import TraceViewer

from .common import Environment


def _indexed(env):
    gz = env.create_test_gzip_file()
    with dftu_utils.Indexer(files=[gz]) as indexer:
        indexer.ensure_indexed()
    return gz


def _concat(chunks):
    """Concatenate native DataFrame stream chunks (each its own schema) into one,
    Arrow only at the final edge."""
    chunks = list(chunks)
    df = chunks[0] if len(chunks) == 1 else chunks[0].concat(*chunks[1:], how="diagonal")
    return df.to_arrow()


def _make_trace(env, name, rows):
    import os

    path = os.path.join(env.temp_dir, name)
    with gzip.open(path, "wt") as f:
        f.write("\n".join(json.dumps(r) for r in rows) + "\n")
    with dftu_utils.Indexer(files=[path]) as indexer:
        indexer.ensure_indexed()
    return path


class TestTraceViewer:
    def test_collect_group_by_agg_returns_arrow(self):
        with Environment(lines=200) as env:
            gz = _indexed(env)
            tbl = (
                TraceViewer(gz)
                .group_by("cat")
                .agg("count", "mean:dur", "std:dur")
                .collect()
                .collect()
            )
            df = tbl.to_pandas()
            assert set(["cat", "count", "mean_dur", "std_dur"]).issubset(df.columns)
            # Every event lands in exactly one cat group.
            assert int(df["count"].sum()) == 200
            assert set(df["cat"]) <= {"posix", "stdio"}  # group value lowercases cat

    def test_agg_set_union_distinct_values(self):
        with Environment(lines=200) as env:
            gz = _indexed(env)
            df = TraceViewer(gz).agg("set_union:cat").collect().collect().to_pandas()
            assert df.shape[0] == 1
            # One string column of the distinct cat values, joined by \x1e.
            assert set(df["set_cat"].iloc[0].split("\x1e")) == {"POSIX", "STDIO"}

    def test_time_metric_reads_cm_record(self):
        with Environment(lines=1) as env:
            rows = [
                {
                    "name": "CM",
                    "ph": "M",
                    "cat": "dftracer",
                    "pid": 0,
                    "tid": 0,
                    "args": {"name": "time_metric", "value": "NS"},
                },
            ]
            for i in range(10):
                rows.append(
                    {
                        "ph": "X",
                        "name": "read",
                        "cat": "POSIX",
                        "pid": 1,
                        "tid": 10,
                        "ts": 1000 + i * 100,
                        "dur": 5,
                        "args": {},
                    }
                )
            gz = _make_trace(env, "time_metric.pfw.gz", rows)
            assert TraceViewer(gz).time_metric() == "ns"

    def test_phase_metadata_row_query_returns_filtered_rows(self):
        # A phase("metadata") row query (no group_by/agg) must return the
        # metadata records as rows, args flattened, and honor the query filter.
        from dftracer.utils import Field

        with Environment(lines=1) as env:
            rows = [
                {
                    "name": "CM",
                    "ph": "M",
                    "pid": 0,
                    "tid": 0,
                    "ts": 0,
                    "args": {"name": "time_metric", "value": "NS"},
                },
            ] + [
                {
                    "ph": "X",
                    "name": "read",
                    "cat": "POSIX",
                    "pid": 1,
                    "tid": 1,
                    "ts": 1000 + i,
                    "dur": 5,
                    "args": {},
                }
                for i in range(5)
            ]
            gz = _make_trace(env, "meta_rows.pfw.gz", rows)

            meta = TraceViewer(gz).phase("metadata").collect().collect().to_arrow()
            assert meta.num_rows >= 1
            assert "args.name" in meta.column_names
            assert "args.value" in meta.column_names
            d = meta.to_pydict()
            assert "CM" in d["name"]
            assert "time_metric" in d["args.name"]

            filtered = (
                TraceViewer(gz)
                .phase("metadata")
                .query(Field("args.name") == "time_metric")
                .collect()
                .collect()
                .to_arrow()
            )
            assert filtered.num_rows == 1
            assert filtered.to_pydict()["args.value"][0] == "NS"

            # A non-matching filter proves the query really runs on metadata.
            none = (
                TraceViewer(gz)
                .phase("metadata")
                .query(Field("args.name") == "nope")
                .collect()
                .collect()
                .to_arrow()
            )
            assert none.num_rows == 0

            # A normal event row query never includes the metadata record.
            events = TraceViewer(gz).phase("events").collect().collect().to_arrow()
            assert events.num_rows == 5
            assert "CM" not in events.to_pydict()["name"]

    def test_time_metric_defaults_to_us(self):
        with Environment(lines=200) as env:
            gz = _indexed(env)
            assert TraceViewer(gz).time_metric() == "us"

    def test_time_bucket_normalize_alignment(self):
        with Environment(lines=1) as env:
            # Events start at ts=5000 (arbitrary absolute time); width 700 does
            # not divide 5000, so alignment changes the first bucket.
            rows = [
                {
                    "ph": "X",
                    "name": "read",
                    "cat": "posix",
                    "pid": 1,
                    "tid": 1,
                    "ts": 5000 + 100 * i,
                    "dur": 5,
                    "args": {},
                }
                for i in range(20)
            ]
            gz = _make_trace(env, "aligned.pfw.gz", rows)

            def first_bucket(normalize_to):
                tbl = (
                    TraceViewer(gz)
                    .group_by("cat")
                    .time_bucket(700, normalize_to)
                    .agg("count")
                    .collect()
                    .collect()
                )
                buckets = tbl.to_arrow().column("time_bucket").to_pylist()
                return min(int(b) for b in buckets)

            assert first_bucket(None) == 4900  # floor(5000/700)*700
            assert first_bucket("min") == 5000  # aligned to the trace min ts
            assert first_bucket(5000) == 5000  # explicit origin

    def test_phase_selects_aggregated_events(self):
        with Environment(lines=1) as env:
            rows = (
                [
                    {
                        "ph": "X",
                        "name": "read",
                        "cat": "posix",
                        "pid": 1,
                        "tid": 1,
                        "ts": 1000 + i,
                        "dur": 5,
                        "args": {},
                    }
                    for i in range(5)
                ]
                + [
                    {
                        "ph": "C",
                        "name": "cpu",
                        "cat": "sys",
                        "pid": 1,
                        "tid": 1,
                        "ts": 2000 + i,
                        "args": {"user_pct": 50},
                    }
                    for i in range(3)
                ]
                + [
                    {
                        "ph": "A",
                        "name": "agg",
                        "cat": "posix",
                        "pid": 1,
                        "tid": 1,
                        "ts": 3000 + i,
                        "dur": 9,
                        "args": {},
                    }
                    for i in range(4)
                ]
                + [
                    {
                        "ph": "M",
                        "name": "process_name",
                        "pid": 1,
                        "tid": 1,
                        "ts": 0,
                        "args": {"value": "proc%d" % i},
                    }
                    for i in range(2)
                ]
            )
            gz = _make_trace(env, "phases.pfw.gz", rows)

            def count(ph):
                t = (
                    TraceViewer(gz)
                    .phase(ph)
                    .agg("count")
                    .collect()
                    .collect()
                    .to_arrow()
                    .to_pydict()
                )
                return int(t["count"][0]) if t["count"] else 0

            assert count("events") == 5
            assert count("counters") == 3
            assert count("aggregated") == 4  # ph="A" folded events, newly selectable
            assert count("metadata") == 2  # ph="M" metadata, aggregated only when selected

    def test_collect_resolves_resolved_fields_per_event(self):
        # resolved.fpath/hostname resolve fhash/hhash through the index name
        # tables (built by the full bloom indexer), as per-event columns.
        with Environment(lines=1) as env:
            rows = [
                {
                    "ph": "M",
                    "name": "FH",
                    "pid": 1,
                    "tid": 1,
                    "ts": 0,
                    "args": {"name": "/data/f.dat", "value": "fa"},
                },
                {
                    "ph": "M",
                    "name": "HH",
                    "pid": 1,
                    "tid": 1,
                    "ts": 0,
                    "args": {"name": "node01", "value": "h1"},
                },
            ] + [
                {
                    "ph": "X",
                    "name": "read",
                    "cat": "posix",
                    "pid": 1,
                    "tid": 1,
                    "ts": 1000 + i,
                    "dur": 5,
                    "args": {"ret": i, "fhash": "fa", "hhash": "h1"},
                }
                for i in range(3)
            ]
            gz = _make_trace(env, "resolved.pfw.gz", rows)

            d = (
                TraceViewer(gz)
                .phase("events")
                .select("name", "resolved.fpath", "r.host")
                .collect()
                .collect()
                .to_arrow()
                .to_pydict()
            )
            assert d["resolved.fpath"] == ["/data/f.dat"] * 3
            assert d["r.host"] == ["node01"] * 3

            # group_by on the resolved alias resolves the same value.
            gf = (
                TraceViewer(gz)
                .phase("events")
                .group_by("resolved.fpath")
                .agg("count")
                .collect()
                .collect()
                .to_arrow()
                .to_pydict()
            )
            assert gf["file_path"] == ["/data/f.dat"]
            gh = (
                TraceViewer(gz)
                .phase("events")
                .group_by("resolved.hostname")
                .agg("count")
                .collect()
                .collect()
                .to_arrow()
                .to_pydict()
            )
            assert gh["host_name"] == ["node01"]

    def test_collect_resolves_fhash_hhash_per_event(self):
        # fhash/hhash are parsed into dedicated fields (not generic args); the
        # raw row builder must resolve them to their string, matching group_by.
        with Environment(lines=1) as env:
            rows = [
                {
                    "ph": "X",
                    "name": "read",
                    "cat": "posix",
                    "pid": 1,
                    "tid": 1,
                    "ts": 1000 + i,
                    "dur": 5,
                    "args": {"ret": i, "fhash": "fa", "hhash": "h1"},
                }
                for i in range(4)
            ]
            gz = _make_trace(env, "hash.pfw.gz", rows)

            sel = (
                TraceViewer(gz)
                .select("name", "fhash", "hhash")
                .collect()
                .collect()
                .to_arrow()
                .to_pydict()
            )
            assert sel["fhash"] == ["fa"] * 4
            assert sel["hhash"] == ["h1"] * 4

            # The all-columns collect includes them too.
            allc = TraceViewer(gz).collect().collect().to_arrow().to_pydict()
            assert allc["fhash"] == ["fa"] * 4
            assert allc["hhash"] == ["h1"] * 4

            # And they still resolve as group_by keys (the pre-existing path).
            g = (
                TraceViewer(gz)
                .group_by("fhash")
                .agg("count")
                .collect()
                .collect()
                .to_arrow()
                .to_pydict()
            )
            assert g["fhash"] == ["fa"]

    def test_view_var_std_match_dataframe_engine(self):
        # The View and the dataframe engine must agree on var/std/mean (both the
        # sample convention over the field-present count, matching pandas).
        with Environment(lines=1) as env:
            rows = [
                {
                    "ph": "X",
                    "name": "read",
                    "cat": "posix",
                    "pid": 1,
                    "tid": 1,
                    "ts": 1000 + i,
                    "dur": 10 + (i * 7) % 50,
                }
                for i in range(60)
            ]
            gz = _make_trace(env, "parity.pfw.gz", rows)
            tv = TraceViewer(gz)
            view = tv.group_by("cat").agg("var:dur", "std:dur", "mean:dur").collect().collect()
            view = view.to_arrow().to_pydict()
            eng = (
                tv.phase("events")
                .collect()
                .collect()
                .group_by("cat", "var:dur", "std:dur", "mean:dur")
                .to_arrow()
                .to_pydict()
            )
            for col in ("var_dur", "std_dur", "mean_dur"):
                assert abs(view[col][0] - eng[col][0]) < 1e-9, col

    def test_collect_row_query_returns_events(self):
        with Environment(lines=1) as env:
            rows = [
                {
                    "ph": "X",
                    "name": "read",
                    "cat": "posix",
                    "pid": 1,
                    "tid": 1,
                    "ts": 1000 + i,
                    "dur": 5,
                    "args": {"ret": 100 + i, "path": "/f%d" % i},
                }
                for i in range(5)
            ]
            gz = _make_trace(env, "events.pfw.gz", rows)

            # A plain collect() (no group_by/agg) returns the matching events
            # with every column - top-level plus a union of the args, each
            # flattened arg prefixed "args." so it can't collide with a
            # top-level column of the same bare name.
            d = TraceViewer(gz).collect().collect().to_arrow().to_pydict()
            assert {"name", "cat", "pid", "tid", "ts", "dur", "ph", "args.ret", "args.path"} <= set(
                d
            )
            assert d["args.ret"] == [100, 101, 102, 103, 104]
            assert d["name"] == ["read"] * 5
            assert d["args.path"][0] == "/f0"

            # select projects a subset (top-level + arg names); a bare arg name
            # still resolves, but the output column is canonicalized to
            # "args.<key>".
            sel = (
                TraceViewer(gz)
                .select("name", "ts", "ret")
                .collect()
                .collect()
                .to_arrow()
                .to_pydict()
            )
            assert set(sel) == {"name", "ts", "args.ret"}

            # filter narrows the event rows (query DSL still resolves the bare
            # arg name).
            assert TraceViewer(gz).filter("ret > 102").collect().collect().num_rows == 2

    def test_call_tree_and_flamegraph(self):
        # One (pid,tid) lane, nested: A[0,100) > B[10,40) > C[15,25).
        with Environment(lines=1) as env:
            rows = [
                {"ph": "X", "name": n, "cat": "posix", "pid": 1, "tid": 1, "ts": t, "dur": d}
                for n, t, d in [("A", 0, 100), ("B", 10, 30), ("C", 15, 10)]
            ]
            gz = _make_trace(env, "tree.pfw.gz", rows)

            ct = TraceViewer(gz).call_tree().to_arrow().to_pydict()
            order = {n: i for i, n in enumerate(ct["name"])}
            assert ct["level"][order["A"]] == 0
            assert ct["parent_id"][order["A"]] == -1
            assert ct["level"][order["B"]] == 1
            assert ct["parent_id"][order["B"]] == order["A"]
            assert ct["level"][order["C"]] == 2
            assert ct["parent_id"][order["C"]] == order["B"]

            fg = TraceViewer(gz).flamegraph().to_arrow().to_pydict()
            node = {n: i for i, n in enumerate(fg["name"])}
            assert fg["total"][node["A"]] == 100 and fg["self"][node["A"]] == 70
            assert fg["total"][node["B"]] == 30 and fg["self"][node["B"]] == 20
            assert fg["total"][node["C"]] == 10 and fg["self"][node["C"]] == 10
            assert fg["parent"][node["A"]] == node["all"]

            # Composes with builder ops: filter drops B, so the tree recomputes
            # and C folds directly under A (self 70 -> 90).
            fg2 = TraceViewer(gz).filter('name != "B"').flamegraph().to_arrow().to_pydict()
            n2 = {n: i for i, n in enumerate(fg2["name"])}
            assert "B" not in n2
            assert fg2["self"][n2["A"]] == 90
            assert fg2["parent"][n2["C"]] == n2["A"]

    def test_containment_single_fold_both_outputs(self):
        with Environment(lines=1) as env:
            rows = [
                {"ph": "X", "name": n, "cat": "posix", "pid": 1, "tid": 1, "ts": t, "dur": d}
                for n, t, d in [("A", 0, 100), ("B", 10, 30), ("C", 15, 10)]
            ]
            gz = _make_trace(env, "both.pfw.gz", rows)

            # Standalone: one scan, both frames.
            c = TraceViewer(gz).containment()
            ct = c.call_tree().to_arrow().to_pydict()
            fg = c.flamegraph().to_arrow().to_pydict()
            order = {n: i for i, n in enumerate(ct["name"])}
            assert ct["parent_id"][order["B"]] == order["A"]
            node = {n: i for i, n in enumerate(fg["name"])}
            assert fg["self"][node["A"]] == 70

            # Session single branch: one fold feeds both.
            with TraceViewer(gz).session() as s:
                cc = s.view().containment()
            ct2 = cc.call_tree().to_arrow().to_pydict()
            fg2 = cc.flamegraph().to_arrow().to_pydict()
            assert set(ct2["name"]) == {"A", "B", "C"}
            n2 = {n: i for i, n in enumerate(fg2["name"])}
            assert fg2["total"][n2["A"]] == 100

    def test_flamegraph_distributed_partials(self):
        # Two "ranks" own disjoint files; each folds a serialized arena partial,
        # rank 0 merges them. Result must equal a single pass over both files.
        with Environment(lines=1) as env:
            r1 = [
                {"ph": "X", "name": n, "cat": "c", "pid": 1, "tid": 1, "ts": t, "dur": d}
                for n, t, d in [("A", 0, 100), ("B", 10, 30)]
            ]
            r2 = [
                {"ph": "X", "name": n, "cat": "c", "pid": 2, "tid": 1, "ts": t, "dur": d}
                for n, t, d in [("A", 0, 50), ("C", 5, 20)]
            ]
            g1 = _make_trace(env, "rank1.pfw.gz", r1)
            g2 = _make_trace(env, "rank2.pfw.gz", r2)
            p1 = TraceViewer(g1).flamegraph_partial()
            p2 = TraceViewer(g2).flamegraph_partial()
            assert isinstance(p1, bytes) and len(p1) > 0
            merged = TraceViewer.merge_flamegraph_partials([p1, p2]).to_arrow().to_pydict()
            single = TraceViewer([g1, g2]).flamegraph().to_arrow().to_pydict()

            def totals(d):
                return {n: d["total"][i] for i, n in enumerate(d["name"])}

            assert totals(merged) == totals(single)
            # A folds across both ranks: 100 + 50.
            assert totals(merged)["A"] == 150

    def test_session_fuses_flamegraph_with_agg(self):
        # One scan feeds a group_by agg AND a flamegraph branch.
        with Environment(lines=1) as env:
            rows = [
                {"ph": "X", "name": n, "cat": "posix", "pid": 1, "tid": 1, "ts": t, "dur": d}
                for n, t, d in [("A", 0, 100), ("B", 10, 30), ("C", 15, 10)]
            ]
            gz = _make_trace(env, "sess_tree.pfw.gz", rows)
            with TraceViewer(gz).session() as s:
                agg = s.view().group_by("cat").agg("count").collect()
                fg = s.view().flamegraph()
                ct = s.view().call_tree()
            adict = agg.result().to_arrow().to_pydict()
            assert int(adict["count"][0]) == 3
            fdict = fg.result().to_arrow().to_pydict()
            node = {n: i for i, n in enumerate(fdict["name"])}
            assert fdict["total"][node["A"]] == 100 and fdict["self"][node["A"]] == 70
            assert fdict["total"][node["B"]] == 30
            cdict = ct.result().to_arrow().to_pydict()
            order = {n: i for i, n in enumerate(cdict["name"])}
            assert cdict["parent_id"][order["B"]] == order["A"]
            assert cdict["level"][order["C"]] == 2

    def test_containment_schemaless_arg_field(self):
        # Lanes and interval come from arg fields, not the schema: partition by
        # arg "rank", interval from arg "begin"/"span".
        with Environment(lines=1) as env:
            rows = [
                {
                    "ph": "X",
                    "name": n,
                    "cat": "c",
                    "pid": 1,
                    "tid": 1,
                    "ts": 0,
                    "dur": 1,
                    "args": {"rank": r, "begin": b, "span": s},
                }
                for n, r, b, s in [("A", 0, 0, 100), ("B", 0, 10, 30), ("X", 1, 0, 100)]
            ]
            gz = _make_trace(env, "argtree.pfw.gz", rows)
            ct = (
                TraceViewer(gz)
                .call_tree(partition=["rank"], ts="begin", dur="span")
                .to_arrow()
                .to_pydict()
            )
            order = {n: i for i, n in enumerate(ct["name"])}
            # B nests under A (same rank lane); X is a root in its own rank lane.
            assert ct["parent_id"][order["B"]] == order["A"]
            assert ct["level"][order["B"]] == 1
            assert ct["parent_id"][order["X"]] == -1

    def test_statistics_summary(self):
        with Environment(lines=150) as env:
            gz = _indexed(env)
            stats = TraceViewer(gz).statistics()
            assert stats["duration_count"] == 150
            assert stats["duration_stddev_us"] >= 0.0
            assert stats["max_timestamp_us"] >= stats["min_timestamp_us"]

    def test_filter_narrows_events(self):
        with Environment(lines=200) as env:
            gz = _indexed(env)
            full = TraceViewer(gz).statistics()["duration_count"]
            stdio = TraceViewer(gz).filter('cat == "STDIO"').statistics()["duration_count"]
            assert 0 < stdio < full

    def test_materialize_row_view_and_reuse(self, tmp_path):
        import os

        with Environment(lines=200) as env:
            gz = _indexed(env)
            mv_root = str(tmp_path / "mv")

            def stdio_view():
                return TraceViewer(gz).filter('cat == "STDIO"').views_root(mv_root)

            def stdio_count():
                v = stdio_view()
                return sum(c.num_rows for c in v.stream(batch_size=128))

            base = stdio_count()
            base_stats = stdio_view().statistics()
            assert base > 0
            assert stdio_view().mv_source() == []  # nothing materialized yet

            # Build the filtered-trace MV with a progress callback.
            ticks = []
            stdio_view().materialize(progress=lambda done, total: ticks.append((done, total)))
            parts = [
                f
                for _root, _dirs, files in os.walk(mv_root)
                for f in files
                if f.endswith(".pfw.gz")
            ]
            assert parts, "materialize should write at least one part file"
            assert ticks and ticks[-1][0] == ticks[-1][1]  # progress reached 100%

            # Observability: a matching query now reports the MV as its source.
            assert stdio_view().mv_source(), "query should be served from the MV"

            # A matching stream and statistics() both stay correct off the MV.
            assert stdio_count() == base
            assert stdio_view().statistics()["duration_count"] == base_stats["duration_count"]

    def test_stream_yields_all_events_as_dataframes(self):
        with Environment(lines=500) as env:
            gz = _indexed(env)
            chunks = list(TraceViewer(gz).stream(batch_size=128))
            assert chunks, "stream produced no chunks"
            # Each chunk is a native DataFrame; concat natively, Arrow at edge.
            tbl = _concat(chunks)
            assert tbl.num_rows == 500
            assert {"name", "cat", "ts", "dur"}.issubset(tbl.column_names)

    def test_stream_respects_filter(self):
        with Environment(lines=400) as env:
            gz = _indexed(env)
            tbl = _concat(TraceViewer(gz).filter('cat == "STDIO"').stream())
            assert 0 < tbl.num_rows < 400
            assert set(tbl.column("cat").to_pylist()) == {"STDIO"}

    def test_export_events_gzip_reindexes(self, tmp_path):
        with Environment(lines=300) as env:
            gz = _indexed(env)
            out = str(tmp_path / "events.pfw.gz")
            TraceViewer(gz).filter('cat == "STDIO"').export_trace(out)
            # The exported trace is a valid, re-indexable dftracer trace.
            with dftu_utils.Indexer(files=[out], index_dir=str(tmp_path)) as ix:
                ix.ensure_indexed()
            reread = TraceViewer(out, index_path=str(tmp_path)).statistics()
            assert 0 < reread["duration_count"] < 300

    def test_export_aggregation_gzip(self, tmp_path):
        with Environment(lines=200) as env:
            gz = _indexed(env)
            out = str(tmp_path / "agg.pfw.gz")
            TraceViewer(gz).group_by("cat").time_bucket(1000).agg("count").export_trace(out)
            with gzip.open(out, "rt") as fh:
                events = [json.loads(line) for line in fh if line.strip()]
            assert events and all(e.get("ph") == 2 for e in events)  # ph=C counters

    def test_export_plain_ndjson(self, tmp_path):
        with Environment(lines=100) as env:
            gz = _indexed(env)
            out = str(tmp_path / "plain.pfw")
            TraceViewer(gz).export_trace(out, compress=False)
            lines = [line for line in open(out) if line.strip()]
            assert len(lines) == 100

    def test_stream_select_pushdown(self):
        with Environment(lines=300) as env:
            gz = _indexed(env)
            tbl = _concat(TraceViewer(gz).select("ts", "dur").stream())
            assert set(tbl.column_names) == {"ts", "dur"}
            assert tbl.num_rows == 300

    def test_stream_small_batches_cover_all_rows(self):
        with Environment(lines=600) as env:
            gz = _indexed(env)
            # Small batches split the scan into several chunks; every row is
            # covered exactly once across them.
            chunks = list(TraceViewer(gz).stream(batch_size=64))
            assert len(chunks) > 1
            assert sum(c.num_rows for c in chunks) == 600
            tbl = _concat(chunks)
            assert pa.types.is_string(tbl.schema.field("cat").type)

    def test_aggregate_partial_merge_matches_collect(self):
        """Distributed partial+merge equals a single collect (incl. mean/std)."""
        with Environment(lines=300) as env:
            files = [env.create_test_gzip_file(f"f{k}/t{k}.pfw.gz") for k in range(3)]
            with dftu_utils.Indexer(files=files, index_dir=env.temp_dir) as ix:
                ix.ensure_indexed()

            def view(fs):
                return (
                    dftu_utils.TraceViewer(fs, index_path=env.temp_dir)
                    .group_by("cat")
                    .agg("count", "mean:dur", "std:dur")
                )

            whole = pa.table(view(files).collect().collect()).sort_by("cat")
            partials = [view([f]).aggregate_partial() for f in files]
            for b in partials:
                assert isinstance(b, bytes) and b
            merged = pa.table(view([]).merge_partials_to_table(partials)).sort_by("cat")

            assert whole.column("count").to_pylist() == merged.column("count").to_pylist()
            for col in ("mean_dur", "std_dur"):
                for a, b in zip(whole.column(col).to_pylist(), merged.column(col).to_pylist()):
                    assert abs(a - b) < 1e-6

    def test_materialize_rollup_then_collect_reads_it(self):
        with Environment(lines=300) as env:
            gz = _indexed(env)
            order = [("cat", "ascending"), ("time_bucket", "ascending")]

            def view():
                return (
                    TraceViewer(gz)
                    .rollup_root(env.temp_dir + "/vc")
                    .group_by("cat")
                    .time_bucket(1000)
                    .agg("count", "mean:dur")
                )

            fresh = pa.table(view().collect().collect()).sort_by(order)
            assert view().reconstruct_if_cached() is None  # cold
            view().materialize()  # build the rollup
            warm = view().reconstruct_if_cached()
            assert warm is not None
            # reconstruct and a plain collect (auto-reads the rollup) both match.
            assert (
                pa.table(warm)
                .sort_by(order)
                .to_pandas()
                .round(6)
                .equals(fresh.to_pandas().round(6))
            )
            assert (
                pa.table(view().collect().collect())
                .sort_by(order)
                .to_pandas()
                .round(6)
                .equals(fresh.to_pandas().round(6))
            )

    def test_cache_terminals_gated_to_aggregation(self):
        """group_by/agg promote TraceViewer -> AggregatedTraceViewer, which alone
        carries the cache terminals; a raw TraceViewer cannot reach them."""
        import pytest

        from dftracer.utils import AggregatedTraceViewer

        with Environment(lines=100) as env:
            gz = _indexed(env)
            base = TraceViewer(gz)
            assert not hasattr(base, "materialize_partials")
            assert not hasattr(base.filter("cat == POSIX"), "reconstruct_if_cached")
            assert isinstance(base.group_by("cat"), AggregatedTraceViewer)
            assert isinstance(base.agg("count"), AggregatedTraceViewer)
            # Aggregation stays through a further op, and only it has the rollup.
            agg = base.group_by("cat").time_bucket(1000)
            assert isinstance(agg, AggregatedTraceViewer)
            assert hasattr(agg, "materialize_partials")
            # Base collect() takes no cache kwarg.
            with pytest.raises(TypeError):
                base.collect(cache=True)

    def test_io_cat_group_and_sumsq(self, tmp_path):
        """group_by io_cat classifies by function; sumsq:dur == sum of dur^2."""
        gz = str(tmp_path / "io.pfw.gz")
        specs = [("read", 2, 10), ("write", 3, 20), ("open", 1, 5)]  # io_cat 1,2,3
        with gzip.open(gz, "wt") as f:
            for name, n, dur in specs:
                for i in range(n):
                    f.write(
                        '{"ph":"X","name":"%s","cat":"POSIX","pid":1,"tid":1,'
                        '"ts":%d,"dur":%d,"args":{}}\n' % (name, 1000 + i, dur)
                    )
        with dftu_utils.Indexer(files=[gz], index_dir=str(tmp_path)) as ix:
            ix.ensure_indexed()
        df = (
            pa.table(
                dftu_utils.TraceViewer(gz, index_path=str(tmp_path))
                .group_by("io_cat")
                .agg("count", "sumsq:dur")
                .collect()
                .collect()
            )
            .to_pandas()
            .set_index("io_cat")
        )
        # io_cat comes back as the stringified IOCategory int (read=1/write=2/meta=3).
        assert df.loc["1", "count"] == 2 and df.loc["1", "sumsq_dur"] == 2 * 10**2
        assert df.loc["2", "count"] == 3 and df.loc["2", "sumsq_dur"] == 3 * 20**2
        assert df.loc["3", "count"] == 1 and df.loc["3", "sumsq_dur"] == 1 * 5**2

    def test_session_fuses_branches_with_parity(self, tmp_path):
        """A session runs several aggregation branches over one scan; each branch
        matches its standalone collect(), and per-branch filters/rank work."""
        gz = str(tmp_path / "sess.pfw.gz")
        with gzip.open(gz, "wt") as f:
            f.write(
                '{"ph":"M","name":"PR","cat":"dftracer","pid":100,"tid":0,'
                '"args":{"name":"rank","value":"0"}}\n'
            )
            f.write(
                '{"ph":"M","name":"PR","cat":"dftracer","pid":200,"tid":0,'
                '"args":{"name":"rank","value":"1"}}\n'
            )
            for i in range(3):
                f.write(
                    '{"ph":"X","name":"read","cat":"POSIX","pid":100,"tid":1,'
                    '"ts":%d,"dur":5,"args":{}}\n' % (1000 + i)
                )
            for i in range(2):
                f.write(
                    '{"ph":"X","name":"write","cat":"STDIO","pid":200,"tid":1,'
                    '"ts":%d,"dur":7,"args":{}}\n' % (2000 + i)
                )
        with dftu_utils.Indexer(files=[gz], index_dir=str(tmp_path)) as ix:
            ix.ensure_indexed()
        out = str(tmp_path / "posix.pfw")
        tv = dftu_utils.TraceViewer(gz, index_path=str(tmp_path))
        with tv.session() as s:
            by_cat = s.view().group_by("cat").agg("count", "mean:dur").collect()
            by_rank = s.view().group_by("rank").agg("count").collect()
            posix = s.view().filter('cat == "POSIX"').group_by("cat").agg("count").collect()
            exported = s.view().filter('cat == "POSIX"').export(out)

        cat = pa.table(by_cat.result()).to_pandas().set_index("cat")
        rank = pa.table(by_rank.result()).to_pandas().set_index("rank")
        pox = pa.table(posix.result()).to_pandas().set_index("cat")

        assert cat.loc["posix", "count"] == 3 and cat.loc["stdio", "count"] == 2
        # rank resolves the pid groups via the PR metadata harvested in the scan.
        assert rank.loc["0", "count"] == 3 and rank.loc["1", "count"] == 2
        # per-branch filter is independent of the other branches.
        assert list(pox.index) == ["posix"] and pox.loc["posix", "count"] == 3
        # the export branch streamed just its matching events over the same scan.
        assert exported.result()["events_matched"] == 3
        lines = [json.loads(line) for line in open(out) if line.strip()]
        assert len(lines) == 3 and all(e["cat"] == "POSIX" for e in lines)

        # per-branch sort_by/limit apply to that branch only.
        with tv.session() as s2:
            top = s2.view().group_by("cat").agg("count").sort_by("count", True).limit(1).collect()
        top_df = pa.table(top.result()).to_pandas()
        assert len(top_df) == 1 and top_df.iloc[0]["cat"] == "posix"

        # statistics branch matches the standalone TraceViewer.statistics().
        with tv.session() as s3:
            allst = s3.view().statistics()
            poxst = s3.view().filter('cat == "POSIX"').statistics()
        assert allst.result() == tv.statistics()
        assert poxst.result()["duration_count"] == 3

        # join / compare of two collect branches over the one scan.
        with tv.session() as s5:
            a = s5.view().group_by("cat").agg("count").collect()
            b = s5.view().filter('cat == "POSIX"').group_by("cat").agg("count").collect()
            j = s5.join(a, b, "left")
            c = s5.compare(a, b)
        jdf = pa.table(j.result()).to_pandas().set_index("cat")
        assert sorted(jdf.columns) == ["l_count", "r_count"]
        assert jdf.loc["posix", "l_count"] == 3 and jdf.loc["posix", "r_count"] == 3
        cdf = pa.table(c.result()).to_pandas().set_index("cat")
        assert {"l_count", "r_count", "delta_count", "pct_count"} <= set(cdf.columns)
        assert (
            cdf.loc["stdio", "r_count"] != cdf.loc["stdio", "r_count"]
        )  # NaN: stdio absent in variant

        # default (inner) join drops stdio (absent on the posix-filtered side);
        # exercises the native n_key inference with no explicit key width.
        with tv.session() as s7:
            ai = s7.view().group_by("cat").agg("count").collect()
            bi = s7.view().filter('cat == "POSIX"').group_by("cat").agg("count").collect()
            ji = s7.join(ai, bi)
        assert list(pa.table(ji.result()).to_pandas()["cat"]) == ["posix"]

        # aggregate_partial branch: a raw serialized partial that merges back to
        # the same table a direct collect produces.
        with tv.session() as s6:
            p = s6.view().group_by("cat").agg("count").aggregate_partial()
        part = p.result()
        assert isinstance(part, bytes) and len(part) > 0
        merged = (
            pa.table(tv.group_by("cat").agg("count").merge_partials_to_table([part]))
            .to_pandas()
            .set_index("cat")
        )
        assert merged["count"].to_dict() == {"posix": 3, "stdio": 2}

        # events (raw events -> DataFrame) and stream (chunks) fuse with collect.
        with tv.session() as s4:
            ev = s4.view().events()
            evp = s4.view().filter('cat == "POSIX"').select("cat", "dur").events()
            stc = s4.view().stream(batch_size=2)
        edf = pa.table(ev.result()).to_pandas()
        assert len(edf) == 5 and "ts" in edf.columns
        pdf = pa.table(evp.result()).to_pandas()
        assert len(pdf) == 3 and sorted(pdf.columns) == ["cat", "dur"]
        assert (pdf["cat"] == "POSIX").all()
        chunks = list(stc.result())
        assert [c.num_rows for c in chunks] == [2, 2, 1]

        # Parity: the fused branch equals the standalone aggregation.
        standalone = (
            pa.table(tv.group_by("cat").agg("count", "mean:dur").collect().collect())
            .to_pandas()
            .set_index("cat")
            .sort_index()
        )
        assert standalone["count"].to_dict() == cat.sort_index()["count"].to_dict()

    def test_session_result_triggers_lazy_execute(self, tmp_path):
        """Reading a Handle before an explicit execute() runs the shared scan."""
        gz = str(tmp_path / "e.pfw.gz")
        with gzip.open(gz, "wt") as f:
            f.write(
                '{"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,'
                '"ts":1000,"dur":5,"args":{}}\n'
            )
        with dftu_utils.Indexer(files=[gz], index_dir=str(tmp_path)) as ix:
            ix.ensure_indexed()
        tv = dftu_utils.TraceViewer(gz, index_path=str(tmp_path))
        s = tv.session()
        h = s.view().group_by("cat").agg("count").collect()
        # no explicit execute(): the first result() triggers the shared scan.
        assert pa.table(h.result()).num_rows == 1

    def test_group_by_rank_resolves_pid_via_pr_metadata(self, tmp_path):
        """group_by("rank") harvests pid -> rank from PR metadata at query time."""
        gz = str(tmp_path / "rank.pfw.gz")
        with gzip.open(gz, "wt") as f:
            f.write(
                '{"ph":"M","name":"PR","cat":"dftracer","pid":100,"tid":0,'
                '"args":{"name":"rank","value":"0"}}\n'
            )
            f.write(
                '{"ph":"M","name":"PR","cat":"dftracer","pid":200,"tid":0,'
                '"args":{"name":"rank","value":"1"}}\n'
            )
            for i in range(3):
                f.write(
                    '{"ph":"X","name":"read","cat":"POSIX","pid":100,"tid":1,'
                    '"ts":%d,"dur":5,"args":{}}\n' % (1000 + i)
                )
            for i in range(2):
                f.write(
                    '{"ph":"X","name":"read","cat":"POSIX","pid":200,"tid":1,'
                    '"ts":%d,"dur":5,"args":{}}\n' % (2000 + i)
                )
        with dftu_utils.Indexer(files=[gz], index_dir=str(tmp_path)) as ix:
            ix.ensure_indexed()
        df = (
            pa.table(
                dftu_utils.TraceViewer(gz, index_path=str(tmp_path))
                .group_by("rank")
                .agg("count")
                .collect()
                .collect()
            )
            .to_pandas()
            .set_index("rank")
        )
        assert df.loc["0", "count"] == 3
        assert df.loc["1", "count"] == 2

    def test_size_derived_from_ret_for_io(self, tmp_path):
        """For POSIX read/write, ret is the byte count, so size == sum(ret)."""
        gz = str(tmp_path / "io.pfw.gz")
        with gzip.open(gz, "wt") as f:
            for i in range(100):
                f.write(
                    '{"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,'
                    '"ts":%d,"dur":%d,"args":{"ret":%d}}\n' % (1000 + i, 10 + i, 10 * (i + 1))
                )
        with dftu_utils.Indexer(files=[gz], index_dir=str(tmp_path)) as ix:
            ix.ensure_indexed()
        tbl = pa.table(
            dftu_utils.TraceViewer(gz, index_path=str(tmp_path))
            .group_by("cat")
            .agg("sum:size")
            .collect()
            .collect()
        )
        assert tbl.column("sum_size").to_pylist()[0] == 10 * (100 * 101 // 2)

    def test_auto_numeric_args_surfaces_size(self, tmp_path):
        gz = str(tmp_path / "sz.pfw.gz")
        with gzip.open(gz, "wt") as f:
            for i in range(100):
                f.write(
                    '{"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,'
                    '"ts":%d,"dur":%d,"args":{"size":%d}}\n' % (1000 + i, 10 + i, 100 * (i + 1))
                )
        with dftu_utils.Indexer(files=[gz], index_dir=str(tmp_path)) as ix:
            ix.ensure_indexed()
        tbl = pa.table(
            dftu_utils.TraceViewer(gz, index_path=str(tmp_path))
            .group_by("cat")
            .agg("count")
            .agg_numeric_args()
            .collect()
            .collect()
        )
        assert "size" in tbl.column_names

    def test_enums_compose_with_builder(self):
        from dftracer.utils import AggOp, GroupKey, Phase

        assert Phase.EVENTS == "events"
        assert GroupKey.CAT == "cat"
        assert GroupKey.arg("epoch") == "arg:epoch"
        assert AggOp.STD.of("dur") == "std:dur"
        assert AggOp.ARGMAX.of("name", by="dur") == "argmax:name:dur"
        assert AggOp.COUNT.of("") == "count"
        with Environment(lines=100) as env:
            gz = _indexed(env)
            tbl = pa.table(
                dftu_utils.TraceViewer(gz)
                .phase(Phase.EVENTS)
                .group_by(GroupKey.CAT)
                .agg(AggOp.COUNT.of(""), AggOp.MEAN.of("dur"))
                .collect()
                .collect()
            )
            assert {"cat", "count", "mean_dur"}.issubset(tbl.column_names)

    def test_time_unit_normalizes_from_source(self, tmp_path):
        """A trace declared in SEC normalizes ts/dur to the target unit."""
        from dftracer.utils import TimeUnit

        gz = str(tmp_path / "sec.pfw.gz")
        with gzip.open(gz, "wt") as f:
            f.write(
                '{"ph":"M","name":"CM","pid":0,"tid":0,'
                '"args":{"name":"time_metric","value":"SEC"}}\n'
            )
            for i in range(50):
                f.write(
                    '{"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,'
                    '"ts":%d,"dur":2,"args":{}}\n' % (100 + i)
                )
        with dftu_utils.Indexer(files=[gz], index_dir=str(tmp_path)) as ix:
            ix.ensure_indexed()

        tv = dftu_utils.TraceViewer(gz, index_path=str(tmp_path))
        native = pa.table(tv.group_by("cat").agg("mean:dur").collect().collect())
        assert native.column("mean_dur").to_pylist()[0] == 2.0  # seconds

        us = pa.table(tv.time_unit(TimeUnit.US).group_by("cat").agg("mean:dur").collect().collect())
        assert us.column("mean_dur").to_pylist()[0] == 2_000_000.0  # 2 s in us

        strm = _concat(tv.time_unit("us").select("ts", "dur").stream())
        durs = [x for x in strm.column("dur").to_pylist() if x is not None]
        assert set(durs) == {2_000_000} and len(durs) == 50

    def test_builder_ops_are_immutable(self):
        with Environment(lines=50) as env:
            gz = _indexed(env)
            base = TraceViewer(gz)
            narrowed = base.filter('cat == "STDIO"')
            # base is unchanged by the derived view's filter.
            assert base.statistics()["duration_count"] == 50
            assert narrowed.statistics()["duration_count"] < 50

    def test_group_by_resolves_any_field_and_nested_paths(self):
        with Environment(lines=1) as env:
            rows = [
                {
                    "ph": "X",
                    "name": "op",
                    "cat": "POSIX",
                    "type": typ,
                    "pid": 1,
                    "tid": 1,
                    "ts": ts,
                    "dur": 10,
                    "args": {"meta": {"host": host}, "tags": [tag0, "y"], "n": {"v": v}},
                }
                for typ, host, tag0, v, ts in [
                    ("c_app", "A", "x", 7, 1000),
                    ("c_app", "A", "x", 7, 1100),
                    ("posix", "B", "z", 9, 1200),
                    ("posix", "B", "z", 9, 1300),
                ]
            ]
            gz = _make_trace(env, "schemaless.pfw.gz", rows)

            def counts(col, key):
                t = pa.table(TraceViewer(gz).group_by(key).agg("count").collect().collect())
                return {
                    k: int(v)
                    for k, v in zip(t.column(col).to_pylist(), t.column("count").to_pylist())
                }

            # Top-level "type" (not a POD scalar) and nested/indexed args resolve
            # the same way filter and select do.
            assert counts("type", "type") == {"c_app": 2, "posix": 2}
            assert counts("args.meta.host", "args.meta.host") == {"A": 2, "B": 2}
            assert counts("args.tags[0]", "args.tags[0]") == {"x": 2, "z": 2}
            assert counts("args.tags.0", "args.tags.0") == {"x": 2, "z": 2}

            t = pa.table(
                TraceViewer(gz).group_by("args.meta.host").agg("mean:args.n.v").collect().collect()
            )
            m = {
                k: v
                for k, v in zip(
                    t.column("args.meta.host").to_pylist(),
                    t.column("mean_args.n.v").to_pylist(),
                )
            }
            assert m["A"] == 7.0 and m["B"] == 9.0

    def test_top_level_field_is_not_shadowed_by_same_named_arg(self):
        with Environment(lines=1) as env:
            rows = [
                {
                    "ph": "X",
                    "name": "kv",
                    "cat": "MPI",
                    "type": "mpi",  # top-level schema field
                    "pid": 1,
                    "tid": 1,
                    "ts": 1000 + i,
                    "dur": 5,
                    "args": {"type": i + 1},  # unrelated same-named param
                }
                for i in range(4)
            ] + [
                {
                    "ph": "X",
                    "name": "send",
                    "cat": "MPI",
                    "type": "mpi",
                    "pid": 1,
                    "tid": 1,
                    "ts": 2000 + i,
                    "dur": 5,
                    "args": {"count": 10},
                }
                for i in range(6)
            ]
            gz = _make_trace(env, "shadow.pfw.gz", rows)

            def counts(col, key):
                t = pa.table(TraceViewer(gz).group_by(key).agg("count").collect().collect())
                return {
                    k: int(v)
                    for k, v in zip(t.column(col).to_pylist(), t.column("count").to_pylist())
                }

            # Bare "type" is the top-level field, never the args param.
            by_type = counts("type", "type")
            assert by_type == {"mpi": 10}
            # The param is still reachable through the explicit args path.
            assert counts("args.type", "args.type") == {"1": 1, "2": 1, "3": 1, "4": 1, "": 6}

    def test_occupancy_invariants_and_cell_knob(self):
        with Environment(lines=1) as env:
            rows = [
                {
                    "ph": "X",
                    "name": name,
                    "cat": "POSIX",
                    "pid": 1,
                    "tid": 1,
                    "ts": ts,
                    "dur": dur,
                    "args": {},
                }
                for name, ts, dur in [
                    ("overlap", 1000, 100),  # union 150 < sum 200
                    ("overlap", 1050, 100),
                    ("serial", 1000, 100),  # disjoint, union 200
                    ("serial", 2000, 100),
                ]
            ]
            gz = _make_trace(env, "occ.pfw.gz", rows)
            t = pa.table(
                TraceViewer(gz)
                .time_range(900, 2200)
                .occ_cell(5)
                .group_by("name")
                .agg("count", "sum:dur", "busy", "concurrency", "utilization")
                .collect()
                .collect()
            )
            assert "busy_cell_us" in t.column_names
            cols = {c: t.column(c).to_pylist() for c in t.column_names}
            row = {name: i for i, name in enumerate(cols["name"])}

            def g(name, col):
                return cols[col][row[name]]

            assert int(g("overlap", "busy_cell_us")) == 5
            # overlap: busy clamps to the makespan (150); serial: to sum(dur).
            assert int(g("overlap", "sum_dur")) == 200
            assert int(g("overlap", "busy")) == 150
            assert abs(g("overlap", "concurrency") - 200 / 150) < 1e-9
            assert abs(g("overlap", "utilization") - 1.0) < 1e-9
            assert int(g("serial", "busy")) == 200
            assert abs(g("serial", "concurrency") - 1.0) < 1e-9
            # Invariants hold everywhere a coverage bitmap can only overshoot.
            for i in range(t.num_rows):
                assert cols["busy"][i] <= cols["sum_dur"][i]
                assert cols["concurrency"][i] >= 1.0 - 1e-9
                assert cols["utilization"][i] <= 1.0 + 1e-9


class TestTraceViewerSchema:
    """columns() / schema() read the harvested column set from the index with
    no trace scan, schemaless over arbitrarily nested args."""

    def _rows(self):
        return [
            {
                "ph": "X",
                "name": "read",
                "cat": "POSIX",
                "pid": 1,
                "tid": 2,
                "ts": 100,
                "dur": 5,
                "args": {
                    "hostname": "h1",
                    "size": 1024,
                    "rate": 3.5,
                    "pos": {"x": 1, "y": 2},
                    "tags": ["a", "b"],
                    "fhash": "fh1",
                    "hhash": "hh1",
                },
            },
            # A second event name carries size as a float, so the type folds
            # (int64 + float64 -> float64) across names.
            {
                "ph": "X",
                "name": "write",
                "cat": "POSIX",
                "pid": 1,
                "tid": 2,
                "ts": 200,
                "dur": 6,
                "args": {"size": 2.5},
            },
        ]

    def test_columns_lists_base_args_and_nested_leaves(self):
        with Environment() as env:
            path = _make_trace(env, "schema.pfw.gz", self._rows())
            cols = set(TraceViewer(path).columns())
            for c in [
                "pid",
                "tid",
                "ts",
                "dur",  # base axis
                "name",
                "cat",  # top-level
                "hostname",
                "size",
                "rate",  # flat args
                "pos.x",
                "pos.y",
                "tags.0",  # nested object + array leaf
                "fhash",
                "hhash",  # lifted hashes
                "resolved.fpath",
                "resolved.hostname",  # aliases
            ]:
                assert c in cols, c

    def test_schema_reports_types(self):
        with Environment() as env:
            path = _make_trace(env, "schema.pfw.gz", self._rows())
            sch = TraceViewer(path).schema()
            assert sch["pid"] == "int64"
            assert sch["ts"] == "int64"
            assert sch["hostname"] == "string"
            assert sch["rate"] == "float64"
            assert sch["pos.x"] == "int64"
            assert sch["tags.0"] == "string"
            assert sch["size"] == "float64"  # int in read, float in write
            assert sch["resolved.fpath"] == "string"


class TestFlamegraphGroup:
    """flamegraph(group=...) roots the tree by an arbitrary field over the raw
    events (not just pid), distinct from the aggregation group_by."""

    def _rows(self):
        return [
            {
                "ph": "X",
                "name": "read",
                "cat": "POSIX",
                "pid": 1,
                "tid": 1,
                "ts": 0,
                "dur": 10,
                "args": {},
            },
            {
                "ph": "X",
                "name": "write",
                "cat": "STDIO",
                "pid": 2,
                "tid": 1,
                "ts": 20,
                "dur": 10,
                "args": {},
            },
        ]

    def test_group_by_cat_roots_by_cat(self):
        with Environment() as env:
            path = _make_trace(env, "fg.pfw.gz", self._rows())
            fg = TraceViewer(path).flamegraph(group=["cat"]).to_pandas()
            names = set(fg["name"])
            # cat-valued root nodes appear...
            assert "POSIX" in names
            assert "STDIO" in names

    def test_ungrouped_has_no_cat_nodes(self):
        with Environment() as env:
            path = _make_trace(env, "fg.pfw.gz", self._rows())
            fg = TraceViewer(path).flamegraph().to_pandas()
            names = set(fg["name"])
            assert "POSIX" not in names  # no group rooting -> only call names
            assert "read" in names and "write" in names


class TestSessionPerBranchPhase:
    """A fused session must honor each branch's phase(): an events() branch and
    an aggregated() branch ride one scan, each filtered to its own phase. Aggregated
    (ph=3) events must be reachable this way (they were dropped before)."""

    def _rows(self):
        rows = [
            {
                "ph": 1,
                "name": "read",
                "cat": "POSIX",
                "pid": 1,
                "tid": 1,
                "ts": 100 + i,
                "dur": 10,
                "args": {},
            }
            for i in range(3)
        ]
        rows += [
            {
                "name": "write",
                "cat": "POSIX",
                "ts": 1000 + i,
                "ph": 3,
                "type": 3,
                "pid": 1,
                "tid": 1,
                "args": {"hhash": "h1", "dur": 250},
            }
            for i in range(4)
        ]
        return rows

    def _ph(self, df):
        return list(df.to_arrow().to_pydict().get("ph"))

    def test_per_branch_phase_in_one_session(self):
        with Environment() as env:
            path = _make_trace(env, "mixed_phase.pfw.gz", self._rows())
            with TraceViewer(path).session() as s:
                a = s.view().phase("events").events()
                b = s.view().phase("aggregated").events()
                c = s.view().events()
            assert self._ph(a.result()) == [1, 1, 1]  # complete only
            assert self._ph(b.result()) == [3, 3, 3, 3]  # aggregated only
            assert sorted(self._ph(c.result())) == [1, 1, 1, 3, 3, 3, 3]  # all


class TestTimeBucketGroupByDedup:
    """Listing "time_bucket" in group_by after time_bucket() must not double the
    auto-added bucket key (which used to emit an empty column)."""

    def _rows(self):
        return [
            {
                "ph": 1,
                "name": "read",
                "cat": "POSIX",
                "pid": 1,
                "tid": 1,
                "ts": 1000 + t,
                "dur": 5,
                "args": {},
            }
            for t in (0, 50, 130)
        ]

    def test_time_bucket_in_group_by_is_deduped(self):
        with Environment() as env:
            path = _make_trace(env, "tb.pfw.gz", self._rows())
            base = TraceViewer(path).time_bucket(100, normalize_to=1000)
            with_key = (
                base.group_by("pid", "time_bucket")
                .agg("count")
                .collect()
                .collect()
                .to_arrow()
                .to_pydict()
            )
            without = base.group_by("pid").agg("count").collect().collect().to_arrow().to_pydict()
            # same result either way, and the bucket column has real values
            assert with_key == without
            assert all(v != "" for v in with_key["time_bucket"])
            assert sorted(with_key["time_bucket"]) == ["1000", "1100"]
