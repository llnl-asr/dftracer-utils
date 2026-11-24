#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test cases for TraceViewer over dftu.table files."""

import gzip
import json
import math
import os
import shutil
import subprocess
import sys
import textwrap

import pytest

np = pytest.importorskip("numpy")
pd = pytest.importorskip("pandas")
pa = pytest.importorskip("pyarrow")

import dftracer.utils as dftu  # noqa: E402
from dftracer.utils import DataFrame, col  # noqa: E402
from dftracer.utils import dftracer_utils_ext as ext  # noqa: E402
from dftracer.utils.index import Indexer  # noqa: E402
from dftracer.utils.trace import TraceViewer, discover_trace_files  # noqa: E402

from .test_table_io import EQUAL, load_table, matrix, save_table, sets_equal_nan  # noqa: E402

REFUSED = [
    ("phase", ("any",)),
    ("time_range", (0, 10)),
    ("time_bucket", (1000,)),
    ("resolution", (1,)),
    ("time_unit", ("us",)),
    ("time_scale", (1.0,)),
    ("flamegraph", ()),
    ("call_tree", ()),
    ("containment", ()),
    ("index_records", ()),
    ("statistics", ()),
    ("materialize", ()),
    ("duql", ("where i > 1",)),
    ("memory_budget", (1 << 20,)),
]


def counters(reset=True):
    return ext._table_read_counters(reset)


def edge_frame(n=200_000, nulls=True):
    """Rows whose columns hold the values a prune could get wrong."""
    i = np.arange(n, dtype="int64")
    f = pd.array(i / 3.0, dtype="Float64")
    if nulls:
        f[::7] = pd.NA
    f[3::1000] = math.nan
    f[5::997] = math.inf
    s = pd.array(
        [f"k{x % 100}" if x % 11 or not nulls else f"n{x % 7}" for x in range(n)], dtype="object"
    )
    s = pd.Series(s, dtype=object)
    s[7::5000] = "é中文"
    s[9::5003] = ""
    return pd.DataFrame({"i": i, "f": f, "s": s})


def same(a, b):
    pd.testing.assert_frame_equal(
        a.reset_index(drop=True), b.reset_index(drop=True), check_dtype=False, rtol=0
    )


def test_a_table_reads_back_equal_on_the_full_matrix(tmp_path):
    df = matrix()
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    tv = TraceViewer(path)
    frame = tv.collect()
    assert isinstance(frame, DataFrame)
    exact = frame.to_pandas()
    keep = [c for c in df.columns if c not in ("sets", "lists")]
    pd.testing.assert_frame_equal(exact[keep], df[keep], **EQUAL)
    sets_equal_nan(exact["sets"].tolist(), df["sets"].tolist())
    sets_equal_nan(exact["lists"].tolist(), df["lists"].tolist())
    assert list(exact.dtypes.astype(str)) == list(df.dtypes.astype(str))
    assert list(load_table(path).to_pandas().columns) == list(df.columns)
    assert tv.columns == list(df.columns)
    assert set(tv.schema) == set(df.columns)


def test_a_table_with_an_index_keeps_it(tmp_path):
    df = pd.DataFrame(
        {"v": [1, 2, 3]},
        index=pd.MultiIndex.from_arrays([[0, 1, 2], ["a", "b", "c"]], names=[None, "lvl"]),
    )
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    frame = TraceViewer(path).collect()
    assert frame.index_names() == ["__index_level_0__", "lvl"]
    pd.testing.assert_frame_equal(frame.to_pandas(), df, **EQUAL)


def test_a_zero_row_table_keeps_its_columns(tmp_path):
    df = pd.DataFrame({"i": pd.Series([], dtype="int64"), "s": pd.Series([], dtype=object)})
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    frame = TraceViewer(path).collect()
    assert frame.num_rows == 0 and frame.columns == ["i", "s"]


@pytest.mark.parametrize("rows", [0, 3])
def test_a_table_with_a_string_and_an_int_index_level_restores_its_index(tmp_path, rows):
    df = pd.DataFrame(
        {
            "name": pd.Series(["a", "b", None][:rows], dtype="string"),
            "bin": pd.Series([3, 1, 3][:rows], dtype="int64"),
            "x": pd.Series([1.5, 2.5, 3.5][:rows], dtype="float64"),
        }
    ).set_index(["name", "bin"])
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    got = TraceViewer(path).collect().to_pandas()
    pd.testing.assert_frame_equal(got, df)
    assert [str(level.dtype) for level in got.index.levels] == ["string", "int64"]


