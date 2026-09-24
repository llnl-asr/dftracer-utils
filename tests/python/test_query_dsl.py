"""Query DSL (Field/Expr) serialization and TraceViewer.filter(Expr) wiring."""

import gzip
import os

import pytest

from dftracer.utils import Field, resolved
from dftracer.utils.query import Expr


def test_comparisons():
    assert str(Field("cat") == "POSIX") == 'cat == "POSIX"'
    assert str(Field("dur") > 1000) == "dur > 1000"
    assert str(Field("dur") <= 5) == "dur <= 5"
    assert str(Field("ok") == True) == "ok == true"  # noqa: E712


def test_boolean_composition():
    q = (Field("cat") == "POSIX") & (Field("dur") > 1000)
    assert str(q) == '(cat == "POSIX" and dur > 1000)'
    q = (Field("cat") == "POSIX") | (Field("cat") == "STDIO")
    assert str(q) == '(cat == "POSIX" or cat == "STDIO")'
    assert str(~(Field("cat") == "POSIX")) == 'not (cat == "POSIX")'


def test_in_and_not_in():
    assert str(Field("cat").is_in(["POSIX", "STDIO"])) == 'cat in ["POSIX", "STDIO"]'
    assert str(Field("cat").not_in(["MPI"])) == 'cat not in ["MPI"]'


def test_string_match_ops():
    assert str(Field("name").like("%read%")) == 'name like "%read%"'
    assert str(Field("name").ilike("READ")) == 'name ilike "READ"'
    assert str(Field("name").regex("^p?read$")) == 'name ~ "^p?read$"'
    assert str(Field("name").iregex("READ")) == 'name ~* "READ"'
    # Substring is case-sensitive (pandas / polars), so it serializes as an
    # escaped LIKE; the DSL's literal-first `"sub" in field` is the
    # case-insensitive form.
    assert str(Field("args.file").contains("tmp")) == 'args.file like "%tmp%"'
    assert str(Field("args.file").contains("a%b_c", case=True)) == 'args.file like "%a\\%b\\_c%"'
    assert str(Field("args.file").contains("tmp", case=False)) == '"tmp" in args.file'
    assert str(Field("name").starts_with("pre")) == 'name like "pre%"'
    assert str(Field("name").ends_with("64")) == 'name like "%64"'
    with pytest.raises(TypeError, match="not an index-pushable"):
        Field("name").fullmatch("x").to_query()
    with pytest.raises(TypeError, match="not an index-pushable"):
        Field("name").lower().like("x").to_query()


def test_resolved_fields():
    assert str(resolved("hostname") == "node01") == 'resolved.hostname == "node01"'
    assert str(resolved("fpath").like("%/scratch/%")) == ('resolved.fpath like "%/scratch/%"')
    with pytest.raises(ValueError):
        resolved("not_a_field")


def test_nested_field_path():
    assert str(Field("args.level") == "DEBUG") == 'args.level == "DEBUG"'


def test_flat_dotted_arg_key_serializes():
    # A flat arg key that itself contains dots ("cqe.raw_ns") serializes as
    # written, bare or with the explicit args. prefix.
    assert str(Field("cqe.raw_ns") > 100) == "cqe.raw_ns > 100"
    assert str(Field("args.cqe.raw_ns") > 100) == "args.cqe.raw_ns > 100"


def _write_trace(path):
    with gzip.open(path, "wt") as f:
        for i in range(20):
            name = ["read", "write"][i % 2]
            cat = ["POSIX", "STDIO"][i % 2]
            f.write(
                '{"ph":"X","name":"%s","cat":"%s","pid":1,"tid":1,'
                '"ts":%d,"dur":%d,"args":{}}\n' % (name, cat, 1000 + i, 5 + i)
            )


def _write_dotted_arg_trace(path):
    # Half the events carry a large cqe.raw_ns; op distinguishes the two groups.
    # Both arg keys are flat members whose names contain dots.
    with gzip.open(path, "wt") as f:
        for i in range(20):
            op = "SEND" if i % 2 == 0 else "RECV"
            raw = 100 if i % 2 == 0 else 200
            f.write(
                '{"ph":"X","name":"rdma","cat":"ib","pid":1,"tid":1,'
                '"ts":%d,"dur":5,"args":{"cqe.raw_ns":%d,"mlx5.op":"%s"}}\n' % (1000 + i, raw, op)
            )


