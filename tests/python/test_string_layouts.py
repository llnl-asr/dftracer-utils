#!/usr/bin/env python3
"""String columns over a View, dictionary export, Arrow slices and IPC."""

import gzip
import json
import os

import pytest

import dftracer.utils as dftu_utils
from dftracer.utils import DataFrame, Series, TraceViewer

from .common import Environment

pa = pytest.importorskip("pyarrow")

CATS = ["posix", "stdio", "mpiio"]
N = 240


def _rows():
    rows = []
    for i in range(N):
        args = {"ret": i}
        if i % 3 != 0:
            args["path"] = "/f%d" % (i % 2)
        rows.append(
            {
                "ph": "X",
                "name": "op%d" % (i % 2),
                "cat": CATS[i % 3],
                "pid": 1,
                "tid": 1,
                "ts": 1000 + i,
                "dur": 5,
                "args": args,
            }
        )
    return rows


def _collect(env, query=None):
    path = os.path.join(env.temp_dir, "dict.pfw.gz")
    with gzip.open(path, "wt") as f:
        f.write("\n".join(json.dumps(r) for r in _rows()) + "\n")
    with dftu_utils.Indexer(files=[path]) as indexer:
        indexer.ensure_indexed()
    view = TraceViewer(path)
    if query:
        view = view.filter(query)
    return view.collect()


def _expected_cat():
    return [CATS[i % 3] for i in range(N)]


def test_view_string_values_and_conversions():
    with Environment(lines=1) as env:
        df = _collect(env)
        assert df["cat"].to_list() == _expected_cat()
        assert list(df["cat"].to_numpy()) == _expected_cat()
        assert df.to_pandas()["cat"].tolist() == _expected_cat()
        assert df.to_arrow()["cat"].to_pylist() == _expected_cat()


def test_missing_string_arg_stays_none():
    with Environment(lines=1) as env:
        df = _collect(env)
        want = [("/f%d" % (i % 2)) if i % 3 != 0 else None for i in range(N)]
        assert df["args.path"].to_list() == want
        assert df.to_arrow()["args.path"].to_pylist() == want


def test_filter_on_string_column():
    with Environment(lines=1) as env:
        df = _collect(env, 'cat == "stdio"')
        assert df.height == N // 3
        assert set(df["cat"].to_list()) == {"stdio"}
        assert df["args.ret"].to_list() == [i for i in range(N) if i % 3 == 1]


def test_iteration_polars_and_value_counts():
    pl = pytest.importorskip("polars")
    with Environment(lines=1) as env:
        df = _collect(env)
        s = df["cat"]
        assert list(s) == _expected_cat()
        assert s[0] == "posix" and s[-1] == "mpiio"
        assert s.n_unique() == 3
        assert sorted(s.unique().to_list()) == sorted(CATS)
        assert df.to_polars()["cat"].cast(pl.Utf8).to_list() == _expected_cat()


def test_scan_string_columns_export_as_dictionary_or_view():
    with Environment(lines=1) as env:
        df = _collect(env)
        table = df.to_arrow()
        assert pa.types.is_dictionary(table["cat"].type) or pa.types.is_string(table["cat"].type)
        assert table["cat"].to_pylist() == _expected_cat()
        assert df["cat"].to_list() == _expected_cat()
        assert df["name"].to_list() == ["op%d" % (i % 2) for i in range(N)]


def _dict_frame():
    values = [CATS[i % 3] if i % 5 else None for i in range(60)]
    return values, DataFrame({"cat": Series(values).dictionary_encode()})


def test_dictionary_column_exports_as_dictionary():
    values, df = _dict_frame()
    tbl = df.to_arrow()
    assert pa.types.is_dictionary(tbl.schema.field("cat").type)
    tbl["cat"].combine_chunks().validate(full=True)
    assert tbl["cat"].to_pylist() == values


def test_ipc_writes_dictionary_column():
    values, df = _dict_frame()
    with pa.ipc.open_stream(df.to_ipc()) as reader:
        assert reader.read_all()["cat"].to_pylist() == values


def test_from_arrow_slice_string():
    arr = pa.array(["a", None, "c", "d"]).slice(1, 2)
    assert Series.from_arrow(arr).to_list() == [None, "c"]


def test_from_arrow_slice_large_string_and_binary():
    for arr in (
        pa.array(["a", None, "c", "d"], pa.large_string()),
        pa.array([b"a", None, b"c", b"d"], pa.binary()),
    ):
        assert Series.from_arrow(arr.slice(1, 2)).to_list() == arr.slice(1, 2).to_pylist()


def test_from_arrow_slice_bool_with_nulls():
    vals = [None if i % 7 == 0 else bool(i % 3 == 0) for i in range(20)]
    arr = pa.array(vals, pa.bool_())
    for off in (3, 8, 9):
        sl = arr.slice(off, 10)
        assert Series.from_arrow(sl).to_list() == sl.to_pylist()


def test_from_arrow_slice_list_and_struct():
    lst = pa.array([[1], [2, 3], None, [4, 5, 6], []])
    assert Series.from_arrow(lst.slice(1, 3)).to_list() == lst.slice(1, 3).to_pylist()
    st = pa.array([{"a": i, "b": str(i)} for i in range(6)])
    sl = st.slice(2, 3)
    assert Series.from_arrow(sl).to_list() == sl.to_pylist()