def test_a_folder_and_a_list_read_the_parts_in_part_number_order(tmp_path):
    parts = []
    folder = tmp_path / "parts"
    folder.mkdir()
    for n in (0, 1, 2, 9, 10, 11):
        df = pd.DataFrame(
            {"i": [n, n], "c": pd.Categorical([f"c{n}", "z"], categories=[f"c{n}", "z"])}
        )
        save_table(df, str(folder / f"part-{n:05d}.pfw.gz"), name="t")
        parts.append(df)
    want = [0, 0, 1, 1, 2, 2, 9, 9, 10, 10, 11, 11]
    by_folder = TraceViewer(str(folder)).collect()
    assert by_folder["i"].to_list() == want
    # What a glob yields is a list of files, sorted by name; the viewer orders it.
    listed = sorted(str(p) for p in folder.iterdir())
    assert TraceViewer(listed).collect()["i"].to_list() == want
    names = [str(folder / f"part-{n:05d}.pfw.gz") for n in (11, 2, 0)]
    by_list = TraceViewer(names).collect()
    assert by_list["i"].to_list() == [0, 0, 2, 2, 11, 11]
    exact = by_folder.to_pandas()
    assert list(exact["c"].cat.categories) == ["c0", "z", "c1", "c2", "c9", "c10", "c11"]
    assert exact["c"].tolist()[:4] == ["c0", "z", "c1", "z"]


def test_unnumbered_names_in_a_folder_keep_their_given_order(tmp_path):
    a, b = tmp_path / "b.pfw.gz", tmp_path / "a.pfw.gz"
    save_table(pd.DataFrame({"i": [1]}), str(a), name="t")
    save_table(pd.DataFrame({"i": [2]}), str(b), name="t")
    assert TraceViewer([str(a), str(b)]).collect()["i"].to_list() == [1, 2]


def test_parts_that_disagree_raise_before_any_row(tmp_path):
    save_table(pd.DataFrame({"a": [1]}), str(tmp_path / "part-0.pfw.gz"), name="t")
    save_table(pd.DataFrame({"a": [1.5]}), str(tmp_path / "part-1.pfw.gz"), name="t")
    with pytest.raises(ValueError, match="differ"):
        TraceViewer(str(tmp_path))


def test_tables_listed_with_traces_are_left_out(tmp_path):
    table = str(tmp_path / "t.pfw.gz")
    save_table(pd.DataFrame({"a": [1]}), table, name="t")
    trace = write_trace(tmp_path / "x.pfw.gz", 1, 1)
    view = TraceViewer([table, trace])
    assert not ext.TraceViewer.is_table(view)
    assert view.collect().to_pandas()["name"].tolist() == ["read"]


def test_dask_tasks_of_one_file_leave_out_tables_listed_with_traces(tmp_path):
    pytest.importorskip("dask")
    from dftracer.utils.dask import DaskTraceViewer

    table = str(tmp_path / "t.pfw.gz")
    save_table(pd.DataFrame({"a": [1]}), table, name="t")
    trace = write_trace(tmp_path / "x.pfw.gz", 1, 1)
    assert DaskTraceViewer([table, trace])._shards() == [[trace]]
    assert DaskTraceViewer([table])._shards() == [[table]]


def test_a_claimed_header_that_is_wrong_raises_and_never_reads_as_a_trace(tmp_path):
    path = str(tmp_path / "t.pfw")
    save_table(pd.DataFrame({"a": [1]}), path, name="t")
    text = open(path).read()
    bad = tmp_path / "v.pfw"
    bad.write_text(text.replace('"format_version":"1.0"', '"format_version":"2.0"', 1))
    assert bad.read_text() != text
    with pytest.raises(ext.DFTUtilsError, match="format_version"):
        TraceViewer(str(bad))
    cut = tmp_path / "c.pfw"
    cut.write_text(text.split("\n", 1)[0][:60] + "\n")
    with pytest.raises(ext.DFTUtilsError, match="dftu.table"):
        TraceViewer(str(cut))
    notice = tmp_path / "n.pfw"
    notice.write_text(text.replace('"ph":4', '"ph":3', 1))
    with pytest.raises(ext.DFTUtilsError, match="ph 4"):
        TraceViewer(str(notice))


@pytest.mark.parametrize("name,args", REFUSED)
def test_trace_ops_are_refused_on_a_table(tmp_path, name, args):
    path = str(tmp_path / "t.pfw.gz")
    save_table(pd.DataFrame({"i": [1, 2, 3]}), path, name="t")
    tv = TraceViewer(path)
    with pytest.raises(ext.DFTUtilsValueError, match=name):
        getattr(tv, name)(*args)


def test_the_table_ops_run(tmp_path):
    df = pd.DataFrame(
        {"i": np.arange(10), "g": ["a", "b"] * 5, "c": pd.Categorical(["x", "y"] * 5)}
    )
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    tv = TraceViewer(path)
    assert tv.filter(col("i") > 6).collect()["i"].to_list() == [7, 8, 9]
    assert tv.select("i").columns == ["i"]
    assert tv.drop("g").columns == ["i", "c"]
    assert tv.rename({"i": "j"}).columns == ["j", "g", "c"]
    assert tv.sort_by("i", descending=True).head(2).collect()["i"].to_list() == [9, 8]
    assert tv.slice(2, 3).collect()["i"].to_list() == [2, 3, 4]
    assert tv.with_columns(k=col("i") + 1).collect()["k"].to_list() == list(range(1, 11))
    got = tv.group_by("g").agg(col("i").sum().alias("s")).collect()
    assert sorted(zip(got["g"].to_list(), got["s"].to_list())) == [("a", 20), ("b", 25)]
    assert tv.lazy().collect().num_rows == 10
    with pytest.raises(ext.DFTUtilsValueError):
        tv.filter("i > 3")


