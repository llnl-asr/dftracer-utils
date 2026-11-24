#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test cases for the table reader behind TraceViewer and the table writer behind DataFrame.write."""

import gzip
import importlib.util
import json
import math
import os

import pytest

np = pytest.importorskip("numpy")
pd = pytest.importorskip("pandas")
pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame, TraceWriteOptions  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402
from dftracer.utils._sink import run_sink  # noqa: E402
from dftracer.utils.trace import TraceViewer  # noqa: E402

DATA = os.path.join(os.path.dirname(__file__), "data")
REFERENCE = os.environ.get("DFTRACER_UTILS_DFANALYZER_TABLE_REF")
U64_MAX = 2**64 - 1
I64_MIN = -(2**63)


def header(nrows, fields, table="t"):
    return json.dumps(
        {
            "format": "dftu.table",
            "format_version": _ext.TABLE_FORMAT_VERSION,
            "table": table,
            "nrows": nrows,
            "index": [],
            "index_names": [],
            "fields": fields,
            "meta": {},
        }
    )


def save_table(frame, path, hdr, raw_columns, level, member_size):
    """Write ``frame`` as a table with the header ``hdr`` through the sink writer."""
    return run_sink(
        frame.lazy(),
        path,
        header=json.loads(hdr),
        known_rows=frame.num_rows,
        raw_columns=tuple(raw_columns),
        mode="overwrite",
        kind="records",
        options=TraceWriteOptions(level=level, member_size=member_size, build_index=False),
    )


def read(path):
    """The frame TraceViewer reads from a table file and its table header."""
    frame = TraceViewer(path).collect()
    return frame, frame._table_header


def sample():
    table = pa.table(
        {
            "i": pa.array(pd.array([I64_MIN, -1, None, 2**63 - 1], dtype="Int64")),
            "u": pa.array([0, U64_MAX, 1 << 63, 7], type=pa.uint64()),
            "f": pa.array([1.5, math.nan, None, math.inf], type=pa.float64(), from_pandas=False),
            "b": pa.array([True, None, False, True]),
            "s": pa.array(['q"uote', "line\nbreak", None, "é中"]),
            "raw": pa.array(["[1,2]", "[]", None, '["a",{"x":null}]']),
        }
    )
    fields = [
        {"name": "i", "dtype": "Int64"},
        {"name": "u", "dtype": "UInt64"},
        {"name": "f", "dtype": "Float64"},
        {"name": "b", "dtype": "boolean"},
        {"name": "s", "dtype": "string"},
        {"name": "raw", "dtype": "object", "elem": "set"},
    ]
    return table, header(table.num_rows, fields)


def float64_with_nan_and_na():
    """A pandas Float64 column holding 1.5, NaN and NA (the constructors turn NaN into NA)."""
    values = np.array([1.5, math.nan, 0.0])
    return pd.arrays.FloatingArray(values, np.array([False, False, True]))


def norm(values):
    return ["NaN" if isinstance(v, float) and math.isnan(v) else v for v in values]


def lines_of(path):
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as f:
        return f.read().splitlines()


