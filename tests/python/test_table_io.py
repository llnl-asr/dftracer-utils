#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test cases for writing typed tables with DataFrame.write and reading them
back through TraceViewer."""

import gzip
import importlib.util
import json
import math
import os
import re
import subprocess
import sys

import pytest

np = pytest.importorskip("numpy")
pd = pytest.importorskip("pandas")
pa = pytest.importorskip("pyarrow")

import dftracer.utils as dftu  # noqa: E402
from dftracer.utils import DataFrame, TraceWriteOptions  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402
from dftracer.utils.trace import TraceViewer  # noqa: E402

REFERENCE = os.environ.get("DFTRACER_UTILS_DFANALYZER_TABLE_REF")
EQUAL = dict(check_dtype=True, check_index_type=False, rtol=0)
U64_MAX = 2**64 - 1


def save_table(df, path, *, name, meta=None, level=6):
    """Write a pandas or native frame as one table, replacing the target."""
    if isinstance(df, pd.DataFrame):
        frame = DataFrame.from_pandas(df)
    elif isinstance(df, DataFrame):
        frame = df
    else:
        raise TypeError(f"expected a pandas or dftracer.utils DataFrame, got {type(df).__name__}")
    return frame.write_table(
        path,
        name=name,
        meta=meta,
        mode="overwrite",
        options=TraceWriteOptions(level=level, build_index=False),
    )


def load_table(path):
    """The frame of a table file, folder, glob or list: a TraceViewer collect."""
    return TraceViewer(path).collect()


def read_pandas(path, **kwargs):
    """The exact pandas frame of a table file: load_table, then to_pandas()."""
    return load_table(path, **kwargs).to_pandas()


def float64_masked(values, mask):
    return pd.arrays.FloatingArray(np.array(values, dtype="float64"), np.array(mask, dtype=bool))


def matrix(n=5):
    """One column of every dtype the format carries."""
    cats = ["lo", "mid", "hi"]
    df = pd.DataFrame(
        {
            "Int64": pd.array([1, None, -3, 2**62, 5], dtype="Int64"),
            "Int32": pd.array([1, None, -3, 4, 5], dtype="Int32"),
            "Float64": float64_masked(
                [1.5, math.nan, math.inf, -math.inf, 0.0], [False, False, False, False, True]
            ),
            "Float32": pd.array([0.1, None, 2.5, 3.0, 4.0], dtype="Float32"),
            "float64": np.array([1.5, math.nan, math.inf, -math.inf, -0.0]),
            "float32": np.array([0.1, 2.5, 3.0, math.nan, 5.0], dtype="float32"),
            "int64": np.array([-(2**63), -1, 0, 2**63 - 1, 7], dtype="int64"),
            "int8": np.array([-128, -1, 0, 127, 7], dtype="int8"),
            "uint64": np.array([0, U64_MAX, 1 << 63, 5, 6], dtype="uint64"),
            "bool": np.array([True, False, True, True, False]),
            "boolean": pd.array([True, None, False, True, False], dtype="boolean"),
            "string": pd.array(["a", None, 'q"uote\\', "line\nbreak\t", "é中"], dtype="string"),
            "str_pyarrow": pd.array(["a", None, "b", "c", "d"], dtype="string[pyarrow]"),
            "str_python": pd.array(["a", None, "b", "c", "d"], dtype=pd.StringDtype("python")),
            "arrow_str": pd.array(["a", None, "b", "c", "d"], dtype=pd.ArrowDtype(pa.string())),
            "arrow_bool": pd.array(
                [True, None, False, True, False], dtype=pd.ArrowDtype(pa.bool_())
            ),
            "arrow_i32": pd.array([1, None, 3, 4, 5], dtype=pd.ArrowDtype(pa.int32())),
            "int64_pyarrow": pd.array([1, None, 3, 4, 5], dtype="int64[pyarrow]"),
            "double_pyarrow": pd.array([1.5, None, 3.0, math.nan, 5.0], dtype="double[pyarrow]"),
            "object": pd.Series(["x", None, "y z", "", "ü"], dtype=object),
            "cat": pd.Categorical(["lo", None, "hi", "mid", "lo"], categories=cats, ordered=False),
            "cat_ordered": pd.Categorical(
                ["lo", "mid", None, "hi", "lo"], categories=cats, ordered=True
            ),
            "cat_int": pd.Categorical([3, 4, None, 3, 4], categories=[3, 4], ordered=True),
            "cat_i32": pd.Categorical(
                [3, 4, None, 3, 4], categories=pd.Index([3, 4], dtype="int32")
            ),
            "cat_u8": pd.Categorical(
                [1, None, 2, 2, 1], categories=pd.Index([1, 2], dtype="uint8")
            ),
            "cat_f32": pd.Categorical(
                [0.5, 1.5, None, 1.5, 0.5], categories=pd.Index([0.5, 1.5], dtype="float32")
            ),
            "cat_Int64": pd.Categorical(
                [1, 2, None, 1, 2], categories=pd.Index([1, 2], dtype="Int64")
            ),
            "cat_nan_label": pd.Categorical(
                ["NaN", "x", None, "x", "NaN"], categories=["NaN", "x"]
            ),
            "sets": pd.Series([{1, 2}, set(), None, {"a", "b"}, {3.5, None}], dtype=object),
            "lists": pd.Series([[1, 2], [], None, ["a", "b", "a"], [3.5, None, 7]], dtype=object),
        }
    )
    return df.iloc[:n].copy()


def sets_equal_nan(a, b):
    """assert_frame_equal cannot compare sets holding NaN, so compare cells."""
    assert len(a) == len(b)
    for x, y in zip(a, b):
        if x is None or y is None:
            assert x is None and y is None
            continue
        assert type(x) is type(y)
        if isinstance(x, list):
            assert len(x) == len(y)
            for p, q in zip(x, y):
                assert p == q or (
                    isinstance(p, float)
                    and isinstance(q, float)
                    and math.isnan(p)
                    and math.isnan(q)
                )
        else:
            assert x == y


def assert_round_trip(df, tmp_path, name="t.pfw.gz", **kwargs):
    path = str(tmp_path / name)
    save_table(df, path, name="t", **kwargs)
    back = read_pandas(path)
    pd.testing.assert_frame_equal(back, df, **EQUAL)
    return path, back


def lines_of(path):
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as f:
        return f.read().splitlines()


def test_the_removed_functions_are_gone_and_the_new_names_exported():
    for name in ("write_table", "read_table", "TableWriter", "export_trace", "sink_json"):
        assert name not in dftu.__all__
        assert not hasattr(dftu, name)
        assert not hasattr(TraceViewer, name)
    assert not hasattr(_ext.TraceViewer, "export_trace")
    for name in ("SinkSummary", "TraceWriteOptions"):
        assert name in dftu.__all__
        assert hasattr(dftu, name)
    assert hasattr(DataFrame, "write")


def test_a_table_read_returns_our_frame_that_restores_pandas_exactly(tmp_path):
    df = matrix()
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    frame = load_table(path)
    assert isinstance(frame, DataFrame)
    assert frame.num_rows == len(df) and frame._table_header["table"] == "t"
    # Category labels are plain values and set and list cells are JSON text.
    assert frame["cat"].to_list() == ["lo", None, "hi", "mid", "lo"]
    assert frame["sets"].to_list()[0] in ("[1,2]", "[2,1]")
    assert frame["lists"].to_list()[3] == '["a","b","a"]'
    exact = frame.to_pandas()
    keep = [c for c in df.columns if c not in ("sets", "lists")]
    pd.testing.assert_frame_equal(exact[keep], df[keep], **EQUAL)
    sets_equal_nan(exact["sets"].tolist(), df["sets"].tolist())
    sets_equal_nan(exact["lists"].tolist(), df["lists"].tolist())
    assert list(frame.to_pandas().dtypes.astype(str)) == list(df.dtypes.astype(str))


def test_index_names_and_the_native_row_index(tmp_path):
    df = pd.DataFrame(
        {"v": [1.5, 2.5, 3.5]},
        index=pd.MultiIndex.from_arrays([[0, 1, 2], ["a", "b", "c"]], names=[None, "lvl"]),
    )
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    frame = load_table(path)
    assert frame.index_names() == ["__index_level_0__", "lvl"]
    pandas_frame = frame.to_pandas()
    assert list(pandas_frame.index.names) == [None, "lvl"]
    pd.testing.assert_frame_equal(pandas_frame, df, **EQUAL)


def test_a_derived_frame_and_explicit_arguments_ignore_the_header(tmp_path):
    df = pd.DataFrame(
        {
            "i": np.array([1, 2, 3], dtype="int64"),
            "c": pd.Categorical(["a", "b", "a"], categories=["a", "b", "z"]),
            "s": pd.array(["x", "y", "z"], dtype=pd.StringDtype("python")),
        }
    )
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    frame = load_table(path)
    exact = frame.to_pandas()
    assert list(exact["c"].cat.categories) == ["a", "b", "z"]
    assert exact["s"].dtype.storage == "python"

    derived = frame.filter(frame["i"] > 1)
    assert getattr(derived, "_table_header", None) is None
    plain = derived.to_pandas()
    assert not isinstance(plain["c"].dtype, pd.CategoricalDtype)
    assert plain["i"].tolist() == [2, 3]

    assert frame.to_pandas(nullable=True)["i"].dtype == "Int64"
    assert not isinstance(frame.to_pandas(nullable=True)["c"].dtype, pd.CategoricalDtype)
    assert isinstance(frame.to_pandas(arrow=True)["i"].dtype, pd.ArrowDtype)
    assert frame.to_pandas(index="i").index.name == "i"
    totals = frame.group_by("c").sum().to_pandas().sort_values("c")
    assert totals["c"].tolist() == ["a", "b"] and totals["i"].tolist() == [4, 2]


def test_a_dftracer_frame_is_written_directly(tmp_path):
    table = pa.table(
        {
            "i": pa.array([1, None, 3]),
            "f": pa.array([1.5, math.nan, None]),
            "b": pa.array([True, None, False]),
            "s": pa.array(["a", None, "b"]),
            "d": pa.array(["u", "v", "u"]).dictionary_encode(),
            "u8": pa.array([1, 2, 3], type=pa.uint8()),
        }
    )
    frame = DataFrame.from_arrow(table)
    path = str(tmp_path / "t.pfw.gz")
    save_table(frame, path, name="native", meta={"k": 1})
    args = json.loads(lines_of(path)[0])["args"]
    assert [f["dtype"] for f in args["fields"]] == [
        "Int64",
        "Float64",
        "boolean",
        "object",
        "category",
        "uint8",
    ]
    back = load_table(path)
    for col in ("i", "f", "b", "s", "u8"):
        assert back[col].to_list() == frame[col].to_list() or col == "f"
    assert back["f"].null_count == 1 and math.isnan(back["f"].to_list()[1])
    exact = back.to_pandas()
    assert exact["f"].isna().tolist() == [False, False, True] and math.isnan(exact["f"][1])
    assert exact["i"].dtype == "Int64" and exact["u8"].dtype == "uint8"
    assert exact["d"].tolist() == ["u", "v", "u"] and list(exact["d"].cat.categories) == ["u", "v"]
    with pytest.raises(TypeError, match="which a table cannot store"):
        save_table(
            DataFrame.from_arrow(pa.table({"t": pa.array([1], type=pa.timestamp("us"))})),
            path,
            name="t",
        )


def test_a_dftracer_frame_keeps_its_row_index(tmp_path):
    frame = DataFrame.from_arrow(pa.table({"k": ["a", "b"], "v": [1, 2]}))
    frame = frame.with_index(["k"])
    path = str(tmp_path / "t.pfw.gz")
    save_table(frame, path, name="t")
    back = load_table(path)
    assert back.index_names() == ["k"]
    assert back.to_pandas().index.tolist() == ["a", "b"]


def test_every_dtype_round_trips_with_exact_dtypes(tmp_path):
    df = matrix().drop(columns=["sets", "lists"])
    _, back = assert_round_trip(df, tmp_path)
    assert list(back.dtypes.astype(str)) == list(df.dtypes.astype(str))


def test_float_column_keeps_nan_na_and_infinity_apart(tmp_path):
    df = matrix().drop(columns=["sets", "lists"])
    _, back = assert_round_trip(df, tmp_path)
    col = back["Float64"]
    assert col.isna().tolist() == [False, False, False, False, True]
    assert math.isnan(col[1]) and col[2] == math.inf and col[3] == -math.inf


def test_numpy_float_bits_are_exact(tmp_path):
    values = np.array([0.1, 1 / 3, 5e-324, 1.7976931348623157e308, -0.0, 1e16, 123456789.123456789])
    path, back = assert_round_trip(pd.DataFrame({"x": values}), tmp_path)
    assert back["x"].to_numpy().tobytes() == values.tobytes()


def test_sets_and_lists_come_back_as_their_kind(tmp_path):
    df = matrix()[["sets", "lists"]]
    path = str(tmp_path / "s.pfw.gz")
    save_table(df, path, name="t")
    back = read_pandas(path)
    sets_equal_nan(back["sets"].tolist(), df["sets"].tolist())
    sets_equal_nan(back["lists"].tolist(), df["lists"].tolist())
    fields = json.loads(lines_of(path)[0])["args"]["fields"]
    assert fields[0]["elem"] == "set" and fields[1]["elem"] == "list"


def test_multiindex_with_a_none_level_and_default_range_index(tmp_path):
    df = pd.DataFrame(
        {"v": [1.5, 2.5, 3.5, 4.5]},
        index=pd.MultiIndex.from_arrays([[0, 1, 2, 3], ["a", "b", "c", "d"]], names=[None, "lvl"]),
    )
    path, back = assert_round_trip(df, tmp_path)
    assert list(back.index.names) == [None, "lvl"]
    args = json.loads(lines_of(path)[0])["args"]
    assert args["index"] == ["__index_level_0__", "lvl"] and args["index_names"] == [None, "lvl"]

    plain = pd.DataFrame({"v": [1, 2, 3]})
    path, back = assert_round_trip(plain, tmp_path, "plain.pfw.gz")
    assert isinstance(back.index, pd.RangeIndex)
    assert json.loads(lines_of(path)[0])["args"]["index"] == []

    shifted = pd.DataFrame({"v": [1, 2, 3]}, index=pd.RangeIndex(5, 8, name="r"))
    assert_round_trip(shifted, tmp_path, "shifted.pfw.gz")


def test_empty_frames(tmp_path):
    df = pd.DataFrame(
        {
            "i": pd.Series([], dtype="int64"),
            "s": pd.Series([], dtype="string"),
            "o": pd.Series([], dtype=object),
            "c": pd.Categorical([], categories=["a", "b"]),
            "f": pd.Series([], dtype="Float64"),
        }
    )
    assert_round_trip(df, tmp_path)


def test_every_line_is_json_with_integer_ph(tmp_path):
    df = matrix()
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t", meta={"k": [1, "x"]})
    lines = lines_of(path)
    assert len(lines) == len(df) + 1
    records = [json.loads(line) for line in lines]
    assert records[0]["ph"] == 4 and type(records[0]["ph"]) is int
    assert records[0]["args"]["format"] == "dftu.table" and records[0]["args"]["meta"] == {
        "k": [1, "x"]
    }
    assert records[0]["args"]["nrows"] == len(df)
    assert all(r["ph"] == 3 for r in records[1:])
    assert [r["ts"] for r in records[1:]] == list(range(len(df)))
    assert records[1]["args"]["uint64"] == 0 and records[2]["args"]["uint64"] == U64_MAX
    assert records[2]["args"]["Float64"] == "NaN" and records[3]["args"]["Float64"] == "Infinity"
    assert records[5]["args"]["Float64"] is None


def test_plain_path_is_not_compressed(tmp_path):
    df = pd.DataFrame({"a": [1, 2]})
    path = str(tmp_path / "t.pfw")
    save_table(df, path, name="t")
    assert lines_of(path)[0].startswith("{")
    pd.testing.assert_frame_equal(read_pandas(path), df, **EQUAL)


def test_dftracer_dataframe_is_accepted(tmp_path):
    df = pd.DataFrame({"a": [1, 2, 3], "s": ["x", "y", "z"]})
    path = str(tmp_path / "t.pfw.gz")
    save_table(DataFrame.from_pandas(df), path, name="t")
    back = read_pandas(path)
    assert back["a"].tolist() == [1, 2, 3] and back["s"].tolist() == ["x", "y", "z"]


def test_refusals(tmp_path):
    path = str(tmp_path / "x.pfw.gz")
    with pytest.raises(TypeError):
        save_table(pd.DataFrame({"t": pd.to_datetime(["2020-01-01"])}), path, name="t")
    with pytest.raises(TypeError):
        save_table(pd.DataFrame({"t": pd.to_timedelta([1], unit="s")}), path, name="t")
    with pytest.raises(TypeError):
        save_table(pd.DataFrame({"t": pd.Series([1, "a"], dtype=object)}), path, name="t")
    with pytest.raises(TypeError):
        save_table(pd.DataFrame({"t": pd.Series([{"a": 1}], dtype=object)}), path, name="t")
    with pytest.raises(TypeError):
        save_table(pd.DataFrame({"t": pd.Series([[{1}]], dtype=object)}), path, name="t")
    with pytest.raises(TypeError, match="NaN"):
        save_table(pd.DataFrame({"t": pd.Series([[math.nan]], dtype=object)}), path, name="t")
    with pytest.raises(ValueError, match="mixes sets and lists"):
        save_table(pd.DataFrame({"t": pd.Series([{1}, [2]], dtype=object)}), path, name="t")
    with pytest.raises(ValueError, match="duplicate"):
        save_table(pd.DataFrame([[1, 2]], columns=["a", "a"]), path, name="t")
    with pytest.raises(TypeError):
        save_table([1, 2, 3], path, name="t")
    with pytest.raises(ValueError):
        save_table(pd.DataFrame({"a": [1]}), path, name="t", meta={"x": math.nan})
    assert not os.path.exists(path)


def test_reader_refusals(tmp_path):
    good = pd.DataFrame({"a": [1, 2]})
    path = str(tmp_path / "t.pfw")
    save_table(good, path, name="t")
    lines = lines_of(path)
    header = json.loads(lines[0])
    bad = tmp_path / "bad.pfw"

    header["args"]["format_version"] = 2
    bad.write_text(json.dumps(header) + "\n" + "\n".join(lines[1:]) + "\n")
    with pytest.raises(dftu.errors.DFTUtilsParseError, match="format_version"):
        read_pandas(str(bad))

    header["args"]["format_version"] = 1
    header["args"]["nrows"] = 5
    bad.write_text(json.dumps(header) + "\n" + "\n".join(lines[1:]) + "\n")
    with pytest.raises(dftu.errors.DFTUtilsParseError, match="rows"):
        read_pandas(str(bad))

    header["args"]["nrows"] = 2
    bad.write_text(
        json.dumps(header) + "\n" + lines[1] + "\n" + json.dumps({"ph": 3, "args": {"b": 1}}) + "\n"
    )
    with pytest.raises(dftu.errors.DFTUtilsParseError, match="field"):
        read_pandas(str(bad))

    bad.write_text(
        json.dumps(header)
        + "\n"
        + lines[1]
        + "\n"
        + json.dumps({"ph": 3, "args": {"a": "x"}})
        + "\n"
    )
    with pytest.raises(dftu.errors.DFTUtilsParseError):
        read_pandas(str(bad))


def test_several_pandas_frames_are_one_file(tmp_path):
    path = str(tmp_path / "w.pfw.gz")
    parts = [
        pd.DataFrame({"a": np.arange(3, dtype="int64") + 3 * i, "s": ["x", "y", None]})
        for i in range(3)
    ]
    save_table(pd.concat(parts, ignore_index=True), path, name="t", meta={"parts": 3})
    back = read_pandas(path)
    pd.testing.assert_frame_equal(back, pd.concat(parts, ignore_index=True), **EQUAL)
    header = json.loads(lines_of(path)[0])
    assert header["args"]["nrows"] == 9 and header["ph"] == 4


def test_native_frames_with_an_index_are_one_file(tmp_path):
    path = str(tmp_path / "w.pfw.gz")
    parts = [
        DataFrame.from_arrow(pa.table({"k": [i, i + 1], "v": [float(i), None]})) for i in (0, 10)
    ]
    flat = [_ext.DataFrame.with_index(p, []) for p in parts]
    whole = DataFrame(flat[0]).concat(DataFrame(flat[1])).with_index(["k"])
    whole.write_table(path, name="t")
    back = load_table(path)
    assert back.index_names() == ["k"] and back.num_rows == 4
    assert back["k"].to_list() == [0, 1, 10, 11]
    assert json.loads(lines_of(path)[0])["args"]["nrows"] == 4


def test_pandas_frames_with_an_index_are_one_file(tmp_path):
    path = str(tmp_path / "w.pfw.gz")
    a = pd.DataFrame({"v": [1.5]}, index=pd.Index(["p"], name="k"))
    b = pd.DataFrame({"v": [2.5]}, index=pd.Index(["q"], name="k"))
    save_table(pd.concat([a, b]), path, name="t")
    pd.testing.assert_frame_equal(read_pandas(path), pd.concat([a, b]), **EQUAL)


def test_a_failed_write_leaves_nothing(tmp_path):
    path = str(tmp_path / "w.pfw.gz")
    with pytest.raises(TypeError):
        save_table(pd.DataFrame({"t": pd.to_timedelta([1], unit="s")}), path, name="t")
    assert os.listdir(tmp_path) == []


def test_a_folder_of_parts_round_trips(tmp_path):
    df = pd.DataFrame({"a": np.arange(10, dtype="int64"), "s": [f"v{i}" for i in range(10)]})
    folder = str(tmp_path / "parts")
    summary = DataFrame.from_pandas(df).write_table(folder, name="t", part_size=4)
    assert (summary.rows, summary.part_count, summary.kind) == (10, 3, "records")
    files = sorted(f for f in os.listdir(folder) if f.startswith("part-"))
    assert files == ["part-00000.pfw.gz", "part-00001.pfw.gz", "part-00002.pfw.gz"]
    for f in files:
        assert json.loads(lines_of(os.path.join(folder, f))[0])["ph"] == 4
    back = read_pandas(folder)
    pd.testing.assert_frame_equal(back, df, **EQUAL)
    with pytest.raises(FileExistsError):
        DataFrame.from_pandas(df).write_table(folder, name="t", part_size=4)


def test_folder_parts_must_agree(tmp_path):
    save_table(pd.DataFrame({"a": [1]}), str(tmp_path / "part-0.pfw.gz"), name="t")
    save_table(pd.DataFrame({"b": [1]}), str(tmp_path / "part-1.pfw.gz"), name="t")
    with pytest.raises(ValueError, match="differ"):
        read_pandas(str(tmp_path))
    empty = tmp_path / "empty"
    empty.mkdir()
    assert load_table(str(empty)).num_rows == 0


def rewrite(path, old, new, count=1):
    """The plain file at ``path`` with ``old`` replaced by ``new`` in its text."""
    text = open(path, encoding="utf-8").read()
    assert old in text
    with open(path, "w", encoding="utf-8") as f:
        f.write(text.replace(old, new, count))


def test_large_integer_labels_stay_exact_with_a_null(tmp_path):
    labels = [2**53, 2**53 + 1, 2**62 + 1]
    cat = pd.Categorical([2**53 + 1, None, 2**62 + 1, 2**53], categories=labels)
    df = pd.DataFrame({"c": cat})
    assert_round_trip(df, tmp_path)
    back = read_pandas(str(tmp_path / "t.pfw.gz"))
    assert back["c"].cat.codes.tolist() == [1, -1, 2, 0]


def test_a_value_that_is_no_category_is_an_error(tmp_path):
    df = pd.DataFrame({"c": pd.Categorical(["a", "b", None], categories=["a", "b"])})
    path = str(tmp_path / "c.pfw")
    save_table(df, path, name="t")
    pd.testing.assert_frame_equal(read_pandas(path), df, **EQUAL)
    rewrite(path, '"c":"b"', '"c":"zzz"')
    with pytest.raises(ValueError, match="'c'.*zzz"):
        read_pandas(path)
    nums = pd.DataFrame({"c": pd.Categorical([1, 2], categories=[1, 2])})
    save_table(nums, path, name="t")
    rewrite(path, '"c":2', '"c":3')
    with pytest.raises(ValueError, match="'c'"):
        read_pandas(path)


def test_category_nan_rules(tmp_path):
    df = pd.DataFrame({"c": pd.Categorical(["a", None, "NaN"], categories=["a", "NaN"])})
    path = str(tmp_path / "c.pfw")
    save_table(df, path, name="t")
    back = read_pandas(path)
    assert (
        back["c"].tolist()[0] == "a"
        and back["c"].tolist()[2] == "NaN"
        and pd.isna(back["c"].tolist()[1])
    )
    # A file without categories_dtype takes a "NaN" value that is no label as missing.
    plain = pd.DataFrame({"c": pd.Categorical(["a", None], categories=["a"])})
    save_table(plain, path, name="t")
    text = re.sub(r',"categories_dtype":"[^"]*"', "", open(path, encoding="utf-8").read())
    assert "categories_dtype" not in text
    open(path, "w", encoding="utf-8").write(text)
    rewrite(path, '"c":null', '"c":"NaN"')
    assert pd.isna(read_pandas(path)["c"].tolist()[1])
    # With categories_dtype the same value is an error.
    save_table(plain, path, name="t")
    rewrite(path, '"c":null', '"c":"NaN"')
    with pytest.raises(ValueError, match="'c'"):
        read_pandas(path)
    # NaN and Infinity decode to floats only when the categories are floats.
    floats = pd.DataFrame({"c": pd.Categorical([1.5, None], categories=[1.5, 2.5])})
    save_table(floats, path, name="t")
    assert read_pandas(path)["c"].cat.categories.dtype == np.float64


@pytest.mark.parametrize("dtype", ["float64", "float32", "Float64"])
def test_infinite_float_category_labels_round_trip(tmp_path, dtype):
    cats = pd.Index([1.5, math.inf, -math.inf], dtype=dtype)
    df = pd.DataFrame(
        {"c": pd.Categorical([1.5, None, math.inf, -math.inf, math.inf], categories=cats)}
    )
    path = str(tmp_path / "inf.pfw.gz")
    save_table(df, path, name="t")
    field = json.loads(lines_of(path)[0])["args"]["fields"][0]
    assert field["categories"] == [1.5, "Infinity", "-Infinity"]
    back = read_pandas(path)
    pd.testing.assert_series_equal(back["c"], df["c"], **{"check_index_type": False})


def test_unsupported_category_labels_are_refused(tmp_path):
    path = str(tmp_path / "x.pfw")
    big = pd.DataFrame({"c": pd.Categorical([1], categories=pd.Index([1, 2**63], dtype="uint64"))})
    with pytest.raises(ValueError, match="2\\*\\*63"):
        save_table(big, path, name="t")
    flags = pd.DataFrame({"c": pd.Categorical([True], categories=[True, False])})
    with pytest.raises(TypeError, match="boolean"):
        save_table(flags, path, name="t")


def test_nan_in_an_object_column_reads_back_as_none(tmp_path):
    df = pd.DataFrame({"o": pd.Series(["a", math.nan, None], dtype=object)})
    path = str(tmp_path / "o.pfw")
    save_table(df, path, name="t")
    assert read_pandas(path)["o"].tolist() == ["a", None, None]


def test_non_string_index_names_and_collisions(tmp_path):
    df = pd.DataFrame({"v": [1, 2]}, index=pd.Index([10, 20], name=0))
    path = str(tmp_path / "i.pfw.gz")
    save_table(df, path, name="t")
    assert json.loads(lines_of(path)[0])["args"]["index"] == ["0"]
    back = read_pandas(path)
    assert list(back.index.names) == [0] and back["v"].tolist() == [1, 2]

    clash = pd.DataFrame({"__index_level_0__": [1]}, index=pd.Index([5]))
    with pytest.raises(ValueError, match="collides"):
        save_table(clash, path, name="t")
    named = pd.DataFrame({"k": [1]}, index=pd.Index([5], name="k"))
    with pytest.raises(ValueError, match="collide"):
        save_table(named, path, name="t")
    with pytest.raises(TypeError, match="index level name"):
        save_table(pd.DataFrame({"v": [1]}, index=pd.Index([5], name=("a", "b"))), path, name="t")


def test_a_set_cell_that_is_no_array_is_an_error(tmp_path):
    df = pd.DataFrame({"s": pd.Series([{1}, {2}], dtype=object)})
    path = str(tmp_path / "s.pfw")
    save_table(df, path, name="t")
    rewrite(path, '"s":[2]', '"s":5')
    with pytest.raises(ValueError, match="'s'.*array"):
        read_pandas(path)


def test_folder_parts_may_differ_in_content_but_not_in_names_and_dtypes(tmp_path):
    folder = tmp_path / "parts"
    folder.mkdir()
    save_table(
        pd.DataFrame({"c": pd.Categorical(["a", "b"], categories=["a", "b"])}),
        str(folder / "part-0.pfw.gz"),
        name="t",
    )
    save_table(
        pd.DataFrame({"c": pd.Categorical(["z", "a"], categories=["z", "a"])}),
        str(folder / "part-1.pfw.gz"),
        name="t",
    )
    back = read_pandas(str(folder))
    assert back["c"].tolist() == ["a", "b", "z", "a"]
    assert list(back["c"].cat.categories) == ["a", "b", "z"]


def test_folder_parts_must_agree_on_elem_and_order(tmp_path):
    folder = tmp_path / "parts"
    folder.mkdir()
    save_table(
        pd.DataFrame({"o": pd.Series([None, None], dtype=object)}),
        str(folder / "part-0.pfw.gz"),
        name="t",
    )
    save_table(
        pd.DataFrame({"o": pd.Series([{1}, {2, 3}], dtype=object)}),
        str(folder / "part-1.pfw.gz"),
        name="t",
    )
    with pytest.raises(ValueError, match="differ.*elem"):
        read_pandas(str(folder))
    ordered = tmp_path / "ordered"
    ordered.mkdir()
    for n, flag in enumerate((False, True)):
        save_table(
            pd.DataFrame({"c": pd.Categorical(["a"], categories=["a", "b"], ordered=flag)}),
            str(ordered / f"part-{n}.pfw.gz"),
            name="t",
        )
    with pytest.raises(ValueError, match="differ.*ordered"):
        read_pandas(str(ordered))


def test_a_partitioned_failure_leaves_nothing(tmp_path):
    folder = tmp_path / "parts"
    part = pd.DataFrame({"a": np.arange(4)}).assign(t=pd.to_timedelta(np.arange(4), unit="s"))
    with pytest.raises(TypeError):
        DataFrame.from_pandas(part).write_table(str(folder), part_size=2)
    assert not folder.exists() and os.listdir(tmp_path) == []


def doc_examples(*parts):
    root = os.path.join(os.path.dirname(__file__), "..", "..", "docs", "source", *parts)
    text = open(root, encoding="utf-8").read().splitlines()
    blocks, i = [], 0
    while i < len(text):
        if text[i].strip() == ".. code-block:: python":
            i += 1
            body = []
            while i < len(text) and (not text[i].strip() or text[i].startswith("   ")):
                body.append(text[i][3:])
                i += 1
            blocks.append("\n".join(body))
        else:
            i += 1
    return blocks


@pytest.mark.parametrize("parts", [("api", "table.rst"), ("guides", "data", "save-tables.rst")])
def test_documented_examples_run(tmp_path, monkeypatch, parts):
    monkeypatch.chdir(tmp_path)
    scope = {}
    blocks = doc_examples(*parts)
    assert blocks
    for block in blocks:
        exec(compile(block, "-".join(parts), "exec"), scope)  # noqa: S102


def load_reference():
    if not REFERENCE or not os.path.exists(REFERENCE):
        pytest.skip("set DFTRACER_UTILS_DFANALYZER_TABLE_REF to DFAnalyzer's checkpoint_table.py")
    spec = importlib.util.spec_from_file_location("_dfanalyzer_checkpoint_table", REFERENCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_arrow_types_that_cannot_be_rebuilt_are_refused(tmp_path):
    path = str(tmp_path / "a.pfw")
    odd = pd.DataFrame({"d": pd.array([pa.scalar(1, pa.int8())], dtype=pd.ArrowDtype(pa.int8()))})
    save_table(odd, path, name="t")
    frame = pd.DataFrame({"d": pd.Series([None], dtype=pd.ArrowDtype(pa.timestamp("ns")))})
    with pytest.raises(TypeError):
        save_table(frame, path, name="t")
    # A list column is stored as JSON arrays: the values come back, as object lists.
    lists = pd.DataFrame(
        {"d": pd.Series([[1, 2], None], dtype=pd.ArrowDtype(pa.list_(pa.int64())))}
    )
    save_table(lists, path, name="t")
    assert TraceViewer(path).collect().to_pandas()["d"].tolist() == [[1, 2], None]


def test_dfanalyzer_reads_our_files_and_we_read_theirs(tmp_path):
    ref = load_reference()
    df = matrix()
    df.index = pd.MultiIndex.from_arrays([np.arange(len(df)), list("abcde")], names=[None, "lvl"])
    ours = str(tmp_path / "ours.pfw.gz")
    save_table(df, ours, name="t")
    theirs_read = ref.read_table(ours)
    # DFAnalyzer's reader keeps a category column's labels but not their dtype.
    cats = [c for c in df.columns if isinstance(df[c].dtype, pd.CategoricalDtype)]
    rest = [c for c in df.columns if c not in cats and c not in ("sets", "lists")]
    pd.testing.assert_frame_equal(theirs_read[rest], df[rest], **EQUAL)
    for c in cats:
        assert theirs_read[c].cat.codes.tolist() == df[c].cat.codes.tolist(), c
        assert list(theirs_read[c].cat.categories) == list(df[c].cat.categories), c
        assert theirs_read[c].cat.ordered == df[c].cat.ordered
    sets_equal_nan(theirs_read["sets"].tolist(), df["sets"].tolist())

    theirs = str(tmp_path / "theirs.pfw.gz")
    ref.write_table(df, theirs, "t")
    back = read_pandas(theirs)
    pd.testing.assert_frame_equal(
        back.drop(columns=["sets", "lists"]), df.drop(columns=["sets", "lists"]), **EQUAL
    )
    sets_equal_nan(back["sets"].tolist(), df["sets"].tolist())
    sets_equal_nan(back["lists"].tolist(), df["lists"].tolist())


def text_parts(folder):
    folder.mkdir()
    parts = [
        (["x", "y", "x"], ["p", None, "q"], [{1}, {2, 3}, set()], ["a", None, "b"]),
        (["y", "x"], [None, "r"], [{4}, set()], ["c", "d"]),
    ]
    for k, (c, o, s, sp) in enumerate(parts):
        df = pd.DataFrame(
            {
                "c": pd.Categorical(c, categories=["x", "y"]),
                "o": pd.Series(o, dtype=object),
                "set": pd.Series(s, dtype=object),
                "sp": pd.array(sp, dtype=pd.StringDtype("python")),
            }
        )
        save_table(df, str(folder / f"part-{k}.pfw.gz"), name="t")


def test_a_folder_with_text_columns_restores_exactly(tmp_path):
    folder = tmp_path / "parts"
    text_parts(folder)
    back = load_table(str(folder)).to_pandas()
    assert isinstance(back["c"].dtype, pd.CategoricalDtype)
    assert back["c"].astype(object).tolist() == ["x", "y", "x", "y", "x"]
    assert back["o"].tolist() == ["p", None, "q", None, "r"]
    assert back["set"].tolist() == [{1}, {2, 3}, set(), {4}, set()]
    assert back["sp"].dtype == pd.StringDtype("python")
    assert back["sp"].tolist() == ["a", pd.NA, "b", "c", "d"]


def test_a_folder_of_parts_keeps_a_string_category(tmp_path):
    df = pd.DataFrame({"a": np.arange(8, dtype="int64"), "c": pd.Categorical(list("abababab"))})
    folder = str(tmp_path / "parts")
    DataFrame.from_pandas(df).write_table(folder, name="t", part_size=4)
    back = load_table(folder).to_pandas()
    assert isinstance(back["c"].dtype, pd.CategoricalDtype)
    assert back["c"].astype(object).tolist() == list("abababab")


def stale_frame(tmp_path):
    df = pd.DataFrame(
        {
            "a": np.array([1, 2, 3], dtype="int32"),
            "s": pd.Categorical(["x", "y", "x"]),
            "o": pd.Series(["p", "q", "r"], dtype=object),
        }
    )
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    return load_table(path)


def test_a_frame_changed_in_place_drops_its_header(tmp_path):
    f = stale_frame(tmp_path)
    f["a"] = f["a"] * 2.5
    assert not hasattr(f, "_table_header")
    assert f.to_pandas()["a"].tolist() == [2.5, 5.0, 7.5]

    g = stale_frame(tmp_path)
    g.loc[0, "a"] = None
    assert not hasattr(g, "_table_header")
    assert g.to_pandas()["a"].isna().tolist() == [True, False, False]

    h = stale_frame(tmp_path)
    h.loc[0, "a"] = 2**20
    assert h.to_pandas()["a"].tolist()[0] == 2**20
    with pytest.raises(TypeError, match="int32 column"):
        h.loc[0, "a"] = 2**40

    k = stale_frame(tmp_path)
    k["c"] = k["s"]
    assert not hasattr(k, "_table_header")
    assert k.to_pandas()["c"].tolist() == ["x", "y", "x"]


def test_a_new_frame_has_no_header_and_the_same_frame_keeps_it(tmp_path):
    f = stale_frame(tmp_path)
    assert hasattr(f.assign(z=1), "_table_header") is False
    assert hasattr(f.sort_index(), "_table_header")
    assert hasattr(f.iloc[:], "_table_header")


def test_a_header_that_no_longer_fits_a_column_is_ignored(tmp_path):
    f = stale_frame(tmp_path)
    f._table_header = {
        **f._table_header,
        "fields": [
            dict(x, dtype="int8") if x["name"] == "a" else x for x in f._table_header["fields"]
        ],
    }
    big = DataFrame({"a": [2**40, 1, 2], "s": ["x", "y", "x"], "o": ["p", "q", "r"]})
    big._table_header = f._table_header
    assert big.to_pandas()["a"].tolist() == [2**40, 1, 2]
    floats = DataFrame({"a": [1.5, 2.0, 3.0], "s": ["x", "y", "x"], "o": ["p", "q", "r"]})
    floats._table_header = f._table_header
    assert floats.to_pandas()["a"].tolist() == [1.5, 2.0, 3.0]


def test_a_native_frame_keeps_nan_apart_from_null_on_write(tmp_path):
    df = DataFrame({"x": [1.0, math.nan, None, 2.5]})
    path = str(tmp_path / "t.pfw.gz")
    save_table(df, path, name="t")
    back = load_table(path)
    values = back["x"].to_list()
    assert values[0] == 1.0 and math.isnan(values[1]) and values[2] is None and values[3] == 2.5


def test_from_pandas_describes_the_frame_without_pyarrow():
    code = """