def test_collect_carries_a_header_for_the_result_only_while_columns_are_the_tables(tmp_path):
    df = pd.DataFrame(
        {
            "i": np.arange(8, dtype="int8"),
            "c": pd.Categorical(["a", "b"] * 4, categories=["a", "b", "z"]),
            "n": pd.array([1, None] * 4, dtype="Int32"),
        }
    )
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    tv = TraceViewer(path)
    out = tv.filter(col("i") > 4).select("n", "c").collect()
    assert out._table_header["nrows"] == 3
    assert [f["name"] for f in out._table_header["fields"]] == ["n", "c"]
    pd.testing.assert_frame_equal(
        out.to_pandas(), df[df["i"] > 4][["n", "c"]].reset_index(drop=True), **EQUAL
    )
    derived = tv.with_columns(k=col("i") + 1).collect()
    assert getattr(derived, "_table_header", None) is None
    assert getattr(tv.rename({"i": "j"}).collect(), "_table_header", None) is None
    assert (
        getattr(tv.group_by("c").agg(col("i").sum().alias("s")).collect(), "_table_header", None)
        is None
    )


def predicates():
    return [
        ("i == 12345", col("i") == 12345, lambda d: d.i == 12345),
        ("i range", (col("i") >= 1000) & (col("i") < 1100), lambda d: (d.i >= 1000) & (d.i < 1100)),
        ("i in", col("i").is_in([5, 70000, 150000]), lambda d: d.i.isin([5, 70000, 150000])),
        ("i vs float", col("i") == 77.0, lambda d: d.i == 77),
        ("f gt", col("f") > 66000.0, lambda d: d.f > 66000.0),
        ("f eq int", col("f") == 3, lambda d: d.f == 3),
        ("s eq", col("s") == "k45", lambda d: d.s == "k45"),
        ("s multibyte", col("s") == "é中文", lambda d: d.s == "é中文"),
        ("s empty", col("s") == "", lambda d: d.s == ""),
        ("or", (col("i") < 3) | (col("i") > 199990), lambda d: (d.i < 3) | (d.i > 199990)),
        ("not equal", col("i") != 5, lambda d: d.i != 5),
        ("no match", col("i") > 10**9, lambda d: d.i > 10**9),
    ]


@pytest.fixture(scope="module", params=[True, False], ids=["nulls", "no-nulls"])
def edge_table(request, tmp_path_factory):
    root = tmp_path_factory.mktemp("edge")
    df = edge_frame(nulls=request.param)
    path = str(root / "t.pfw.gz")
    save_table(df, path, name="t")
    plain = root / "copy"
    plain.mkdir()
    shutil.copy(path, plain / "t.pfw.gz")
    return df, path, str(plain / "t.pfw.gz")


def test_indexed_results_equal_full_scan_results_on_edge_values(request, edge_table):
    df, path, copy = edge_table
    Indexer(path).build()
    indexed, scanned = TraceViewer(path), TraceViewer(copy)
    for label, expr, mask in predicates():
        counters()
        got = indexed.filter(expr).collect().to_pandas()
        used = counters()
        full = scanned.filter(expr).collect().to_pandas()
        assert counters()["indexed_files"] == 0, label
        # A != cannot skip a chunk, so it reads the file whole.
        assert used["indexed_files"] == (0 if label == "not equal" else 1), label
        want = df[mask(df).fillna(False).astype(bool)]
        same(got, full)
        same(got, want)


@pytest.fixture(scope="module")
def big_int_table(tmp_path_factory):
    root = tmp_path_factory.mktemp("bigint")
    n = 300_000
    i = np.arange(n, dtype="int64")
    df = pd.DataFrame(
        {
            "i": i,
            "near53": i + (2**53 - 150_000),
            "near62": i * 1000 + 2**62,
            "near63": pd.array(
                np.arange(n, dtype="uint64") + np.uint64(2**63 - 150_000), dtype="UInt64"
            ),
        }
    )
    path = str(root / "t.pfw.gz")
    save_table(df, path, name="t")
    plain = root / "copy"
    plain.mkdir()
    shutil.copy(path, plain / "t.pfw.gz")
    Indexer(path).build()
    return df, path, str(plain / "t.pfw.gz")