def load_reference():
    if not REFERENCE or not os.path.exists(REFERENCE):
        return None
    spec = importlib.util.spec_from_file_location("_dfanalyzer_checkpoint_table", REFERENCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("name", ["t.pfw.gz", "t.pfw"])
def test_round_trip_keeps_values_and_masks(tmp_path, name):
    table, hdr = sample()
    path = str(tmp_path / name)
    save_table(DataFrame.from_arrow(table), path, hdr, ["raw"], 6, 0)
    frame, back_header = read(path)
    assert back_header == json.loads(hdr)
    got = frame.to_arrow()
    assert got.column_names == table.column_names
    for col in table.column_names:
        assert norm(got.column(col).to_pylist()) == norm(table.column(col).to_pylist()), col


def test_every_line_is_json_with_integer_ph(tmp_path):
    table, hdr = sample()
    path = str(tmp_path / "t.pfw.gz")
    save_table(DataFrame.from_arrow(table), path, hdr, ["raw"], 1, 2)
    lines = lines_of(path)
    assert len(lines) == table.num_rows + 1
    records = [json.loads(line) for line in lines]
    assert records[0]["ph"] == 4 and records[0]["args"]["format"] == "dftu.table"
    assert all(r["ph"] == 3 for r in records[1:])
    assert records[1]["args"]["raw"] == [1, 2]
    assert records[1]["args"]["u"] == 0 and records[2]["args"]["u"] == U64_MAX
    assert records[2]["args"]["f"] == "NaN" and records[4]["args"]["f"] == "Infinity"
    assert records[3]["args"]["f"] is None


def _version_file(tmp_path, version):
    path = tmp_path / "v.pfw"
    args = json.loads(header(0, []))
    args["format_version"] = version
    path.write_text(json.dumps({"ph": 4, "name": "dftu.table", "args": args}) + "\n")
    return str(path)


def test_the_engines_major_with_a_lower_or_equal_minor_reads(tmp_path):
    major, minor = (int(p) for p in _ext.TABLE_FORMAT_VERSION.split("."))
    for version in (f"{major}.{minor}", f"{major}.0", major):
        frame, _ = read(_version_file(tmp_path, version))
        assert frame.num_rows == 0


@pytest.mark.parametrize("delta", ["higher_minor", "other_major", "lower_major"])
def test_another_major_or_a_higher_minor_is_refused(tmp_path, delta):
    major, minor = (int(p) for p in _ext.TABLE_FORMAT_VERSION.split("."))
    version = {
        "higher_minor": f"{major}.{minor + 1}",
        "other_major": f"{major + 1}.0",
        "lower_major": f"{major - 1}.0",
    }[delta]
    with pytest.raises(Exception, match="unknown dftu.table format_version") as err:
        read(_version_file(tmp_path, version))
    assert version in str(err.value) and _ext.TABLE_FORMAT_VERSION in str(err.value)


@pytest.mark.parametrize("version", ["1", "1.x", "1.0.0", "", None, 1.5, [1, 0]])
def test_a_malformed_version_is_refused(tmp_path, version):
    with pytest.raises(Exception, match="unknown dftu.table format_version"):
        read(_version_file(tmp_path, version))


def test_reads_a_dfanalyzer_v1_file():
    frame, args = read(os.path.join(DATA, "dfanalyzer_table_v1.pfw.gz"))
    assert args["index_names"] == [None, "lvl"]
    got = frame.to_arrow()
    assert got.column_names == [f["name"] for f in args["fields"]]
    assert got.num_rows == args["nrows"]
    assert got.column("__index_level_0__").to_pylist() == [0, 1, 2]
    assert norm(got.column("x").to_pylist()) == [1.5, "NaN", None]
    assert [json.loads(t) for t in got.column("tags").to_pylist()] == [[1, 2], [], ["a"]]


def test_dfanalyzer_reads_our_file(tmp_path):
    ref = load_reference()
    if ref is None:
        pytest.skip("set DFTRACER_UTILS_DFANALYZER_TABLE_REF to DFAnalyzer's checkpoint_table.py")
    df = pd.DataFrame(
        {
            "i": pd.array([1, None, 3], dtype="Int64"),
            "f": float64_with_nan_and_na(),
            "s": pd.array(["a", None, "c"], dtype="string"),
        }
    )
    fields = [
        {"name": "i", "dtype": "Int64"},
        {"name": "f", "dtype": "Float64"},
        {"name": "s", "dtype": "string"},
    ]
    path = str(tmp_path / "ours.pfw.gz")
    table = pa.table(
        {
            "i": pa.array(df["i"]),
            "f": pa.array([1.5, math.nan, None], type=pa.float64(), from_pandas=False),
            "s": pa.array(df["s"]),
        }
    )
    save_table(DataFrame.from_arrow(table), path, header(3, fields), [], 6, 0)
    back = ref.read_table(path)
    pd.testing.assert_frame_equal(back.drop(columns="f"), df.drop(columns="f"), check_dtype=True)
    assert str(back["f"].dtype) == "Float64"
    assert back["f"].isna().tolist() == [False, False, True]
    assert math.isnan(back["f"][1])


def test_we_read_dfanalyzer_written_file(tmp_path):
    ref = load_reference()
    if ref is None:
        pytest.skip("set DFTRACER_UTILS_DFANALYZER_TABLE_REF to DFAnalyzer's checkpoint_table.py")
    df = pd.DataFrame(
        {"i": pd.array([1, None, 3], dtype="Int64"), "f": float64_with_nan_and_na()},
        index=pd.MultiIndex.from_arrays([[0, 1, 2], ["a", "b", "c"]], names=[None, "lvl"]),
    )
    path = str(tmp_path / "ref.pfw.gz")
    ref.write_table(df, path, "t")
    frame, hdr = read(path)
    got = frame.to_arrow()
    assert got.column_names == ["__index_level_0__", "lvl", "i", "f"]
    assert got.column("i").to_pylist() == [1, None, 3]
    assert norm(got.column("f").to_pylist()) == [1.5, "NaN", None]
