# SPDX-License-Identifier: MIT
"""pandas forms a port from pandas leans on: each one gives what pandas (or
numpy) gives on the same data."""

import math
import os
import subprocess
import sys
import textwrap

import pytest

np = pytest.importorskip("numpy")
pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, DType, Series, TraceWriteOptions  # noqa: E402
from dftracer.utils.errors import DFTUtilsError, DFTUtilsTranspileError, DFTUtilsValueError  # noqa: E402
from dftracer.utils.trace import TraceViewer  # noqa: E402


def _nan_eq(a, b):
    return len(a) == len(b) and all(
        (x is None and (y is None or (isinstance(y, float) and math.isnan(y))))
        or (isinstance(x, float) and math.isnan(x) and isinstance(y, float) and math.isnan(y))
        or x == y
        for x, y in zip(a, b)
    )


def test_transpile_error_is_a_public_library_error():
    assert issubclass(DFTUtilsTranspileError, DFTUtilsValueError)
    assert issubclass(DFTUtilsTranspileError, DFTUtilsError)
    with pytest.raises(DFTUtilsError, match="constant exponent"):
        DataFrame({"a": [1]}).eval("a ** a")


def test_truth_of_a_series_is_ambiguous_as_pandas():
    for data in ([1, 2], [0], []):
        with pytest.raises(ValueError, match="ambiguous"):
            bool(Series(data))
        with pytest.raises(ValueError, match="ambiguous"):
            bool(pd.Series(data, dtype="float64"))


def test_abs_and_string_concatenation_match_pandas():
    assert abs(Series([-1, 2, None])).to_list() == [1, 2, None]
    assert abs(Series([-1.5, 2.0])).to_list() == pd.Series([-1.5, 2.0]).abs().tolist()
    s, p = Series(["a", None, "c"]), pd.Series(["a", None, "c"])
    assert _nan_eq(("app#" + s).to_list(), ("app#" + p).tolist())
    assert _nan_eq((s + "!").to_list(), (p + "!").tolist())
    assert _nan_eq((s + s).to_list(), (p + p).tolist())
    # A number column still adds numbers.
    assert (Series([1, 2]) + 1).to_list() == [2, 3]


def test_memory_usage_is_a_series_by_column():
    df = DataFrame({"a": [1, 2], "s": ["x", "yy"]})
    usage = df.memory_usage()
    assert usage.index.to_list() == ["Index", "a", "s"]
    assert usage["a"] == df["a"].nbytes and usage["Index"] == 0
    no_index = df.memory_usage(index=False, deep=True)
    assert no_index.index.to_list() == ["a", "s"]
    assert no_index.sum() == sum(df[c].nbytes for c in df.columns)
    indexed = DataFrame({"k": [1, 2], "a": [3, 4]}).set_index("k")
    assert indexed.memory_usage()["Index"] == indexed.index.nbytes


def test_drop_with_errors_ignore_skips_missing_columns():
    df, p = DataFrame({"a": [1], "b": [2]}), pd.DataFrame({"a": [1], "b": [2]})
    got = df.drop(columns=["b", "zz"], errors="ignore")
    assert got.columns == list(p.drop(columns=["b", "zz"], errors="ignore").columns)
    with pytest.raises(KeyError):
        df.drop(columns=["zz"])
    with pytest.raises(ValueError, match="errors"):
        df.drop(columns=["b"], errors="warn")


def test_items_and_keys_yield_labels():
    s = Series([1, 5, 3], index=["x", "y", "z"])
    p = pd.Series([1, 5, 3], index=["x", "y", "z"])
    assert list(s.items()) == list(p.items())
    assert list(s.nlargest(2).items()) == list(p.nlargest(2).items())
    assert s.keys() == list(p.keys())
    assert list(Series([4, 6]).items()) == list(pd.Series([4, 6]).items())
    two = DataFrame({"a": [1, 2], "b": ["x", "y"], "v": [3, 4]}).set_index(["a", "b"])["v"]
    p2 = pd.DataFrame({"a": [1, 2], "b": ["x", "y"], "v": [3, 4]}).set_index(["a", "b"])["v"]
    assert list(two.items()) == list(p2.items())
    assert Series([1], name="n", index=["q"]).name == "n"