def big_int_predicates():
    two53 = 9007199254740992.0
    c = col("near53")
    yield "near53==2^53", c == two53
    yield "near53>2^53", c > two53
    yield "near53>=2^53", c >= two53
    yield "near53<2^53", c < two53
    yield "near53<=2^53", c <= two53
    yield "near53 inside a chunk", c > 9007199254741142.0
    yield "near53 below a chunk", c < 9007199254740893.0
    yield "near62>f", col("near62") > 4.6e18 + 1e8
    yield "near62<=2^62", col("near62") <= float(2**62)
    yield "near62>=last", col("near62") >= float(2**62 + 1000 * 299_999)
    yield "near63<f", col("near63") < 9.223372036854775e18
    yield "near63>=2^63", col("near63") >= 9.223372036854775808e18
    yield "near63>2^63", col("near63") > 9.223372036854775808e18
    yield "near63==neg", col("near63") == -1.0
    yield "near53 and near62", (c > 9007199254741142.0) & (col("near62") < 4.7e18)


def test_a_float_literal_against_large_integers_prunes_exactly(big_int_table):
    df, path, copy = big_int_table
    indexed, scanned = TraceViewer(path), TraceViewer(copy)
    for label, expr in big_int_predicates():
        counters()
        got = indexed.filter(expr).collect().to_pandas()
        counters()
        full = scanned.filter(expr).collect().to_pandas()
        assert list(got["i"]) == list(full["i"]), label


@pytest.fixture(scope="module")
def full_table(tmp_path_factory):
    """Rows with a value in every column: the index can prove the file clean."""
    root = tmp_path_factory.mktemp("full")
    n = 200_000
    df = pd.DataFrame(
        {
            "i": np.arange(n, dtype="int64"),
            "f": np.arange(n) / 3.0,
            "s": [f"k{x % 100}" for x in range(n)],
        }
    )
    path = str(root / "t.pfw.gz")
    save_table(df, path, name="t")
    return df, path, None


def test_a_selective_filter_inflates_fewer_chunks_than_a_full_scan(full_table):
    _, path, _ = full_table
    Indexer(path).build()
    tv = TraceViewer(path)
    counters()
    tv.filter(col("i") == 12345).collect()
    one = counters()
    assert one["indexed_files"] == 1
    assert one["chunks_skipped"] > 0 and one["chunks_read"] >= 1
    total = one["chunks_read"] + one["chunks_skipped"]
    assert one["chunks_read"] < total
    tv.filter(col("i") >= 0).collect()
    every = counters()
    # The header member holds no rows, so even a keep-everything filter skips it.
    assert every["chunks_read"] == total - 1 and every["chunks_skipped"] == 1


def test_a_stale_missing_or_other_index_scans(tmp_path, full_table):
    df, source, _ = full_table
    path = str(tmp_path / "t.pfw.gz")
    shutil.copy(source, path)
    expr = col("i") == 12345
    # no index
    counters()
    assert TraceViewer(path).filter(expr).collect().num_rows == 1
    assert counters()["indexed_files"] == 0
    Indexer(path).build()
    assert TraceViewer(path).filter(expr).collect().num_rows == 1
    assert counters()["indexed_files"] == 1
    # an index of another directory
    other = str(tmp_path / "idx")
    counters()
    assert TraceViewer(path, index_dir=other).filter(expr).collect().num_rows == 1
    assert counters()["indexed_files"] == 0
    # the file changes under its index: the stale index must not be believed
    text = gzip.open(path, "rt").read().replace('"i":12345,', '"i":-1,', 1)
    with gzip.open(path, "wt", compresslevel=1) as f:
        f.write(text)
    os.utime(path, (1_900_000_000, 1_900_000_000))
    counters()
    assert TraceViewer(path).filter(expr).collect().num_rows == 0
    assert TraceViewer(path).filter(col("i") == -1).collect().num_rows == 1
    assert counters()["indexed_files"] == 0


def test_a_filtered_read_streams_in_bounded_memory(tmp_path):
    n = 1_200_000
    df = pd.DataFrame(
        {"i": np.arange(n), "f": np.arange(n) / 7.0, "s": [f"value-{x % 977}" for x in range(n)]}
    )
    path = str(tmp_path / "big.pfw.gz")
    save_table(df, path, name="t")
    script = textwrap.dedent(
        """
        import resource, sys
        from dftracer.utils import col
        from dftracer.utils.trace import TraceViewer
        mode, path = sys.argv[1:3]
        tv = TraceViewer(path)
        if mode == "filter":
            out = tv.filter(col("i") == 777).collect()
        else:
            out = tv.collect()
        # On Linux ru_maxrss also holds the peak of the process that spawned
        # this one, so read this image's own high-water mark.
        if sys.platform == "darwin":
            peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        else:
            with open("/proc/self/status") as status:
                peak = next(int(l.split()[1]) for l in status if l.startswith("VmHWM:")) * 1024
        print(out.num_rows, peak)
        """
    )

    def run(mode):
        env = dict(os.environ)
        out = subprocess.run(
            [sys.executable, "-I", "-c", script, mode, path],
            capture_output=True,
            text=True,
            check=True,
            env=env,
        ).stdout.split()[-2:]
        return int(out[0]), int(out[1])

    rows, small = run("filter")
    whole, big = run("all")
    assert rows == 1 and whole == n
    assert small < big * 0.6, (small, big)


