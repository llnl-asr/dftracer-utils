#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test cases for write_table, read_table and TableWriter."""

import gzip
import importlib.util
import json
import math
import os
import re

import pytest

np = pytest.importorskip("numpy")
pd = pytest.importorskip("pandas")
pa = pytest.importorskip("pyarrow")

import dftracer.utils as dftu  # noqa: E402
from dftracer.utils import DataFrame, TableWriter, read_table, write_table  # noqa: E402

REFERENCE = os.environ.get("DFTRACER_UTILS_DFANALYZER_TABLE_REF")
EQUAL = dict(check_dtype=True, check_index_type=False, rtol=0)
U64_MAX = 2**64 - 1


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
    write_table(df, path, name="t", **kwargs)
    back = read_table(path)
    pd.testing.assert_frame_equal(back, df, **EQUAL)
    return path, back


def lines_of(path):
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as f:
        return f.read().splitlines()


def test_exported():
    for name in ("write_table", "read_table", "TableWriter"):
        assert name in dftu.__all__
        assert hasattr(dftu, name)


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
    write_table(df, path, name="t")
    back = read_table(path)
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
    write_table(df, path, name="t", meta={"k": [1, "x"]})
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
    write_table(df, path, name="t")
    assert lines_of(path)[0].startswith("{")
    pd.testing.assert_frame_equal(read_table(path), df, **EQUAL)


def test_dftracer_dataframe_is_accepted(tmp_path):
    df = pd.DataFrame({"a": [1, 2, 3], "s": ["x", "y", "z"]})
    path = str(tmp_path / "t.pfw.gz")
    write_table(DataFrame.from_pandas(df), path, name="t")
    back = read_table(path)
    assert back["a"].tolist() == [1, 2, 3] and back["s"].tolist() == ["x", "y", "z"]


def test_refusals(tmp_path):
    path = str(tmp_path / "x.pfw.gz")
    with pytest.raises(TypeError):
        write_table(pd.DataFrame({"t": pd.to_datetime(["2020-01-01"])}), path, name="t")
    with pytest.raises(TypeError):
        write_table(pd.DataFrame({"t": pd.to_timedelta([1], unit="s")}), path, name="t")
    with pytest.raises(TypeError):
        write_table(pd.DataFrame({"t": pd.Series([1, "a"], dtype=object)}), path, name="t")
    with pytest.raises(TypeError):
        write_table(pd.DataFrame({"t": pd.Series([{"a": 1}], dtype=object)}), path, name="t")
    with pytest.raises(TypeError):
        write_table(pd.DataFrame({"t": pd.Series([[{1}]], dtype=object)}), path, name="t")
    with pytest.raises(TypeError, match="NaN"):
        write_table(pd.DataFrame({"t": pd.Series([[math.nan]], dtype=object)}), path, name="t")
    with pytest.raises(ValueError, match="mixes sets and lists"):
        write_table(pd.DataFrame({"t": pd.Series([{1}, [2]], dtype=object)}), path, name="t")
    with pytest.raises(ValueError, match="duplicate"):
        write_table(pd.DataFrame([[1, 2]], columns=["a", "a"]), path, name="t")
    with pytest.raises(TypeError):
        write_table([1, 2, 3], path, name="t")
    with pytest.raises(ValueError):
        write_table(pd.DataFrame({"a": [1]}), path, name="t", meta={"x": math.nan})
    assert not os.path.exists(path)


def test_reader_refusals(tmp_path):
    good = pd.DataFrame({"a": [1, 2]})
    path = str(tmp_path / "t.pfw")
    write_table(good, path, name="t")
    lines = lines_of(path)
    header = json.loads(lines[0])
    bad = tmp_path / "bad.pfw"

    header["args"]["format_version"] = 2
    bad.write_text(json.dumps(header) + "\n" + "\n".join(lines[1:]) + "\n")
    with pytest.raises(dftu.DFTUtilsParseError, match="format_version"):
        read_table(str(bad))

    header["args"]["format_version"] = 1
    header["args"]["nrows"] = 5
    bad.write_text(json.dumps(header) + "\n" + "\n".join(lines[1:]) + "\n")
    with pytest.raises(dftu.DFTUtilsParseError, match="rows"):
        read_table(str(bad))

    header["args"]["nrows"] = 2
    bad.write_text(
        json.dumps(header) + "\n" + lines[1] + "\n" + json.dumps({"ph": 3, "args": {"b": 1}}) + "\n"
    )
    with pytest.raises(dftu.DFTUtilsParseError, match="field"):
        read_table(str(bad))

    bad.write_text(
        json.dumps(header)
        + "\n"
        + lines[1]
        + "\n"
        + json.dumps({"ph": 3, "args": {"a": "x"}})
        + "\n"
    )
    with pytest.raises(dftu.DFTUtilsParseError):
        read_table(str(bad))