import sys
sys.modules["pyarrow"] = None
import numpy as np
import pandas as pd
from dftracer.utils import DataFrame

df = pd.DataFrame(
    {
        "i8": np.arange(3, dtype="int8"),
        "u32": np.arange(3, dtype="uint32"),
        "f32": np.arange(3, dtype="float32"),
        "n": pd.array([1, None, 3], dtype="Int16"),
        "b": pd.array([True, None, False], dtype="boolean"),
        "s": ["a", None, "c"],
        "c": pd.Categorical(["x", "y", None], categories=["y", "x"], ordered=True),
        "sets": pd.Series([{1}, set(), None], dtype=object),
    }
)
df.index = pd.Index([5, 6, 7], name="k")
header = DataFrame.from_pandas(df)._table_header
fields = {f["name"]: f for f in header["fields"]}
assert header["index"] == ["k"] and header["nrows"] == 3
assert fields["i8"]["dtype"] == "int8" and fields["u32"]["dtype"] == "uint32"
assert fields["f32"]["dtype"] == "float32" and fields["n"]["dtype"] == "Int16"
assert fields["b"]["dtype"] == "boolean"
assert fields["c"]["categories"] == ["y", "x"] and fields["c"]["ordered"] is True
assert fields["sets"]["elem"] == "set"
dated = pd.DataFrame({"t": pd.to_datetime(["2020-01-01"])})
assert not hasattr(DataFrame.from_pandas(dated), "_table_header")
"""
    result = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


class TaggedSet(set):
    """A set subclass, as DFAnalyzer's BetterSet is one."""