def test_an_unfiltered_read_uses_the_direct_decode(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    save_table(pd.DataFrame({"i": np.arange(1000)}), path, name="t")
    counters()
    TraceViewer(path).collect()
    got = counters()
    assert got["direct_files"] == 1 and got["streamed_files"] == 0
    TraceViewer(path).filter(col("i") > 3).collect()
    got = counters()
    assert got["streamed_files"] == 1 and got["direct_files"] == 0


def test_a_viewer_collects_the_frame_of_a_table(tmp_path):
    df = matrix()
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    a = load_table(path)
    b = TraceViewer(path).collect()
    assert a._table_header == b._table_header
    assert json.dumps(a.to_dict(), default=str) == json.dumps(b.to_dict(), default=str)


def test_a_filter_that_keeps_no_row_keeps_the_columns(tmp_path):
    df = pd.DataFrame({"i": np.arange(5), "s": list("abcde")})
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    out = TraceViewer(path).filter(col("i") > 100).collect()
    assert out.num_rows == 0 and out.columns == ["i", "s"]
    pd.testing.assert_frame_equal(out.to_pandas(), df.iloc[:0].reset_index(drop=True), **EQUAL)


def test_an_empty_result_keeps_its_typed_columns_on_every_terminal(tmp_path):
    df = pd.DataFrame({"i": np.arange(50), "s": [f"v{x}" for x in range(50)]})
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    none = col("i") > 10**6
    plan = TraceViewer(path).filter(none)
    for batch in (0, 4):
        assert plan.lazy().collect(batch).columns == ["i", "s"], batch
        assert TraceViewer(path).filter(none).collect(batch).columns == ["i", "s"], batch
    assert dftu.collect_all([plan.lazy()])[0].columns == ["i", "s"]
    assert plan.lazy().collect_async().get().columns == ["i", "s"]
    assert list(plan.lazy().stream()) == []
    mem = dftu.DataFrame.from_dict({"a": [1, 2, 3], "s": ["x", "y", "z"]}).lazy()
    for batch in (0, 4):
        assert mem.filter(col("a") > 100).collect(batch).columns == ["a", "s"], batch
    assert dftu.collect_all([mem.filter(col("a") > 100)])[0].columns == ["a", "s"]
    assert mem.filter(col("a") > 100).collect_async().get().columns == ["a", "s"]


def _write_lines(path, header_rows, lines):
    head = {
        "name": "dftu.table",
        "cat": "dftu",
        "pid": 0,
        "tid": 0,
        "ts": 0,
        "ph": 4,
        "args": {
            "format": "dftu.table",
            "format_version": 1,
            "table": "t",
            "nrows": header_rows,
            "index": [],
            "index_names": [],
            "fields": [{"name": "i", "dtype": "int64"}, {"name": "s", "dtype": "object"}],
            "meta": {},
        },
    }
    body = [json.dumps(head)] + lines
    with gzip.open(path, "wt") as f:
        f.write("\n".join(body) + "\n")


def _row(i, s='"a"'):
    return '{"name":"t","cat":"dftu","ts":%d,"ph":3,"pid":0,"tid":0,"args":{"i":%d,"s":%s}}' % (
        i,
        i,
        s,
    )


def test_a_projected_read_converts_only_the_selected_columns(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    _write_lines(path, 3, [_row(1), _row(2, "5"), _row(3)])
    assert list(TraceViewer(path).select("i").collect().to_pandas()["i"]) == [1, 2, 3]
    with pytest.raises(Exception, match="not a string"):
        TraceViewer(path).collect()
    with pytest.raises(Exception, match="not a string"):
        TraceViewer(path).select("s").collect()


@pytest.mark.parametrize("indexed", [False, True])
@pytest.mark.parametrize("shape", ["all", "select", "filter", "count"])
def test_every_line_is_checked_as_json_in_every_mode(tmp_path, indexed, shape):
    path = str(tmp_path / "t.pfw.gz")
    _write_lines(path, 3, [_row(1), "this is not json", _row(3)])
    if indexed:
        Indexer(path).build()
    tv = TraceViewer(path)
    reads = {
        "all": lambda: tv.collect(),
        "select": lambda: tv.select("i").collect(),
        "filter": lambda: tv.filter(col("i") >= 0).collect(),
        "count": lambda: (
            tv.group_by("i").agg(dftu.col("i").count().alias("n")).select("n").head(1).collect()
        ),
    }
    with pytest.raises(Exception, match="(?i)json|row"):
        reads[shape]()


@pytest.mark.parametrize("indexed", [False, True])
def test_a_whole_read_counts_rows_against_the_header(tmp_path, indexed):
    path = str(tmp_path / "t.pfw.gz")
    _write_lines(path, 5, [_row(1), _row(2), _row(3)])
    if indexed:
        Indexer(path).build()
    with pytest.raises(Exception, match="says 5 rows"):
        TraceViewer(path).filter(col("i") >= 0).collect()


def _compressible_table(path, rows, member_rows=50_000):
    row = _row(1) + "\n"
    head = {
        "name": "dftu.table",
        "cat": "dftu",
        "pid": 0,
        "tid": 0,
        "ts": 0,
        "ph": 4,
        "args": {
            "format": "dftu.table",
            "format_version": 1,
            "table": "t",
            "nrows": rows,
            "index": [],
            "index_names": [],
            "fields": [{"name": "i", "dtype": "int64"}, {"name": "s", "dtype": "object"}],
            "meta": {},
        },
    }
    import zlib

    def member(text):
        c = zlib.compressobj(1, zlib.DEFLATED, 31)
        return c.compress(text.encode()) + c.flush()

    with open(path, "wb") as out:
        out.write(member(json.dumps(head) + "\n"))
        left = rows
        while left > 0:
            n = min(left, member_rows)
            out.write(member(row * n))
            left -= n


@pytest.mark.skipif(sys.platform == "win32", reason="peak memory is read with resource")
def test_a_very_compressible_table_stays_under_the_budget(tmp_path):
    rows = 4_000_000
    path = str(tmp_path / "t.pfw.gz")
    _compressible_table(path, rows)
    assert os.path.getsize(path) < 8_000_000
    script = textwrap.dedent(
        """
        import resource, sys
        from dftracer.utils import col
        from dftracer.utils.trace import TraceViewer
        mode, path = sys.argv[1:3]
        plan = TraceViewer(path).lazy().memory_budget(128 << 20)
        if mode == "select":
            out = plan.select("i").collect()
        else:
            out = plan.filter(col("i") < 0).collect()
        # On Linux ru_maxrss also holds the peak of the process that spawned
        # this one, so read this image's own high-water mark.
        if sys.platform == "darwin":
            peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        else:
            with open("/proc/self/status") as status:
                peak = next(int(l.split()[1]) for l in status if l.startswith("VmHWM:")) * 1024
        print(out.num_rows, peak)
        """
    )
    for mode, want in (("select", rows), ("filter", 0)):
        got = subprocess.run(
            [sys.executable, "-I", "-c", script, mode, path],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.split()[-2:]
        assert int(got[0]) == want, mode
        inflated = rows * len(_row(1) + "\n")
        assert int(got[1]) < inflated // 2, (mode, got, inflated)


def _member(text):
    import zlib

    c = zlib.compressobj(1, zlib.DEFLATED, 31)
    return c.compress(text.encode()) + c.flush()


def _members_file(path, nrows, lines, fields=None):
    head = {
        "name": "dftu.table",
        "cat": "dftu",
        "pid": 0,
        "tid": 0,
        "ts": 0,
        "ph": 4,
        "args": {
            "format": "dftu.table",
            "format_version": 1,
            "table": "t",
            "nrows": nrows,
            "index": [],
            "index_names": [],
            "fields": fields or [{"name": "i", "dtype": "Int64"}, {"name": "s", "dtype": "object"}],
            "meta": {},
        },
    }
    with open(path, "wb") as out:
        out.write(_member(json.dumps(head) + "\n"))
        for line in lines:
            out.write(_member(line + "\n"))


def _outcome(make):
    try:
        return make().to_dict(), None
    except Exception as exc:
        return None, str(exc)


@pytest.mark.parametrize("damage", ["missing field", "not json", "short count"])
def test_an_indexed_read_of_a_damaged_table_matches_the_full_scan(tmp_path, damage):
    rows = [_row(1), _row(2), _row(3)]
    nrows = 3
    if damage == "missing field":
        rows[1] = '{"name":"t","cat":"dftu","ts":1,"ph":3,"pid":0,"tid":0,"args":{"s":"missing"}}'
    elif damage == "not json":
        rows[1] = "this is not json"
    else:
        nrows = 4
    path = str(tmp_path / "t.pfw.gz")
    _members_file(path, nrows, rows)
    Indexer(path).build()
    counters()
    indexed = _outcome(lambda: TraceViewer(path).filter(col("i") == 2).collect())
    scanned = _outcome(
        lambda: (
            TraceViewer(path, index_dir=str(tmp_path / "no-index")).filter(col("i") == 2).collect()
        )
    )
    assert scanned[1] is not None
    assert indexed == scanned


def test_a_table_with_many_nulls_still_prunes(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    df = edge_frame()
    assert df["f"].isna().sum() > 10_000
    save_table(df, path, name="t")
    Indexer(path).build()
    counters()
    got = TraceViewer(path).filter(col("i") == 12345).collect().to_pandas()
    used = counters()
    assert list(got["i"]) == [12345]
    assert used["indexed_files"] == 1 and used["chunks_skipped"] > 0


def test_a_clean_table_with_every_row_still_prunes_by_its_index(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    _members_file(path, 300, [_row(i) for i in range(300)])
    Indexer(path).build()
    counters()
    assert TraceViewer(path).filter(col("i") == 123).collect().num_rows == 1
    got = counters()
    assert got["indexed_files"] == 1 and got["chunks_skipped"] > 0


def _category_part(path, labels, ordered, value):
    _members_file(
        path,
        1,
        ['{"name":"t","cat":"dftu","ts":0,"ph":3,"pid":0,"tid":0,"args":{"c":"%s"}}' % value],
        fields=[{"name": "c", "dtype": "category", "ordered": ordered, "categories": labels}],
    )


def test_ordered_categories_must_have_the_same_labels_in_the_same_order(tmp_path):
    folder = tmp_path / "ordered"
    folder.mkdir()
    _category_part(str(folder / "part-0.pfw.gz"), ["a", "b"], True, "a")
    _category_part(str(folder / "part-1.pfw.gz"), ["b", "a"], True, "a")
    with pytest.raises(ValueError, match="ordered"):
        TraceViewer(str(folder))
    same = tmp_path / "same"
    same.mkdir()
    _category_part(str(same / "part-0.pfw.gz"), ["a", "b"], True, "a")
    _category_part(str(same / "part-1.pfw.gz"), ["a", "b"], True, "b")
    assert TraceViewer(str(same)).collect().num_rows == 2
    loose = tmp_path / "loose"
    loose.mkdir()
    _category_part(str(loose / "part-0.pfw.gz"), ["a", "b"], False, "a")
    _category_part(str(loose / "part-1.pfw.gz"), ["b", "c"], False, "c")
    assert TraceViewer(str(loose)).collect().num_rows == 2


def narrow_frame():
    bounds = {
        "int8": (-(2**7), 2**7 - 1),
        "int16": (-(2**15), 2**15 - 1),
        "int32": (-(2**31), 2**31 - 1),
        "uint8": (0, 2**8 - 1),
        "uint16": (0, 2**16 - 1),
        "uint32": (0, 2**32 - 1),
    }
    cols = {}
    for name, (lo, hi) in bounds.items():
        cols[name] = np.array([lo, lo + 1, 0 if lo < 0 else 1, hi - 1, hi, 3] * 5, dtype=name)
        nullable = name.replace("uint", "UInt") if name[0] == "u" else name.replace("int", "Int")
        cols["N" + nullable] = pd.array([lo, None, hi, 1, None, 2] * 5, dtype=nullable)
    cols["float32"] = np.array([0.1, -2.5, 3.4e38, 1e-45, math.nan, 7.0] * 5, dtype="float32")
    cols["Float32"] = pd.array([0.1, None, 2.5, -0.0, 4.0, 1e30] * 5, dtype="Float32")
    cols["arrow_i16"] = pd.array([1, None, 3, 4, 5, 6] * 5, dtype=pd.ArrowDtype(pa.int16()))
    return pd.DataFrame(cols)


@pytest.mark.parametrize("indexed", [False, True], ids=["scan", "indexed"])
def test_narrow_dtypes_keep_their_width_through_read_and_sink(tmp_path, indexed):
    df = narrow_frame()
    path = str(tmp_path / "n.pfw.gz")
    save_table(df, path, name="n")
    if indexed:
        Indexer(path).build()
    frame = TraceViewer(path).collect()
    want = {
        "int8": dftu.DType.INT8,
        "int16": dftu.DType.INT16,
        "int32": dftu.DType.INT32,
        "uint8": dftu.DType.UINT8,
        "uint16": dftu.DType.UINT16,
        "uint32": dftu.DType.UINT32,
        "float32": dftu.DType.FLOAT32,
        "Float32": dftu.DType.FLOAT32,
        "NInt8": dftu.DType.INT8,
        "NUInt32": dftu.DType.UINT32,
        "arrow_i16": dftu.DType.INT16,
    }
    for name, dtype in want.items():
        assert frame[name].type == dtype, name
    assert TraceViewer(path).schema["int8"] == dftu.DType.INT8
    pd.testing.assert_frame_equal(frame.to_pandas(), df, **EQUAL)
    out = str(tmp_path / "out.pfw.gz")
    TraceViewer(path).sink_table(out)
    carried = {f["name"]: f["dtype"] for f in header_fields(path)}
    written = {f["name"]: f["dtype"] for f in header_fields(out)}
    assert written == carried
    pd.testing.assert_frame_equal(TraceViewer(out).collect().to_pandas(), df, **EQUAL)


def header_fields(path):
    with gzip.open(path, "rt", encoding="utf-8") as f:
        return json.loads(f.readline())["args"]["fields"]


def test_a_value_outside_the_declared_width_is_a_decode_error(tmp_path):
    df = pd.DataFrame({"i": np.array([1, 2, 3], dtype="int8"), "u": np.array([1, 2, 3], "uint8")})
    path = tmp_path / "t.pfw"
    save_table(df, str(path), name="t")
    text = path.read_text()
    for field, bad in (("i", "300"), ("i", "-129"), ("u", "256")):
        edited = text.replace(f'"{field}":3', f'"{field}":{bad}', 1)
        assert edited != text
        broken = tmp_path / f"{field}{bad}.pfw"
        broken.write_text(edited)
        with pytest.raises(Exception, match=rf"row 2, field {field}: outside the integer dtype"):
            TraceViewer(str(broken)).collect()


@pytest.mark.parametrize("name", ["int8", "int16", "int32", "uint8", "uint16", "uint32"])
def test_filters_at_the_bounds_of_a_narrow_column_agree_indexed_and_scanned(tmp_path, name):
    df = narrow_frame()[[name]]
    plain = str(tmp_path / "plain" / "t.pfw.gz")
    idx = str(tmp_path / "idx" / "t.pfw.gz")
    save_table(df, plain, name="t")
    save_table(df, idx, name="t")
    Indexer(idx).build()
    lo, hi = int(df[name].min()), int(df[name].max())
    c = col(name)
    for predicate, mask in (
        (c == lo, df[name] == lo),
        (c == hi, df[name] == hi),
        (c < lo, df[name] < lo),
        (c > hi, df[name] > hi),
        (c >= hi, df[name] >= hi),
        (c <= lo, df[name] <= lo),
        (c == hi + 1, df[name] == hi + 1),
        (c == lo - 1, df[name] == lo - 1),
        (c.is_in([lo, hi, hi + 1]), df[name].isin([lo, hi, hi + 1])),
    ):
        scanned = TraceViewer(plain).filter(predicate).collect()
        indexed = TraceViewer(idx).filter(predicate).collect()
        assert scanned[name].to_list() == indexed[name].to_list() == df[name][mask].tolist()


def write_trace(path, pid, n):
    lines = [
        json.dumps(
            {
                "id": i,
                "name": "read",
                "cat": "POSIX",
                "pid": pid,
                "tid": 1,
                "ts": 10 * i,
                "dur": 2,
                "ph": "X",
            }
        )
        for i in range(n)
    ]
    with gzip.open(path, "wt") as f:
        f.write("\n".join(lines) + "\n")
    return str(path)


def test_tables_beside_traces_are_left_out_of_trace_reads(tmp_path):
    traces = [
        write_trace(tmp_path / "trace-1.pfw.gz", 7, 30),
        write_trace(tmp_path / "trace-2.pfw.gz", 8, 20),
    ]
    frame = DataFrame.from_pandas(pd.DataFrame({"pid": [1, 2, 3], "x": [1.5, 2.5, 3.5]}))
    tables = [str(tmp_path / "view.pfw.gz"), str(tmp_path / "flat.pfw.gz")]
    frame.write_table(tables[0], name="view")
    save_table(frame, tables[1], name="flat")

    assert discover_trace_files(tmp_path) == traces
    assert discover_trace_files(str(tmp_path / "*.pfw.gz")) == traces
    assert ext.tables_among_traces([*traces, *tables]) == [False, False, True, True]
    assert ext.tables_among_traces(tables) == [False, False]

    # A table indexed on its own shares the index root of the traces.
    Indexer(tables[1]).build()
    for files in [str(tmp_path), [*traces, *tables]]:
        assert len(TraceViewer(files).collect()) == 50
        with Indexer(files) as indexer:
            indexer.ensure()
            want = {traces[0]: {7}, traces[1]: {8}}
            assert indexer.query_file_info() == want
            assert indexer.query_all_file_pids() == want

    alone = TraceViewer(tables)
    assert ext.TraceViewer.is_table(alone)
    assert len(alone.collect()) == 6
    assert TraceViewer(tables[1]).collect().to_pandas()["x"].tolist() == [1.5, 2.5, 3.5]


def test_a_broken_table_among_traces_raises_and_is_never_read_as_a_trace(tmp_path):
    trace = write_trace(tmp_path / "trace-1.pfw.gz", 7, 3)
    table = str(tmp_path / "t.pfw")
    save_table(pd.DataFrame({"a": [1]}), table, name="t")
    text = open(table).read()
    os.remove(table)
    (tmp_path / "v.pfw.gz").write_bytes(
        gzip.compress(text.replace('"format_version":"1.0"', '"format_version":"2.0"', 1).encode())
    )
    with pytest.raises(ext.DFTUtilsError, match="format_version"):
        discover_trace_files(tmp_path)
    with pytest.raises(ext.DFTUtilsError, match="format_version"):
        TraceViewer(str(tmp_path)).collect()
    with pytest.raises(ext.DFTUtilsError, match="format_version"):
        Indexer(str(tmp_path)).ensure()
    assert trace