@pytest.mark.parametrize(
    "values, divisor",
    [([1.0, 0.5, -1.0, 7.5, -7.5, 0.3], 0.1), ([5.0, -5.0, 1e300], 3.0), ([0.0, -0.0], 2.0)],
)
def test_float_floor_division_matches_numpy(values, divisor):
    got = (Series(values) // divisor).to_list()
    want = np.floor_divide(np.array(values), divisor).tolist()
    assert got == want
    assert [math.copysign(1, v) for v in got] == [math.copysign(1, v) for v in want]
    assert (Series(values) % divisor).to_list() == pytest.approx(
        np.mod(np.array(values), divisor).tolist()
    )


def test_linear_quantile_matches_numpy():
    rng = np.random.default_rng(7)
    data = [[1.0, math.inf], [-math.inf, 1.0], list(rng.normal(size=11)), [3.0, 1.0, 2.0, 4.0]]
    for values in data:
        for q in (0.0, 0.05, 0.25, 0.5, 0.7, 0.95, 1.0):
            got = Series(values).quantile(q, interpolation="linear")
            with np.errstate(invalid="ignore"):
                want = float(np.quantile(np.array(values), q, method="linear"))
            assert (math.isnan(got) and math.isnan(want)) or got == want, (values, q)
            for method in ("lower", "higher", "midpoint"):
                got = Series(values).quantile(q, interpolation=method)
                with np.errstate(invalid="ignore"):
                    want = float(np.quantile(np.array(values), q, method=method))
                assert (math.isnan(got) and math.isnan(want)) or got == want, (values, q, method)


def test_groupby_leaves_nan_keys_out_as_pandas():
    d = {"k": [1.0, math.nan, 1.0, None], "v": [1, 2, 3, 4]}
    got = DataFrame(d).groupby("k").agg(s=("v", "sum"))
    want = pd.DataFrame(d).groupby("k").agg(s=("v", "sum"))
    assert got.index.to_list() == want.index.tolist()
    assert got["s"].to_list() == want["s"].tolist()


def test_group_by_reads_leading_column_names_as_keys():
    d = {"host": ["a", "a", None, "b"], "time_range": [1, 1, 2, None], "v": [1, 2, 3, 4]}
    df = DataFrame(d)
    two = df.group_by("host", "time_range").agg(n=("v", "sum"))
    want = pd.DataFrame(d).groupby(["host", "time_range"]).agg(n=("v", "sum")).reset_index()
    assert two["host"].to_list() == want["host"].tolist()
    assert two["time_range"].to_list() == want["time_range"].tolist()
    assert two["n"].to_list() == want["n"].tolist()
    assert df.group_by("host", "time_range", "sum:v")["sum_v"].to_list() == [3]


def test_isin_takes_floats_bools_empty_lists_and_uint64():
    big = 2**63 + 1
    d = {"f": [1.5, 2.0, None], "b": [True, False, None], "i": [1, 2, 3]}
    df, p = DataFrame(d), pd.DataFrame(d).astype({"b": "boolean", "i": "int64"})
    for expr in ("f.isin([1.5, 2])", "b.isin([True])", "i.isin([])", "i.isin([2.0])"):
        assert df.eval(expr).fill_null(False).to_list() == p.eval(expr).tolist(), expr
    u = DataFrame({"u": Series([2**63, big, 1], dtype="uint64")})
    assert u.eval(f"u.isin([{big}])").to_list() == [False, True, False]
    assert u["u"].isin([big, 1]).to_list() == [False, True, True]
    assert u.query("u in [1]")["u"].to_list() == [1]
    with pytest.raises(TypeError, match="all numbers"):
        df.eval("i.isin([1, 'a'])")
    with pytest.raises(OverflowError):
        df.eval(f"i.isin([-1, {big}])")


def test_map_with_a_mapping_matches_pandas():
    s, p = Series(["a", "b", "zz", None, "a"]), pd.Series(["a", "b", "zz", None, "a"])
    m = {"a": 1, "b": 2}
    assert _nan_eq(s.map(m).to_list(), p.map(m).tolist())
    labelled = Series([1, 2], index=["p", "q"], name="v").map({2: 5})
    assert labelled.index.to_list() == ["p", "q"] and labelled.name == "v"
    assert labelled.to_list() == [None, 5]
    n = 100_000
    keys = [i % 97 for i in range(n)]
    assert Series(keys).map({k: k * 2 for k in range(50)}).to_list() == [
        k * 2 if k < 50 else None for k in keys
    ]


def test_cut_labels_and_include_lowest_match_pandas():
    x = [0.0, 0.5, 1.0, 5.0, 6.0, None]
    b = [0.0, 1.0, 5.0]
    for lowest in (False, True):
        got = Series(x).cut(Series(b), right=True, labels=["lo", "hi"], include_lowest=lowest)
        want = pd.cut(pd.Series(x), b, right=True, labels=["lo", "hi"], include_lowest=lowest)
        assert _nan_eq(got.to_list(), [None if pd.isna(v) else v for v in want.tolist()])
    codes = Series(x).cut(Series(b), right=True, labels=False)
    want = pd.cut(pd.Series(x), b, labels=False).tolist()
    assert _nan_eq(codes.to_list(), want)
    with pytest.raises(ValueError, match="one fewer"):
        Series(x).cut(Series(b), labels=["only"])


def test_named_nunique_and_median_are_exact():
    d = {
        "k": ["a", "a", "b", "b", "b", "c"],
        "v": [1, 3, 2, 2, None, None],
        "w": [1.0, 2, 3, 4, 5, 6],
    }
    spec = {"n": ("v", "nunique"), "m": ("w", "median"), "s": ("w", "sum")}
    got = DataFrame(d).groupby("k").agg(**spec)
    want = pd.DataFrame(d).groupby("k").agg(**spec)
    for c in ("n", "m", "s"):
        assert got[c].to_list() == want[c].tolist(), c
    # Past the distinct sketch's exact range: 5000 distinct values in one group.
    many = DataFrame({"k": ["a"] * 5000, "v": list(range(5000))})
    assert many.groupby("k").agg(n=("v", "nunique"))["n"].to_list() == [5000]
    whole = DataFrame(d).agg({"v": "nunique"})
    assert whole["v"].to_list() == [pd.DataFrame(d)["v"].nunique()]


def test_write_table_stores_list_columns(tmp_path):
    df = DataFrame({"a": [1, 2, 3], "tags": [["x", "y"], [], None], "n": [[1, 2], [3], [None]]})
    path = os.fspath(tmp_path / "t")
    df.write_table(path, name="t", options=TraceWriteOptions(build_index=False))
    back = TraceViewer(path).collect()
    # A native read keeps each list column as a list of its element type.
    assert back["tags"].dtype == DType.LIST
    assert back["tags"].child(0).dtype == DType.STRING
    assert back["n"].child(0).dtype == DType.INT64
    assert back["tags"].to_list() == [["x", "y"], [], None]
    assert back["n"].to_list() == [[1, 2], [3], [None]]
    restored = back.to_pandas()
    assert restored["tags"].tolist() == [["x", "y"], [], None]
    assert restored["n"].tolist() == [[1, 2], [3], [None]]
    with pytest.raises(Exception, match="NaN"):
        DataFrame({"f": [[math.nan]]}).write_table(
            os.fspath(tmp_path / "bad"), name="b", options=TraceWriteOptions(build_index=False)
        )


_TYPED_LISTS = [
    ("list<int32>", [[1, None], [], None, [-7]], DType.INT32, "int32"),
    ("list<float32>", [[1.5, None], None, [], [0.25]], DType.FLOAT32, "float32"),
    ("list<string>", [["a", None], [], None, ['b"c']], DType.STRING, "string"),
    ("list<bool>", [[True, None], None, [False], []], DType.BOOL, "bool"),
    ("list<uint8>", [[0, 255], [], [None], None], DType.UINT8, "uint8"),
]


@pytest.mark.parametrize("dtype,rows,child,elem", _TYPED_LISTS, ids=[t[0] for t in _TYPED_LISTS])
def test_typed_list_columns_round_trip_through_a_table(tmp_path, dtype, rows, child, elem):
    frame = DataFrame({"i": list(range(len(rows))), "v": Series(rows, dtype=dtype)})
    assert frame["v"].dtype == DType.LIST
    assert frame["v"].child(0).dtype == child
    path = os.fspath(tmp_path / "t")
    frame.write_table(path, name="t", options=TraceWriteOptions(build_index=False))
    header = [f for f in _header_fields(path) if f["name"] == "v"][0]
    assert header == {"name": "v", "dtype": "object", "elem": "list", "elem_dtype": elem}
    back = TraceViewer(path).collect()
    assert back["v"].dtype == DType.LIST
    assert back["v"].child(0).dtype == child
    assert back["v"].to_list() == rows
    assert back["v"].null_count == sum(r is None for r in rows)
    assert back.to_pandas()["v"].tolist() == rows
    # the carried header still describes the frame, so to_pandas restores it exactly
    from dftracer.utils import _table

    assert _table._exact_pandas(back, back._table_header) is not None
    again = os.fspath(tmp_path / "again")
    back.write_table(again, name="t", options=TraceWriteOptions(build_index=False))
    assert TraceViewer(again).collect()["v"].to_list() == rows


def _header_fields(path):
    import gzip
    import json

    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if os.path.isfile(full) and name.endswith((".gz", ".pfw")):
            opener = gzip.open if name.endswith(".gz") else open
            with opener(full, "rt") as handle:
                return json.loads(handle.readline())["args"]["fields"]
    raise AssertionError(f"no table file in {path}")


def _write_raw_rows(path, fields_json, texts, version="1.0"):
    """A table file with the given header fields (JSON text) and rows (JSON
    texts), written byte for byte."""
    import gzip

    path.mkdir()
    head = '{"name":"dftu.table","cat":"dftu","pid":0,"tid":0,"ts":0,"ph":4,"args":'
    args = (
        '{"format":"dftu.table","format_version":"%s","table":"t","nrows":%d,'
        '"index":[],"index_names":[],"fields":%s,"meta":{}}' % (version, len(texts), fields_json)
    )
    row = '{"name":"r","cat":"dftu","pid":0,"tid":0,"ts":0,"ph":3,"args":'
    with gzip.open(path / "t.pfw.gz", "wt") as handle:
        handle.write(head + args + "}\n")
        for text in texts:
            handle.write(row + text + "}\n")


def _list_field(elem, name="v"):
    return '[{"name":"%s","dtype":"object","elem":"list","elem_dtype":"%s"}]' % (name, elem)


def test_an_empty_table_result_keeps_every_column(tmp_path):
    from dftracer.utils import col

    opts = TraceWriteOptions(build_index=False)
    full = DataFrame({"i": [1, 2], "v": Series([[1], [2, 3]], dtype="list<int32>")})
    path = os.fspath(tmp_path / "full")
    full.write_table(path, name="t", options=opts)
    filtered = TraceViewer(path).filter(col("i") > 5).collect()
    assert filtered.columns == ["i", "v"] and len(filtered) == 0
    assert filtered["v"].dtype == DType.LIST and filtered["v"].child(0).dtype == DType.INT32
    empty = DataFrame({"i": Series([], dtype="int32"), "v": Series([], dtype="list<int32>")})
    path = os.fspath(tmp_path / "empty")
    empty.write_table(path, name="t", options=opts)
    viewer = TraceViewer(path)
    back = viewer.collect()
    assert list(viewer.schema) == ["i", "v"] and back.columns == ["i", "v"]
    assert back["i"].dtype == DType.INT32
    assert back["v"].child(0).dtype == DType.INT32
    assert len(back.to_pandas()) == 0


_BAD_CELLS = [
    ("int32", "[1 2]"),
    ("int32", "[1]junk"),
    ("int32", "[1,]"),
    ("int32", "[1,2"),
    ("uint64", "[01]"),
    ("int32", "[01]"),
    ("float32", "[3.5e38]"),
    ("float64", "[1e999]"),
    ("float64", '["NaN"]'),
    ("float64", '["Infinity","-Infinity"]'),
    ("string", '["a" "b"]'),
    ("bool", "[true false]"),
]


@pytest.mark.parametrize("elem,cell", _BAD_CELLS)
def test_a_malformed_list_cell_is_a_load_error_that_names_the_column(tmp_path, elem, cell):
    path = tmp_path / "bad"
    _write_raw_rows(path, _list_field(elem), ['{"v":[]}', '{"v":' + cell + "}"])
    with pytest.raises(Exception, match=r"row 1.*\bv\b"):
        TraceViewer(os.fspath(path)).collect()


def test_a_well_formed_list_cell_with_zero_and_decimals_reads(tmp_path):
    fields = (
        '[{"name":"u","dtype":"object","elem":"list","elem_dtype":"uint64"},'
        '{"name":"f","dtype":"object","elem":"list","elem_dtype":"float32"}]'
    )
    path = tmp_path / "ok"
    _write_raw_rows(path, fields, ['{"u":[0,10],"f":[0.5,-3.4e38]}'])
    back = TraceViewer(os.fspath(path)).collect()
    assert back["u"].to_list() == [[0, 10]] and back["f"].child(0).dtype == DType.FLOAT32


@pytest.mark.parametrize("value", ["34", "null", '""', '"int33"', '"list"', "true"])
def test_an_invalid_elem_dtype_refuses_the_header(tmp_path, value):
    fields = '[{"name":"v","dtype":"object","elem":"list","elem_dtype":%s}]' % value
    path = tmp_path / "t"
    _write_raw_rows(path, fields, ['{"v":[1]}'])
    with pytest.raises(Exception, match="elem_dtype"):
        TraceViewer(os.fspath(path)).collect()


@pytest.mark.parametrize(
    "fields",
    [
        '[{"name":"v","dtype":"int64","elem_dtype":"int32"}]',
        '[{"name":"v","dtype":"object","elem":"set","elem_dtype":"int32"}]',
        '[{"name":"v","dtype":"object","elem_dtype":"int32"}]',
    ],
)
def test_elem_dtype_is_refused_on_a_field_that_is_not_a_list(tmp_path, fields):
    path = tmp_path / "t"
    _write_raw_rows(path, fields, ['{"v":[1]}'])
    with pytest.raises(Exception, match="elem_dtype"):
        TraceViewer(os.fspath(path)).collect()


def test_list_dtype_refuses_large_string_and_a_mapping_row():
    with pytest.raises(ValueError, match="unknown dtype name"):
        Series([["a"]], dtype="list<large_string>")
    with pytest.raises(TypeError, match="not a list"):
        Series([{2: "a"}], dtype="list<int32>")


def test_a_float32_list_refuses_a_value_it_cannot_hold():
    with pytest.raises(ValueError, match="float32"):
        Series([[1e40]]).cast("list<float32>")
    with pytest.raises(ValueError, match="float32"):
        Series([[1.0, 1e40]], dtype="list<float32>")
    kept = Series([[math.inf, 1.5]], dtype="list<float32>")
    assert kept.to_list() == [[math.inf, 1.5]]


def test_a_float32_list_rounds_and_refuses_only_an_overflow():
    rounded = Series([[16777217, 0.1, None]], dtype="list<float32>").to_list()
    assert rounded[0][0] == 16777216.0
    assert rounded[0][1] == pytest.approx(0.1, rel=1e-7)
    assert rounded[0][2] is None
    cast = Series([[16777217]], dtype="list<int32>").astype("list<float32>")
    assert cast.to_list() == [[16777216.0]]
    with pytest.raises(ValueError, match="float32"):
        Series([[1e300]], dtype="list<float32>")
    with pytest.raises(ValueError, match="float32"):
        Series([[np.float64(1e300)]], dtype="list<float32>")
    with pytest.raises(ValueError, match="float32"):
        Series([np.array([1.0, 1e300])], dtype="list<float32>")
    kept = Series([[np.int64(2**53 + 1), np.uint64(2**63 + 1)]], dtype="list<float32>")
    assert kept.to_list() == [[2.0**53, 2.0**63]]
    text = Series([["inf", "", "abc"]], dtype="list<string>").astype("list<float32>")
    assert text.to_list() == [[math.inf, None, None]]
    with pytest.raises(ValueError, match="float32"):
        Series([["1e40", "abc"]], dtype="list<string>").astype("list<float32>")


def test_a_null_list_keeps_the_child_in_place(tmp_path):
    rows = [[1, 2], None, [3], None, []]
    series = Series(rows, dtype="list<int32>")
    assert series.null_count == 2 and series.to_list() == rows
    path = os.fspath(tmp_path / "t")
    DataFrame({"v": series}).write_table(
        path, name="t", options=TraceWriteOptions(build_index=False)
    )
    back = TraceViewer(path).collect()["v"]
    assert back.null_count == 2 and back.to_list() == rows and back.child(0).length == 3


def test_a_list_header_without_elem_dtype_reads_as_text(tmp_path):
    import gzip
    import json

    path = tmp_path / "old"
    path.mkdir()
    args = {
        "format": "dftu.table",
        "format_version": "1.0",
        "table": "t",
        "nrows": 2,
        "index": [],
        "index_names": [],
        "fields": [{"name": "v", "dtype": "object", "elem": "list"}],
        "meta": {},
    }
    lines = [
        {"name": "dftu.table", "cat": "dftu", "pid": 0, "tid": 0, "ts": 0, "ph": 4, "args": args}
    ]
    for cell in ([1, 2], None):
        lines.append(
            {"name": "r", "cat": "dftu", "pid": 0, "tid": 0, "ts": 0, "ph": 3, "args": {"v": cell}}
        )
    with gzip.open(path / "t.pfw.gz", "wt") as handle:
        handle.write("".join(json.dumps(line, separators=(",", ":")) + "\n" for line in lines))
    back = TraceViewer(os.fspath(path)).collect()
    assert back["v"].dtype == DType.STRING
    assert back["v"].to_list() == ["[1,2]", None]
    assert back.to_pandas()["v"].tolist() == [[1, 2], None]


def test_list_dtype_builds_a_narrow_child_without_pyarrow():
    body = """
    import sys
    from dftracer.utils import DType, Series
    s = Series([[1], [], None, [None, 3]], dtype="list<int32>")
    assert s.dtype == DType.LIST and s.child(0).dtype == DType.INT32
    assert s.to_list() == [[1], [], None, [None, 3]]
    t = Series.from_list([["a"], None], dtype="List<String>")
    assert t.child(0).dtype == DType.STRING and t.to_list() == [["a"], None]
    c = Series([[1, 2], None], name="x").astype("list<int8>")
    assert c.child(0).dtype == DType.INT8 and c.to_list() == [[1, 2], None] and c.name == "x"
    assert "pyarrow" not in sys.modules
    """
    out = subprocess.run(
        [sys.executable, "-c", textwrap.dedent(body)], capture_output=True, text=True
    )
    assert out.returncode == 0, out.stderr[-2000:]


def test_list_dtype_refuses_what_it_cannot_hold():
    with pytest.raises(ValueError, match="unknown dtype name"):
        Series([[1]], dtype="list<decimal>")
    with pytest.raises(ValueError, match="unknown dtype name"):
        Series([[1]], dtype="list<list<int32>>")
    with pytest.raises(ValueError):
        Series([[300]], dtype="list<int8>")
    with pytest.raises(TypeError):
        Series([1, 2]).astype("list<int32>")
    with pytest.raises(TypeError, match="not a list"):
        Series(["ab"], dtype="list<string>")


def test_parts_with_other_list_element_types_do_not_merge(tmp_path):
    path = os.fspath(tmp_path / "t")
    opts = TraceWriteOptions(build_index=False)
    DataFrame({"v": Series([[1]], dtype="list<int32>")}).write_table(path, name="t", options=opts)
    other = os.fspath(tmp_path / "u")
    DataFrame({"v": Series([[1]], dtype="list<int64>")}).write_table(other, name="t", options=opts)
    merged = os.fspath(tmp_path / "both")
    os.mkdir(merged)
    for src, tag in ((path, "a"), (other, "b")):
        for name in os.listdir(src):
            if name.endswith((".gz", ".pfw")):
                os.rename(os.path.join(src, name), os.path.join(merged, tag + name))
    with pytest.raises(Exception, match="elem_dtype"):
        TraceViewer(merged).collect()


_NO_NUMPY = """
import importlib.abc, os, sys, tempfile

class _Block(importlib.abc.MetaPathFinder):
    def find_spec(self, name, path, target=None):
        if name.split(".")[0] in ("numpy", "pandas", "pyarrow", "polars"):
            raise ModuleNotFoundError(f"No module named {name!r}")

sys.meta_path.insert(0, _Block())
from dftracer.utils import DataFrame, Series, TraceWriteOptions
from dftracer.utils.trace import TraceViewer
"""


def test_core_paths_run_without_numpy_pandas_or_pyarrow(tmp_path):
    body = """
    df = DataFrame({"a": [1, 2], "f": [1.0, 2.0], "s": ["x", None]})
    df["c"] = 0
    df["a"] = 7
    df["f"] = 2.5
    df.loc[df["c"] == 0, "s"] = "y"
    assert df["a"].to_list() == [7, 7] and df["s"].to_list() == ["y", "y"]
    df.groupby("s").agg(n=("a", "nunique"), m=("f", "median"))
    Series(["a"]).map({"a": 1})
    df.memory_usage(index=False).sum()
    path = os.path.join(sys.argv[1], "t")
    opts = TraceWriteOptions(build_index=False)
    frame = DataFrame({"a": [1, 2], "f": [1.0, None], "tags": [["p"], ["q"]]})
    frame.write_table(path, name="t", options=opts)
    back = TraceViewer(path).collect()
    back.write_table(os.path.join(sys.argv[1], "u"), name="u", options=opts)
    assert TraceViewer(os.path.join(sys.argv[1], "u")).collect()["a"].to_list() == [1, 2]
    for name in ("numpy", "pandas", "pyarrow"):
        assert name not in sys.modules, name
    """
    out = subprocess.run(
        [sys.executable, "-c", _NO_NUMPY + textwrap.dedent(body), os.fspath(tmp_path)],
        capture_output=True,
        text=True,
    )
    assert out.returncode == 0, out.stderr[-2000:]


def test_list_join_reads_a_list_of_string_views():
    pa = pytest.importorskip("pyarrow")
    if not hasattr(pa, "string_view"):
        pytest.skip("pyarrow has no string_view")
    data = [["/a", "b"], [], None, ["c"]]
    table = pa.table({"a": pa.array(data, type=pa.list_(pa.string_view()))})
    got = DataFrame.from_arrow(table)["a"].list_join("-").to_list()
    assert got == ["/a-b", "", None, "c"]
    assert DataFrame.from_arrow(table)["a"].list_join("").to_list() == ["/ab", "", None, "c"]


def test_large_list_columns_take_the_list_functions(tmp_path):
    pa = pytest.importorskip("pyarrow")
    data = [["a", "bb"], [], None, ["c"]]
    table = pa.table({"v": pa.array(data, type=pa.large_list(pa.string()))})
    v = DataFrame.from_arrow(table)["v"]
    assert v.list_len().to_list() == [2, 0, None, 1]
    assert v.list_join("/").to_list() == ["a/bb", "", None, "c"]
    assert v.list.get(0).to_list() == ["a", None, None, "c"]
    exploded = DataFrame.from_arrow(table).explode("v")
    assert exploded["v"].to_list() == ["a", "bb", None, None, "c"]
    path = os.fspath(tmp_path / "t")
    DataFrame.from_arrow(table).write_table(
        path, name="t", options=TraceWriteOptions(build_index=False)
    )
    assert TraceViewer(path).collect().to_pandas()["v"].tolist() == data


def test_large_list_get_in_an_expression():
    pa = pytest.importorskip("pyarrow")
    from dftracer.utils import col

    table = pa.table({"a": pa.array([["x", "y"], [], None], type=pa.large_list(pa.string()))})
    got = DataFrame.from_arrow(table).select(col("a").str.get(0).alias("x"))
    assert got["x"].to_list() == ["x", None, None]


def test_write_table_decodes_a_dictionary_list_child(tmp_path):
    pa = pytest.importorskip("pyarrow")
    child = pa.array([1, 2, 1]).dictionary_encode()
    arr = pa.ListArray.from_arrays(pa.array([0, 2, 3], pa.int32()), child)
    path = os.fspath(tmp_path / "t")
    DataFrame.from_arrow(pa.table({"l": arr})).write_table(
        path, name="t", options=TraceWriteOptions(build_index=False)
    )
    assert TraceViewer(path).collect().to_pandas()["l"].tolist() == [[1, 2], [1]]


@pytest.mark.parametrize(
    ("data", "mapping"),
    [
        ([1, 2], {1.2: "a", 1.8: "b"}),
        ([1, 2], {1.0: "a", 2.5: "b"}),
        ([1.0, 2.5], {1: "a"}),
        ([1, 2], {True: "t"}),
        ([1, None, 3], {None: 5, 1: 2}),
        ([1.0, math.nan], {math.nan: "z", 1: "a"}),
        (["a", None], {None: "z", "a": "q"}),
        ([1, 2], {}),
    ],
)
def test_map_matches_keys_exactly_as_pandas(data, mapping):
    got = Series(data).map(mapping)
    assert len(got) == len(data)
    assert _nan_eq(got.to_list(), pd.Series(data).map(mapping).tolist())


def test_drop_errors_ignore_keeps_the_row_index():
    df = DataFrame({"a": [1], "b": [2]}).set_index("a")
    assert df.drop(columns="a", errors="ignore").index.to_list() == [1]
    assert df.drop(columns=["a", "b"], errors="ignore").index.to_list() == [1]


def test_isin_compares_integers_and_floats_exactly():
    pa = pytest.importorskip("pyarrow")
    big = 2**53 + 1
    df = DataFrame.from_arrow(pa.table({"u": pa.array([big, 2**53, 3], pa.uint64())}))
    got = df.eval("u.isin([9007199254740992.0, 3.5])").to_list()
    assert (
        got == pd.Series([big, 2**53, 3], dtype="uint64").isin([9007199254740992.0, 3.5]).tolist()
    )


def test_nunique_takes_the_list_form_and_a_key_column():
    df = DataFrame({"g": [1, 1, 2], "a": [1, 2, 4]})
    p = df.to_pandas()
    got = df.groupby("g")["a"].agg(["nunique", "median"]).to_pandas()
    want = p.groupby("g")["a"].agg(["nunique", "median"])
    assert list(got.columns) == list(want.columns)
    assert got["nunique"].tolist() == want["nunique"].tolist()
    assert got["median"].tolist() == want["median"].tolist()
    keyed = df.groupby("g").agg(n=("g", "nunique")).to_pandas()
    assert keyed["n"].tolist() == p.groupby("g").agg(n=("g", "nunique"))["n"].tolist()


def test_series_from_a_series_reindexes_by_label():
    s = Series([10, 20], index=["a", "b"])
    p = pd.Series([10, 20], index=["a", "b"])
    assert _nan_eq(
        Series(s, index=["b", "a", "z"]).to_list(), pd.Series(p, index=["b", "a", "z"]).tolist()
    )
    assert _nan_eq(
        Series(Series([1, 2]), index=[1, 0]).to_list(),
        pd.Series(pd.Series([1, 2]), index=[1, 0]).tolist(),
    )
    assert Series([1, 2], index=["p", "q"]).index.to_list() == ["p", "q"]


@pytest.mark.parametrize(
    "data",
    [[1.0, math.inf], [-1e308, 1e308], [1.0, 2.0, math.inf, math.inf], [1.0, 3.0], [1.0, 5.0, 2.0]],
)
def test_median_and_group_quantile_match_pandas(data):
    with np.errstate(all="ignore"):
        p = pd.DataFrame({"k": [0] * len(data), "v": data})
        df = DataFrame({"k": [0] * len(data), "v": data})
        assert _nan_eq([Series(data).median()], [p["v"].median()])
        assert _nan_eq(
            df.groupby("k").median()["v"].to_list(), p.groupby("k")["v"].median().tolist()
        )
        named = df.groupby("k").agg(m=("v", "median"))["m"].to_list()
        assert _nan_eq(named, p.groupby("k").agg(m=("v", "median"))["m"].tolist())
        for q in (0.0, 0.3, 0.95, 1.0):
            got = df.groupby("k").quantile(q)["v"].to_list()
            assert _nan_eq(got, p.groupby("k")["v"].quantile(q).tolist())


def test_eval_floor_division_and_modulo_match_numpy():
    a = [1.0, 7.5, -1.0, math.inf]
    b = [0.1, 0.1, -0.1, 1.0]
    df = DataFrame({"a": a, "b": b})
    with np.errstate(all="ignore"):
        assert _nan_eq(df.eval("a // b").to_list(), np.floor_divide(a, b).tolist())
        assert _nan_eq(df.eval("a % b").to_list(), np.mod(a, b).tolist())
    ints = DataFrame({"a": [7, -7, 7], "b": [2, 2, -2]})
    assert ints.eval("a // b").to_list() == [3, -4, -4]
    assert ints.eval("a % b").to_list() == [1, 1, -1]


def test_isin_an_empty_list_on_a_string_column():
    df = DataFrame({"s": ["a", None]})
    assert df.eval("s.isin([])").to_list() == [False, False]
    assert len(df.query("s in []")) == 0
    assert len(df.query("s not in []")) == 2


def test_lazy_group_by_reads_leading_column_names_as_keys():
    df = DataFrame({"h": ["a", "a", "b"], "t": [1, 1, 2], "v": [1, 2, 3]})
    got = df.lazy().group_by("h", "t").agg(n=("v", "sum")).collect().sort("h")
    assert got["n"].to_list() == [3, 3]


def test_small_pandas_forms_abs_bool_cut_true_and_defaultdict_map():
    from collections import defaultdict

    assert abs(Series([True, False])).to_list() == [True, False]
    with pytest.raises(ValueError, match="labels"):
        Series([1.0]).cut(Series([0.0, 2.0]), labels=True)
    d = defaultdict(lambda: "z", {1: "a"})
    assert (
        Series([1, 2]).map(d).to_list()
        == pd.Series([1, 2]).map(defaultdict(lambda: "z", {1: "a"})).tolist()
    )


def test_median_of_a_dictionary_column():
    pa = pytest.importorskip("pyarrow")
    s = Series(pa.array([10, 40, 20, 20]).dictionary_encode())
    assert s.median() == 20.0


def _layered_columns():
    pa = pytest.importorskip("pyarrow")

    vals = [5.0, None, 1.0, 9.0, 3.0, 3.0, None, 7.0]
    keep = [v is not None and v != 9.0 for v in vals]
    selection = Series(pa.array(vals)).filter(Series(keep))
    assert selection.encoding == 3
    chunks = [vals[:4], vals[4:]]
    dict_chunked = DataFrame.from_arrow(
        pa.Table.from_batches(
            [pa.record_batch({"x": pa.array(c).dictionary_encode()}) for c in chunks]
        )
    )["x"]
    assert dict_chunked.encoding == 5
    plain_chunked = DataFrame.from_arrow(
        pa.Table.from_batches([pa.record_batch({"x": pa.array(c)}) for c in chunks])
    )["x"]
    assert plain_chunked.encoding == 5
    kept = [v for v, k in zip(vals, keep) if k]
    present = [v for v in vals if v is not None]
    return [(selection, kept), (dict_chunked, present), (plain_chunked, present)]


def test_median_and_quantile_read_layered_columns_in_place():
    for s, want in _layered_columns():
        assert s.median() == float(np.median(want))
        for q in (0.0, 0.3, 0.5, 1.0):
            assert s.quantile(q, interpolation="linear") == pytest.approx(
                float(np.quantile(want, q))
            )


def test_map_refuses_keys_that_are_not_equal():
    assert _nan_eq(Series(["1"]).map({1: "x"}).to_list(), pd.Series(["1"]).map({1: "x"}).tolist())
    f32 = Series([1.0], dtype="float32")
    assert f32.map({1 + 2**-30: "x"}).to_list() == [None]
    assert f32.map({1.0: "x"}).to_list() == ["x"]


def test_map_defaultdict_fills_null_rows_too():
    from collections import defaultdict

    data = ["a", None, "b"]
    d = defaultdict(lambda: "default", {"a": "first"})
    want = pd.Series(data).map(defaultdict(lambda: "default", {"a": "first"})).tolist()
    assert Series(data).map(d).to_list() == want


def test_an_aggregate_named_as_a_key_is_refused():
    df = DataFrame({"g": [0.5, 0.5, 1.5], "v": [1, 2, 3]})
    for spec in (("g", "nunique"), ("v", "sum")):
        with pytest.raises(ValueError, match="group key"):
            df.groupby("g").agg(g=spec)


def test_map_skips_keys_no_value_of_the_dtype_equals():
    pa = pytest.importorskip("pyarrow")
    assert Series(["1", "a"]).map({1: "ONE", "a": "A"}).to_list() == [None, "A"]
    assert Series(["a", "b"]).map({(1, 2): "T", "a": "A"}).to_list() == ["A", None]
    assert Series([1.0, 2.0]).map({10**400: "BIG", 1.0: "A"}).to_list() == ["A", None]
    f32 = Series(pa.array([float(2**24), 1.0], type=pa.float32()))
    assert f32.map({2**24 + 1: "BAD", 1: "OK", 1e300: "X"}).to_list() == [None, "OK"]


def test_abs_of_a_bool_column_everywhere():
    assert Series([True, False]).abs().to_list() == [True, False]
    assert DataFrame({"b": [True, False]}).abs()["b"].to_list() == [True, False]


def test_list_agg_keeps_a_key_that_starts_with_the_column_name():
    df = DataFrame({"a_id": [1, 1, 2], "a": [1, 2, 4]})
    got = df.groupby("a_id", as_index=False)["a"].agg(["nunique", "median"]).to_pandas()
    want = df.to_pandas().groupby("a_id", as_index=False)["a"].agg(["nunique", "median"])
    assert list(got.columns) == list(want.columns)


def test_median_above_the_parallel_size_with_far_ranks():
    # Two clusters of distinct values: the two middle ranks sit in buckets
    # with empty buckets between them, and no bucket holds a quarter of them.
    data = list(range(200_000)) + [10**9] * 50_000 + list(range(10**9 + 1, 10**9 + 150_001))
    s = Series(data)
    want = pd.Series(data).median()
    assert s.median() == want
    assert s.quantile(0.5, interpolation="midpoint") == want
    assert DataFrame({"k": [0] * len(data), "v": data}).groupby("k").median()["v"].to_list() == [
        want
    ]


def test_floor_division_by_a_float_zero_and_on_uint64_match_numpy():
    pa = pytest.importorskip("pyarrow")
    df = DataFrame({"x": [1.0, -1.0, 0.0], "y": [0.0, 0.0, 0.0]})
    with np.errstate(all="ignore"):
        assert _nan_eq(df.eval("x // y").to_list(), np.floor_divide([1.0, -1.0, 0.0], 0.0).tolist())
        # pandas gives NaN, its missing value, which is null here.
        assert df.eval("x % y").to_list() == [None, None, None]
    big = [2**64 - 1, 2**63, 7]
    u = DataFrame.from_arrow(pa.table({"u": pa.array(big, pa.uint64())}))
    a = np.array(big, dtype=np.uint64)
    assert u.eval("u // 2").to_list() == (a // 2).tolist()
    assert u.eval("u % 3").to_list() == (a % 3).tolist()


def test_rolling_median_of_wide_values():
    s = Series([-1e308, 1e308])
    assert (
        s.rolling_median(2).to_list()[-1]
        == pd.Series([-1e308, 1e308]).rolling(2).median().tolist()[-1]
    )


def test_a_key_named_size_keeps_its_values():
    data = {"size": [9, 9, 8], "v": [2, 3, 4]}
    want = pd.DataFrame(data).groupby("size")
    got = DataFrame(data).groupby("size")
    assert got.size().to_pandas().to_dict() == want.size().to_dict()
    assert (
        got.agg(distinct=("v", "nunique")).to_pandas().to_dict()
        == want.agg(distinct=("v", "nunique")).to_dict()
    )
    with pytest.raises(ValueError, match="cannot insert size"):
        DataFrame(data).groupby("size", as_index=False).size()


def test_a_list_agg_named_as_a_key_is_refused():
    data = {"sum": [1, 1, 2], "v": [2, 3, 4]}
    with pytest.raises(ValueError, match="cannot insert sum"):
        pd.DataFrame(data).groupby("sum", as_index=False)["v"].agg(["sum", "median"])
    for as_index in (True, False):
        with pytest.raises(ValueError, match="cannot insert sum"):
            DataFrame(data).groupby("sum", as_index=as_index)["v"].agg(["sum", "median"])


def test_reverse_uint64_floordiv_and_mod_stay_exact():
    pa = pytest.importorskip("pyarrow")
    big = 2**53
    s = Series(pa.array([big, 3, 0], type=pa.uint64()))
    want = np.array([big + 1], dtype=np.uint64) % np.array([big, 3], dtype=np.uint64)
    got = (big + 1) % s
    assert got.dtype == DType.UINT64
    assert got.to_list() == [int(want[0]), int(want[1]), None]
    assert ((big + 1) // s).to_list() == [1, (big + 1) // 3, None]


def test_map_matches_float16_keys():
    pa = pytest.importorskip("pyarrow")
    s = Series(pa.array(np.array([1.0, 1.5], dtype=np.float16)))
    assert s.map({1.0: "hit", 1.5: "half"}).to_list() == ["hit", "half"]
    assert s.map({1.0001: "near"}).to_list() == [None, None]


def test_abs_of_a_filtered_bool_keeps_its_encoding():
    s = Series([True, False, True]).filter(Series([True, False, True]))
    assert s.encoding == 3
    out = abs(s)
    assert out.encoding == 3
    assert out.to_list() == [True, True]


def test_describe_keeps_a_key_that_starts_with_the_column_name():
    data = {"a_id": [1, 1, 2], "a": [1, 2, 4]}
    out = DataFrame(data).groupby("a_id", as_index=False)["a"].describe()
    assert out.columns[0] == "a_id"
    assert out["a_id"].to_list() == [1, 2]


def test_map_matches_numpy_scalar_keys():
    ints = Series([1, 2])
    assert ints.map({np.int64(1): "A"}).to_list() == ["A", None]
    f32 = Series([1.5, 2.0], dtype="float32")
    assert f32.map({np.float32(1.5): "A"}).to_list() == ["A", None]


def test_lazy_group_by_refuses_an_aggregate_named_as_a_key():
    from dftracer.utils import col

    lf = DataFrame({"g": [1, 1, 2], "v": [1, 2, 4]}).lazy()
    with pytest.raises(DFTUtilsError, match="is a group key"):
        lf.group_by("g").agg(col("v").sum().alias("g")).collect()