def test_table_writer_appends_frames_into_one_file(tmp_path):
    path = str(tmp_path / "w.pfw.gz")
    parts = [
        pd.DataFrame({"a": np.arange(3, dtype="int64") + 3 * i, "s": ["x", "y", None]})
        for i in range(3)
    ]
    with TableWriter(path, name="t", meta={"parts": 3}) as w:
        for p in parts:
            w.write(p)
    back = read_table(path)
    pd.testing.assert_frame_equal(back, pd.concat(parts, ignore_index=True), **EQUAL)
    header = json.loads(lines_of(path)[0])
    assert header["args"]["nrows"] == 9 and header["ph"] == 4


def test_table_writer_with_an_index_and_a_mismatch(tmp_path):
    path = str(tmp_path / "w.pfw.gz")
    a = pd.DataFrame({"v": [1.5]}, index=pd.Index(["p"], name="k"))
    b = pd.DataFrame({"v": [2.5]}, index=pd.Index(["q"], name="k"))
    with TableWriter(path, name="t") as w:
        w.write(a)
        w.write(b)
        with pytest.raises(ValueError):
            w.write(pd.DataFrame({"v": [1], "extra": [2]}))
    pd.testing.assert_frame_equal(read_table(path), pd.concat([a, b]), **EQUAL)


def test_table_writer_discards_on_exception_and_when_empty(tmp_path):
    path = str(tmp_path / "w.pfw.gz")
    with pytest.raises(RuntimeError):
        with TableWriter(path, name="t") as w:
            w.write(pd.DataFrame({"a": [1]}))
            raise RuntimeError("stop")
    assert not os.path.exists(path)
    with pytest.raises(ValueError):
        with TableWriter(path, name="t"):
            pass
    w = TableWriter(path, name="t")
    w.discard()
    with pytest.raises(ValueError):
        w.write(pd.DataFrame({"a": [1]}))


def test_dask_folder_round_trip(tmp_path):
    dd = pytest.importorskip("dask.dataframe")
    df = pd.DataFrame({"a": np.arange(10, dtype="int64"), "s": [f"v{i}" for i in range(10)]})
    folder = str(tmp_path / "parts")
    ddf = dd.from_pandas(df, npartitions=3)
    write_table(ddf, folder, name="t")
    files = sorted(os.listdir(folder))
    assert files == ["part-00000.pfw.gz", "part-00001.pfw.gz", "part-00002.pfw.gz"]
    for f in files:
        assert json.loads(lines_of(os.path.join(folder, f))[0])["ph"] == 4
    back = read_table(folder)
    pd.testing.assert_frame_equal(back, ddf.compute(), **EQUAL)
    with pytest.raises(FileExistsError):
        write_table(dd.from_pandas(df, npartitions=2), folder, name="t")


def test_folder_parts_must_agree(tmp_path):
    write_table(pd.DataFrame({"a": [1]}), str(tmp_path / "part-0.pfw.gz"), name="t")
    write_table(pd.DataFrame({"b": [1]}), str(tmp_path / "part-1.pfw.gz"), name="t")
    with pytest.raises(ValueError, match="differ"):
        read_table(str(tmp_path))
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(ValueError, match="holds no"):
        read_table(str(empty))


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
    back = read_table(str(tmp_path / "t.pfw.gz"))
    assert back["c"].cat.codes.tolist() == [1, -1, 2, 0]


def test_a_value_that_is_no_category_is_an_error(tmp_path):
    df = pd.DataFrame({"c": pd.Categorical(["a", "b", None], categories=["a", "b"])})
    path = str(tmp_path / "c.pfw")
    write_table(df, path, name="t")
    pd.testing.assert_frame_equal(read_table(path), df, **EQUAL)
    rewrite(path, '"c":"b"', '"c":"zzz"')
    with pytest.raises(ValueError, match="'c'.*zzz"):
        read_table(path)
    nums = pd.DataFrame({"c": pd.Categorical([1, 2], categories=[1, 2])})
    write_table(nums, path, name="t")
    rewrite(path, '"c":2', '"c":3')
    with pytest.raises(ValueError, match="'c'"):
        read_table(path)


def test_category_nan_rules(tmp_path):
    df = pd.DataFrame({"c": pd.Categorical(["a", None, "NaN"], categories=["a", "NaN"])})
    path = str(tmp_path / "c.pfw")
    write_table(df, path, name="t")
    back = read_table(path)
    assert (
        back["c"].tolist()[0] == "a"
        and back["c"].tolist()[2] == "NaN"
        and pd.isna(back["c"].tolist()[1])
    )
    # A file without categories_dtype takes a "NaN" value that is no label as missing.
    plain = pd.DataFrame({"c": pd.Categorical(["a", None], categories=["a"])})
    write_table(plain, path, name="t")
    text = re.sub(r', "categories_dtype": "[^"]*"', "", open(path, encoding="utf-8").read())
    assert "categories_dtype" not in text
    open(path, "w", encoding="utf-8").write(text)
    rewrite(path, '"c":null', '"c":"NaN"')
    assert pd.isna(read_table(path)["c"].tolist()[1])
    # With categories_dtype the same value is an error.
    write_table(plain, path, name="t")
    rewrite(path, '"c":null', '"c":"NaN"')
    with pytest.raises(ValueError, match="'c'"):
        read_table(path)
    # NaN and Infinity decode to floats only when the categories are floats.
    floats = pd.DataFrame({"c": pd.Categorical([1.5, None], categories=[1.5, 2.5])})
    write_table(floats, path, name="t")
    assert read_table(path)["c"].cat.categories.dtype == np.float64