def test_from_arrow_slice_dictionary():
    arr = pa.array(["x", None, "y", "x", "z"]).dictionary_encode().slice(1, 3)
    assert Series.from_arrow(arr).to_list() == arr.to_pylist()


def test_selection_outer_fill_exports_valid_dictionary():
    np = pytest.importorskip("numpy")
    base = pa.array(["x", "y", "z"])
    idx = pa.py_buffer(np.array([0, -1, 2], dtype=np.int64))
    typ = pa.dictionary(pa.int64(), pa.string())
    arr = pa.DictionaryArray.from_buffers(typ, 3, [pa.py_buffer(b"\x05"), idx], dictionary=base)
    out = Series.from_arrow(arr).to_arrow()
    out.validate(full=True)
    assert out.to_pylist() == ["x", None, "z"]


VIEW_ROWS = [
    "short",
    None,
    "a string that is longer than twelve bytes",
    "short",
    "",
    "another string that is longer than twelve",
    "twelve bytes",
    "a string that is longer than twelve bytes",
]


def _view_series(rows=VIEW_ROWS):
    return Series.from_arrow(pa.array(rows, pa.string_view()))


def _flat_frame(rows=VIEW_ROWS):
    return DataFrame({"k": Series(rows), "v": Series(list(range(len(rows))))})


def _view_frame(rows=VIEW_ROWS):
    return DataFrame({"k": _view_series(rows), "v": Series(list(range(len(rows))))})


def test_string_view_round_trip():
    s = _view_series()
    assert s.encoding == 4
    out = s.to_arrow()
    assert out.type == pa.string_view()
    out.validate(full=True)
    assert out.to_pylist() == VIEW_ROWS
    assert s.to_list() == VIEW_ROWS


def test_binary_view_round_trip():
    rows = [b"ab", None, b"a binary value longer than twelve", b""]
    s = Series.from_arrow(pa.array(rows, pa.binary_view()))
    assert s.encoding == 4
    out = s.to_arrow()
    assert out.type == pa.binary_view()
    out.validate(full=True)
    assert out.to_pylist() == rows


def test_string_view_sliced():
    arr = pa.array(VIEW_ROWS, pa.string_view()).slice(1, 5)
    s = Series.from_arrow(arr)
    assert s.to_list() == arr.to_pylist()
    out = s.to_arrow()
    out.validate(full=True)
    assert out.to_pylist() == arr.to_pylist()


def test_string_view_dictionary_values():
    values = pa.array(["x", "a string that is longer than twelve bytes"], pa.string_view())
    arr = pa.DictionaryArray.from_arrays(pa.array([1, 0, None, 1], pa.int32()), values)
    s = Series.from_arrow(arr)
    assert s.to_list() == arr.to_pylist()
    out = s.to_arrow()
    out.validate(full=True)
    assert out.to_pylist() == arr.to_pylist()


def test_string_view_conversions():
    np = pytest.importorskip("numpy")
    pd = pytest.importorskip("pandas")
    pl = pytest.importorskip("polars")
    s = _view_series()
    assert s.to_list() == VIEW_ROWS
    assert [None if v is None else str(v) for v in s.to_numpy()] == VIEW_ROWS
    assert isinstance(s.to_numpy(), np.ndarray)
    assert [None if pd.isna(v) else v for v in s.to_pandas()] == VIEW_ROWS
    assert s.to_polars().to_list() == VIEW_ROWS
    assert isinstance(s.to_polars(), pl.Series)


def test_string_view_filter_matches_flat():
    mask = [i % 2 == 0 for i in range(len(VIEW_ROWS))]
    view = _view_series().filter(Series(mask))
    flat = Series(VIEW_ROWS).filter(Series(mask))
    assert view.to_list() == flat.to_list()


def test_string_view_sort_matches_flat():
    assert (
        _view_frame().sort("k").to_arrow().to_pylist()
        == _flat_frame().sort("k").to_arrow().to_pylist()
    )


def test_string_view_group_by_matches_flat():
    def counts(df):
        out = df.group_by("k").count()
        return sorted(zip(out["k"].to_list(), out["v"].to_list()), key=lambda t: t[0])

    assert counts(_view_frame()) == counts(_flat_frame())


def test_string_view_join_matches_flat():
    right = DataFrame({"k": Series(["short", "twelve bytes", "zzz"]), "w": Series([1, 2, 3])})

    def joined(df):
        out = df.join(right, on="k", how="inner").sort("v")
        return list(zip(out["k"].to_list(), out["v"].to_list(), out["w"].to_list()))

    assert joined(_view_frame()) == joined(_flat_frame())


@pytest.mark.parametrize(
    ("op", "expected"),
    [
        ("lt", ["apple"]),
        ("le", ["apple", "m"]),
        ("gt", ["zoo"]),
        ("ge", ["zoo", "m"]),
    ],
)
def test_ordered_compare_against_a_string(op, expected):
    from dftracer.utils import col

    df = DataFrame({"name": ["apple", "zoo", "m", None]})
    c = col("name")
    pred = {"lt": c < "m", "le": c <= "m", "gt": c > "m", "ge": c >= "m"}[op]
    out = df.lazy().filter(pred).collect()
    assert out["name"].to_list() == expected