@pytest.mark.parametrize("make", [frozenset, TaggedSet, sorted])
def test_set_type_builds_each_set_cell(tmp_path, make):
    df = pd.DataFrame(
        {"s": pd.Series([{1, 2}, set(), None, {"a"}], dtype=object), "v": [1, 2, 3, 4]}
    )
    path = str(tmp_path / "s.pfw.gz")
    save_table(df, path, name="t")
    cells = load_table(path).to_pandas(set_type=make)["s"].tolist()
    assert cells[2] is None
    assert [type(c) for c in cells if c is not None] == [type(make([]))] * 3
    assert [set(c) for c in cells if c is not None] == [{1, 2}, set(), {"a"}]


def test_set_type_refuses_the_arguments_that_skip_the_header(tmp_path):
    df = pd.DataFrame({"s": pd.Series([{1}], dtype=object)})
    path = str(tmp_path / "s.pfw.gz")
    save_table(df, path, name="t")
    frame = load_table(path)
    with pytest.raises(ValueError, match="set_type"):
        frame.to_pandas(arrow=True, set_type=frozenset)
    with pytest.raises(ValueError, match="set_type"):
        frame.to_pandas(nullable=True, set_type=frozenset)
    with pytest.raises(ValueError, match="set_type"):
        DataFrame({"v": [1, 2]}).to_pandas(set_type=frozenset)