def test_unsupported_category_labels_are_refused(tmp_path):
    path = str(tmp_path / "x.pfw")
    inf = pd.DataFrame({"c": pd.Categorical([1.5], categories=[1.5, math.inf])})
    with pytest.raises(ValueError, match="infinite"):
        write_table(inf, path, name="t")
    big = pd.DataFrame({"c": pd.Categorical([1], categories=pd.Index([1, 2**63], dtype="uint64"))})
    with pytest.raises(ValueError, match="2\\*\\*63"):
        write_table(big, path, name="t")
    flags = pd.DataFrame({"c": pd.Categorical([True], categories=[True, False])})
    with pytest.raises(TypeError, match="boolean"):
        write_table(flags, path, name="t")


def test_nan_in_an_object_column_reads_back_as_none(tmp_path):
    df = pd.DataFrame({"o": pd.Series(["a", math.nan, None], dtype=object)})
    path = str(tmp_path / "o.pfw")
    write_table(df, path, name="t")
    assert read_table(path)["o"].tolist() == ["a", None, None]


def test_non_string_index_names_and_collisions(tmp_path):
    df = pd.DataFrame({"v": [1, 2]}, index=pd.Index([10, 20], name=0))
    path = str(tmp_path / "i.pfw.gz")
    write_table(df, path, name="t")
    assert json.loads(lines_of(path)[0])["args"]["index"] == ["0"]
    back = read_table(path)
    assert list(back.index.names) == [0] and back["v"].tolist() == [1, 2]

    clash = pd.DataFrame({"__index_level_0__": [1]}, index=pd.Index([5]))
    with pytest.raises(ValueError, match="'t'.*collide"):
        write_table(clash, path, name="t")
    named = pd.DataFrame({"k": [1]}, index=pd.Index([5], name="k"))
    with pytest.raises(ValueError, match="collide"):
        write_table(named, path, name="t")
    with pytest.raises(TypeError, match="index level name"):
        write_table(pd.DataFrame({"v": [1]}, index=pd.Index([5], name=("a", "b"))), path, name="t")


def test_a_set_cell_that_is_no_array_is_an_error(tmp_path):
    df = pd.DataFrame({"s": pd.Series([{1}, {2}], dtype=object)})
    path = str(tmp_path / "s.pfw")
    write_table(df, path, name="t")
    rewrite(path, '"s":[2]', '"s":5')
    with pytest.raises(ValueError, match="'s'.*array"):
        read_table(path)


def test_folder_parts_may_differ_in_content_but_not_in_names_and_dtypes(tmp_path):
    folder = tmp_path / "parts"
    folder.mkdir()
    write_table(
        pd.DataFrame({"o": pd.Series([None, None], dtype=object)}),
        str(folder / "part-0.pfw.gz"),
        name="t",
    )
    write_table(
        pd.DataFrame({"o": pd.Series([{1}, {2, 3}], dtype=object)}),
        str(folder / "part-1.pfw.gz"),
        name="t",
    )
    back = read_table(str(folder))
    assert back["o"].tolist() == [None, None, {1}, {2, 3}]


def test_dask_failure_leaves_nothing(tmp_path):
    dd = pytest.importorskip("dask.dataframe")
    base = dd.from_pandas(pd.DataFrame({"a": np.arange(8)}), npartitions=2)

    def second_part_has_a_timedelta(part):
        # Only the second partition gets a column the format cannot carry.
        return part.assign(
            t=pd.to_timedelta(part["a"], unit="s") if part["a"].iloc[0] >= 4 else part["a"]
        )

    ddf = base.map_partitions(second_part_has_a_timedelta, meta=base._meta.assign(t=np.int64(0)))
    folder = tmp_path / "parts"
    with pytest.raises(TypeError):
        write_table(ddf, str(folder), name="t")
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
    pytest.importorskip("dask.dataframe")
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


def test_dfanalyzer_reads_our_files_and_we_read_theirs(tmp_path):
    ref = load_reference()
    df = matrix()
    df.index = pd.MultiIndex.from_arrays([np.arange(len(df)), list("abcde")], names=[None, "lvl"])
    ours = str(tmp_path / "ours.pfw.gz")
    write_table(df, ours, name="t")
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
    # Their writer names a StringDtype "string" with no storage.
    df = df.drop(columns=["str_python", "str_pyarrow"])
    ref.write_table(df, theirs, "t")
    back = read_table(theirs)
    pd.testing.assert_frame_equal(
        back.drop(columns=["sets", "lists"]), df.drop(columns=["sets", "lists"]), **EQUAL
    )
    sets_equal_nan(back["sets"].tolist(), df["sets"].tolist())
    sets_equal_nan(back["lists"].tolist(), df["lists"].tolist())