def test_flat_dotted_arg_key_resolves_at_runtime(tmp_path):
    """A flat arg key that contains dots resolves through the native query and
    group-by paths, bare or args.-prefixed, identically."""
    pa = pytest.importorskip("pyarrow")
    from dftracer.utils import AggregationConfig, Indexer, TraceViewer

    trace = os.path.join(tmp_path, "t.pfw.gz")
    _write_dotted_arg_trace(trace)
    idx = os.path.join(tmp_path, "idx")
    with Indexer(
        directory=str(tmp_path),
        index_dir=idx,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()

    def count(pred):
        tbl = (
            TraceViewer(str(tmp_path), index_path=idx)
            .phase("events")
            .filter(pred)
            .group_by("mlx5.op")
            .agg("count")
            .collect()
        )
        df = pa.table(tbl).to_pandas()
        return int(df["count"].sum())

    # Group-by a flat dotted key returns both groups.
    all_ops = count(Field("cat") == "ib")
    assert all_ops == 20
    # Filter on the flat dotted key, bare and prefixed, agree and select half.
    assert count(Field("cqe.raw_ns") > 150) == 10
    assert count(Field("args.cqe.raw_ns") > 150) == 10


def test_traceviewer_filter_accepts_expr(tmp_path):
    """filter()/query() accept an Expr, producing the same result as its str()."""
    pa = pytest.importorskip("pyarrow")
    from dftracer.utils import AggregationConfig, Indexer, TraceViewer

    trace = os.path.join(tmp_path, "t.pfw.gz")
    _write_trace(trace)
    idx = os.path.join(tmp_path, "idx")
    with Indexer(
        directory=str(tmp_path),
        index_dir=idx,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()

    expr = Field("cat") == "POSIX"
    assert isinstance(expr, Expr)

    def count(pred):
        tbl = (
            TraceViewer(str(tmp_path), index_path=idx)
            .phase("events")
            .filter(pred)
            .group_by("cat")
            .agg("count")
            .collect()
        )
        df = pa.table(tbl).to_pandas()
        return int(df["count"].sum())

    # Passing the Expr must match passing its serialized string.
    assert count(expr) == count(str(expr)) == 10


def test_unified_f_is_one_object():
    """The two historical `F`s (columnar value builder, query predicate builder)
    are now the same object, exported from every entry point."""
    from dftracer.utils import F as top_f
    from dftracer.utils import Field as top_field
    from dftracer.utils import resolved as top_resolved
    from dftracer.utils.columnar import F as columnar_f
    from dftracer.utils.query import F as query_f
    from dftracer.utils.query import Field as query_field
    from dftracer.utils.query import Value, resolved

    assert top_f is columnar_f is query_f
    assert top_field is query_field
    assert top_resolved is resolved
    assert Value is not None
    # Field("dur") and F.dur are the same field leaf.
    assert isinstance(query_field("dur"), Expr)
    assert str(query_field("dur") == 1) == str(top_f.dur == 1)


def test_unified_f_value_and_predicate():
    """One F builds an in-memory value/mask via .apply and a pushdown predicate
    via .to_query()/str()."""
    pa = pytest.importorskip("pyarrow")
    from dftracer.utils import F
    from dftracer.utils.dataframe import _dataframe_from_arrow

    df = _dataframe_from_arrow(pa.table({"dur": pa.array([1, 5, 9], pa.int64())}))
    assert (F.dur * 2).apply(df).to_arrow().to_pylist() == [2, 10, 18]
    assert (F.dur > 4).apply(df).to_arrow().to_pylist() == [False, True, True]
    assert (F.dur > 4).to_query() == "dur > 4"
    assert str(F.cat == "POSIX") == 'cat == "POSIX"'


def test_unified_predicate_methods_serialize():
    from dftracer.utils import F, resolved

    assert str(F.cat.like("%io%")) == 'cat like "%io%"'
    assert str(F.cat.is_in(["POSIX", "STDIO"])) == 'cat in ["POSIX", "STDIO"]'
    assert str(F.cat.eq("io")) == 'cat == "io"'
    assert str(resolved("fpath").like("%/scratch/%")) == 'resolved.fpath like "%/scratch/%"'
    assert str((F.cat == "POSIX") & (F.dur > 1000)) == '(cat == "POSIX" and dur > 1000)'


def test_non_pushable_predicate_raises():
    from dftracer.utils import F

    with pytest.raises(TypeError, match="not an index-pushable predicate"):
        ((F.a + F.b) > 3).to_query()


def test_string_predicates_apply_in_memory():
    """String match, membership and string == run through the engine's string
    kernels; only iregex and a bool comparison stay filter-only."""
    pa = pytest.importorskip("pyarrow")
    from dftracer.utils import F
    from dftracer.utils.dataframe import _dataframe_from_arrow

    df = _dataframe_from_arrow(
        pa.table({"cat": pa.array(["io", "cpu", "IO", "Disk io"], pa.string())})
    )

    def mask(expr):
        return expr.apply(df).to_arrow().to_pylist()

    assert mask(F.cat.like("%io%")) == [True, False, False, True]
    assert mask(F.cat.ilike("%io%")) == [True, False, True, True]
    assert mask(F.cat == "io") == [True, False, False, False]
    assert mask(F.cat != "io") == [False, True, True, True]
    assert mask(F.cat.contains("io")) == [True, False, False, True]
    assert mask(F.cat.contains("IO", case=False)) == [True, False, True, True]
    assert mask(F.cat.starts_with("c")) == [False, True, False, False]
    assert mask(F.cat.ends_with("o")) == [True, False, False, True]
    assert mask(F.cat.regex("^[a-z]+$")) == [True, True, False, False]
    assert mask(F.cat.regex("io")) == [True, False, False, True]
    assert mask(F.cat.fullmatch("io")) == [True, False, False, False]
    assert mask(F.cat.is_in(["io", "IO"])) == [True, False, True, False]
    assert mask(F.cat.not_in(["io", "IO"])) == [False, True, False, True]
    assert mask(F.cat.lower().is_in(["io"])) == [True, False, True, False]
    with pytest.raises(TypeError):
        F.cat.iregex("io").apply(df)
    with pytest.raises(TypeError):
        (F.cat > "a").apply(df)
    with pytest.raises(TypeError):
        F.cat.is_in([1, "a"])


def test_string_values_apply_in_memory():
    pa = pytest.importorskip("pyarrow")
    from dftracer.utils import F
    from dftracer.utils.dataframe import _dataframe_from_arrow

    df = _dataframe_from_arrow(pa.table({"s": pa.array([" Read ", "pread64"], pa.string())}))

    def vals(expr):
        return expr.apply(df).to_arrow().to_pylist()

    assert vals(F.s.upper()) == [" READ ", "PREAD64"]
    assert vals(F.s.lower()) == [" read ", "pread64"]
    assert vals(F.s.strip()) == ["Read", "pread64"]
    assert vals(F.s.lstrip()) == ["Read ", "pread64"]
    assert vals(F.s.rstrip()) == [" Read", "pread64"]
    assert vals(F.s.len_bytes()) == [6, 7]
    assert vals(F.s.len_chars()) == [6, 7]
    assert vals(F.s.find("ead")) == [2, 2]
    assert vals(F.s.replace("e", "E")) == [" REad ", "prEad64"]
    assert vals(F.s.replace_all("e", "E")) == [" REad ", "prEad64"]
    assert vals(F.s.slice(1, 3)) == ["Rea", "rea"]
    assert vals(F.s.strip().len_bytes() + 1) == [5, 8]


def test_traceviewer_filter_unified_predicates(tmp_path):
    """filter() accepts unified-F predicates (comparison, membership, like,
    resolved); a non-pushable one filters the plan's rows instead."""
    pa = pytest.importorskip("pyarrow")
    from dftracer.utils import AggregationConfig, F, Indexer, TraceViewer, resolved

    trace = os.path.join(tmp_path, "t.pfw.gz")
    _write_trace(trace)
    idx = os.path.join(tmp_path, "idx")
    with Indexer(
        directory=str(tmp_path),
        index_dir=idx,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()

    def count(pred):
        tbl = (
            TraceViewer(str(tmp_path), index_path=idx)
            .phase("events")
            .filter(pred)
            .group_by("cat")
            .agg("count")
            .collect()
        )
        return int(pa.table(tbl).to_pandas()["count"].sum())

    assert count(F.cat.is_in(["POSIX"])) == count('cat in ["POSIX"]') == 10
    assert count(F.dur > 0) == count("dur > 0")

    # filter accepts like/resolved predicates (built and pushed down).
    v = TraceViewer(str(tmp_path), index_path=idx).phase("events").filter(F.cat.like("POS%"))
    assert isinstance(v, TraceViewer)
    v2 = TraceViewer(str(tmp_path), index_path=idx).filter(resolved("hostname") == "n01")
    assert isinstance(v2, TraceViewer)

    # A non-pushable predicate filters the collected rows, not the index.
    events = TraceViewer(str(tmp_path), index_path=idx).phase("events")
    v3 = events.filter((F.dur + F.ts) > 1005)
    assert isinstance(v3, TraceViewer)
    every = pa.table(events.collect()).to_pandas()
    kept = pa.table(v3.collect()).to_pandas()
    expected = every[every["dur"] + every["ts"] > 1005]
    assert 0 < len(kept) < len(every)
    assert sorted(kept["ts"]) == sorted(expected["ts"])
