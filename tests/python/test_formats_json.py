# SPDX-License-Identifier: MIT
"""The JSON reader and writer of dftracer.utils.formats."""

import gc
import gzip
import json
import os
import subprocess
import sys
import textwrap
import threading
import time
from collections.abc import Mapping

import pytest

from dftracer.utils.errors import DFTUtilsIOError, DFTUtilsReaderError
from dftracer.utils.formats import json as fjson
from dftracer.utils.formats import yaml as fyaml

DOC = {
    "name": "demo",
    "n": 7,
    "big": 2**64 - 1,
    "neg": -3,
    "f": 1.5,
    "ok": True,
    "no": False,
    "nothing": None,
    "list": [1, "two", [3], {"four": 4}],
    "obj": {"a": {"b": [1, 2]}},
    "text": 'café ☃ "quoted" \\ \n',
}


def compact(data):
    return json.dumps(data, separators=(",", ":"), ensure_ascii=False)


def write_lines(path, records, gz=False):
    text = "".join(compact(r) + "\n" for r in records)
    if gz:
        with gzip.open(path, "wt", encoding="utf-8") as f:
            f.write(text)
    else:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)


def read_back(path):
    data = open(path, "rb").read()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    return data.decode("utf-8")


@pytest.mark.valgrind
def test_parse_reads_every_kind():
    value = fjson.parse(compact(DOC))
    assert value.kind is fjson.Kind.OBJECT
    assert value["name"].kind is fjson.Kind.STRING
    assert value["n"].kind is fjson.Kind.INT
    assert value["big"].kind is fjson.Kind.INT
    assert value["f"].kind is fjson.Kind.FLOAT
    assert value["ok"].kind is fjson.Kind.BOOL
    assert value["nothing"].kind is fjson.Kind.NULL
    assert value["list"].kind is fjson.Kind.ARRAY
    assert value["name"].as_str() == "demo"
    assert value["n"].as_int() == 7
    assert value["big"].as_int() == 2**64 - 1
    assert value["neg"].as_int() == -3
    assert value["f"].as_float() == 1.5
    assert value["n"].as_float() == 7.0
    assert value["ok"].as_bool() is True
    assert value["list"][1].as_str() == "two"
    assert value["obj"]["a"]["b"][1].as_int() == 2
    assert value.to_python() == DOC


def test_loads_matches_the_standard_library():
    assert fjson.loads(compact(DOC)) == json.loads(compact(DOC))
    assert fjson.loads("[]") == []
    assert fjson.loads('{"a":[]}') == {"a": []}


def test_lookup_errors_and_defaults():
    value = fjson.parse('{"a":1,"list":[10,20,30]}')
    with pytest.raises(KeyError):
        value["missing"]
    assert value.get("missing") is None
    assert value.get("a").as_int() == 1
    assert value["list"][-1].as_int() == 30
    with pytest.raises(IndexError):
        value["list"][3]
    with pytest.raises(IndexError):
        value["list"][-4]
    with pytest.raises(TypeError):
        value["list"]["a"]
    with pytest.raises(TypeError):
        value[0]
    with pytest.raises(TypeError):
        value["a"]["b"]


def test_a_value_of_the_wrong_kind_raises_type_error():
    value = fjson.parse('{"s":"x","n":1,"b":true,"z":null,"f":1.5,"a":[1]}')
    with pytest.raises(TypeError):
        value["s"].as_int()
    with pytest.raises(TypeError):
        value["n"].as_str()
    with pytest.raises(TypeError):
        value["n"].as_bool()
    with pytest.raises(TypeError):
        value["b"].as_int()
    with pytest.raises(TypeError):
        value["z"].as_float()
    with pytest.raises(TypeError):
        value["f"].as_int()
    with pytest.raises(TypeError):
        value["a"].as_str()
    with pytest.raises(TypeError):
        len(value["s"])


def test_items_iteration_and_length():
    value = fjson.parse('{"a":1,"b":[1,2,3],"c":{}}')
    assert [(k, v.to_python()) for k, v in value.items()] == [
        ("a", 1),
        ("b", [1, 2, 3]),
        ("c", {}),
    ]
    assert len(value) == 3
    assert len(value["b"]) == 3
    assert [v.as_int() for v in value["b"]] == [1, 2, 3]
    assert not value["c"]
    assert value["b"]
    assert "a" in value and "z" not in value
    with pytest.raises(TypeError):
        iter(value)
    with pytest.raises(TypeError):
        list(value["a"])


@pytest.mark.parametrize("gz", [False, True])
def test_read_and_load_a_file(tmp_path, gz):
    path = tmp_path / ("doc.json.gz" if gz else "doc.json")
    text = json.dumps(DOC, indent=2)
    if gz:
        path.write_bytes(gzip.compress(text.encode()))
    else:
        path.write_text(text, encoding="utf-8")
    assert fjson.read(path).to_python() == DOC
    assert fjson.read(str(path)).to_python() == DOC
    assert fjson.load(path) == DOC


def test_read_joins_the_members_of_a_gzip_file(tmp_path):
    path = tmp_path / "two.json.gz"
    path.write_bytes(gzip.compress(b'{"a":') + gzip.compress(b"[1,2]}"))
    assert fjson.load(path) == {"a": [1, 2]}


def test_a_missing_file_raises_file_not_found(tmp_path):
    with pytest.raises(FileNotFoundError):
        fjson.read(tmp_path / "missing.json")
    with pytest.raises(FileNotFoundError):
        list(fjson.read_records(tmp_path / "missing.jsonl"))


@pytest.mark.parametrize(
    "text",
    ['{"a":1,}', '{"a":1} x', '{"a":1', "", "7", '{"a":01}', '{"a":"\\x"}'],
)
def test_a_syntax_error_raises_a_reader_error(text):
    with pytest.raises(DFTUtilsReaderError):
        fjson.parse(text)
    with pytest.raises(DFTUtilsReaderError):
        fjson.loads(text)


def test_a_syntax_error_names_the_line(tmp_path):
    path = tmp_path / "bad.json"
    path.write_text('{\n  "a": 1,\n  "b": ,\n}\n')
    with pytest.raises(DFTUtilsReaderError, match=r":3:"):
        fjson.read(path)
    records = tmp_path / "bad.jsonl"
    records.write_text('{"a":1}\n\n{"a":2}\n{"a":\n{"a":4}\n')
    with pytest.raises(DFTUtilsReaderError, match=r"bad.jsonl:4:"):
        list(fjson.read_records(records))


def test_a_whole_file_over_the_memory_budget_is_refused(tmp_path):
    path = tmp_path / "big.json"
    path.write_text(compact({"x": "a" * 400_000}))
    script = textwrap.dedent(
        """
        import sys
        from dftracer.utils.formats import json
        try:
            json.read(sys.argv[1])
        except Exception as e:
            print(type(e).__name__, e)
        else:
            print("read")
        """
    )
    env = dict(os.environ, DFTRACER_UTILS_MEM_BUDGET="1MiB")
    out = subprocess.run(
        [sys.executable, "-I", "-c", script, str(path)],
        capture_output=True,
        text=True,
        env=env,
        check=True,
    ).stdout
    assert out.startswith("MemoryError"), out
    assert "read_records" in out


@pytest.mark.parametrize("gz", [False, True])
def test_read_records_streams_batches_in_order(tmp_path, gz):
    records = [{"i": i, "pad": "x" * 40} for i in range(500)]
    path = tmp_path / ("r.jsonl.gz" if gz else "r.jsonl")
    write_lines(path, records, gz)
    batches = list(fjson.read_records(path, batch_bytes=2000))
    assert len(batches) > 5
    assert sum(len(b) for b in batches) == 500
    got = [v.to_python() for b in batches for v in b]
    assert got == records
    one = list(fjson.read_records(path))
    assert len(one) == 1
    assert one[0][499]["i"].as_int() == 499
    assert one[0][-1]["i"].as_int() == 499


def test_read_records_skips_blank_lines_and_a_missing_final_newline(tmp_path):
    path = tmp_path / "r.jsonl"
    path.write_text('{"i":1}\n\n   \n{"i":2}\r\n{"i":3}')
    got = [v["i"].as_int() for b in fjson.read_records(path) for v in b]
    assert got == [1, 2, 3]


def test_a_kept_record_keeps_its_batch_alive(tmp_path):
    path = tmp_path / "r.jsonl"
    write_lines(path, [{"i": i, "pad": "y" * 60} for i in range(300)])
    kept = None
    seen = 0
    for batch in fjson.read_records(path, batch_bytes=1000):
        if kept is None:
            kept = batch[0]
        seen += len(batch)
    assert seen == 300
    assert kept["i"].as_int() == 0
    assert kept["pad"].as_str() == "y" * 60
    assert kept.to_python() == {"i": 0, "pad": "y" * 60}


def test_a_value_holds_its_batch_and_a_dropped_one_does_not(tmp_path):
    path = tmp_path / "r.jsonl"
    write_lines(path, [{"i": 1}, {"i": 2}])
    (batch,) = list(fjson.read_records(path))
    base = sys.getrefcount(batch)
    value = batch[0]
    assert sys.getrefcount(batch) == base + 1
    child = value["i"]
    assert sys.getrefcount(batch) == base + 2
    del value, child
    assert sys.getrefcount(batch) == base


_PEAK_SCRIPT = textwrap.dedent(
    """
    import resource, sys
    from dftracer.utils.formats import json
    path, batch_bytes = sys.argv[1], int(sys.argv[2])
    def peak():
        if sys.platform == "darwin":
            return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        with open("/proc/self/status") as status:
            return next(int(l.split()[1]) for l in status if l.startswith("VmHWM:")) * 1024
    for _ in json.read_records(path, batch_bytes=1 << 20):
        break
    before = peak()
    count = 0
    for batch in json.read_records(path, batch_bytes=batch_bytes):
        count += len(batch)
    print(count, before, peak())
    """
)


@pytest.mark.skipif(sys.platform == "win32", reason="peak memory is read with resource")
def test_read_records_holds_about_one_batch(tmp_path):
    path = tmp_path / "big.jsonl.gz"
    line = compact({"name": "event", "cat": "posix", "args": {"fd": 3, "pad": "z" * 80}})
    count = 1_600_000
    with gzip.open(path, "wt") as f:
        chunk = (line + "\n") * 100_000
        for _ in range(count // 100_000):
            f.write(chunk)
    size = count * (len(line) + 1)
    batch_bytes = 4 << 20
    got = subprocess.run(
        [sys.executable, "-I", "-c", _PEAK_SCRIPT, str(path), str(batch_bytes)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    seen, before, after = int(got[0]), int(got[1]), int(got[2])
    assert seen == count
    assert size > 30 * batch_bytes
    # The peak is the process RSS high water: Python has no tracked-bytes API.
    # One live batch is about batch_bytes; the loop holds the previous batch
    # while it reads the next.
    assert after - before < 3 * batch_bytes
    assert after - before < size // 3


@pytest.mark.parametrize("name", ["out.jsonl", "out.jsonl.gz"])
def test_a_written_file_reads_back(tmp_path, name):
    path = tmp_path / name
    records = [DOC, {"i": 1}, [1, 2, [3]], {"empty": {}, "arr": []}]
    with fjson.Writer(path) as w:
        for r in records:
            w.write(r)
    raw = path.read_bytes()
    assert (raw[:2] == b"\x1f\x8b") == name.endswith(".gz")
    assert [v.to_python() for b in fjson.read_records(path) for v in b] == records
    assert read_back(path) == "".join(compact(r) + "\n" for r in records)


def test_compress_overrides_the_suffix(tmp_path):
    plain = tmp_path / "a.jsonl.gz"
    with fjson.Writer(plain, compress=False) as w:
        w.write({"a": 1})
    assert plain.read_bytes() == b'{"a":1}\n'
    packed = tmp_path / "b.jsonl"
    with fjson.Writer(packed, compress=True, level=1) as w:
        w.write({"a": 1})
    assert gzip.decompress(packed.read_bytes()) == b'{"a":1}\n'


def test_a_written_file_of_many_members_reads_back(tmp_path):
    path = tmp_path / "m.jsonl.gz"
    records = [{"i": i, "pad": "p" * 50} for i in range(5000)]
    with fjson.Writer(path, member_size=4096) as w:
        for r in records:
            w.write(r)
    assert [v.to_python() for b in fjson.read_records(path) for v in b] == records


def test_record_builders_write_one_record(tmp_path):
    path = tmp_path / "b.jsonl"
    with fjson.Writer(path) as w:
        with w.record() as r:
            r.member("name", "demo")
            r.member("n", 3)
            with r.object("args") as a:
                a.member("fd", 7)
                with a.array("sizes") as s:
                    s.value(1)
                    s.value("x")
                    with s.object() as o:
                        o.member("deep", None)
                    with s.array() as inner:
                        inner.value(True)
            with r.array("tail") as t:
                t.value(2.5)
        w.write({"after": 1})
    assert read_back(path) == (
        compact(
            {
                "name": "demo",
                "n": 3,
                "args": {"fd": 7, "sizes": [1, "x", {"deep": None}, [True]]},
                "tail": [2.5],
            }
        )
        + "\n"
        + compact({"after": 1})
        + "\n"
    )


def test_builders_raise_when_called_in_the_wrong_order(tmp_path):
    path = tmp_path / "o.jsonl"
    w = fjson.Writer(path)
    with w.record() as r:
        inner = r.object("a")
        with pytest.raises(RuntimeError):
            r.member("b", 1)
        with pytest.raises(RuntimeError):
            r.object("c")
        with pytest.raises(RuntimeError):
            w.write({"x": 1})
        with pytest.raises(RuntimeError):
            w.record()
        with inner:
            inner.member("k", 1)
        r.member("b", 2)
        with pytest.raises(RuntimeError):
            inner.member("late", 1)
    with pytest.raises(RuntimeError):
        r.member("after", 1)
    w.close()
    w.close()
    with pytest.raises(RuntimeError):
        w.write({"x": 1})
    with pytest.raises(RuntimeError):
        w.record()
    assert read_back(path) == compact({"a": {"k": 1}, "b": 2}) + "\n"


def test_closing_an_outer_builder_with_an_inner_one_open_raises(tmp_path):
    path = tmp_path / "o.jsonl"
    with fjson.Writer(path) as w:
        w.write({"first": 1})
        with pytest.raises(RuntimeError):
            with w.record() as r:
                r.object("a")
        w.write({"last": 2})
    assert read_back(path) == compact({"first": 1}) + "\n" + compact({"last": 2}) + "\n"


def test_a_failed_block_drops_its_record(tmp_path):
    path = tmp_path / "o.jsonl"
    with fjson.Writer(path) as w:
        with pytest.raises(ZeroDivisionError):
            with w.record() as r:
                r.member("a", 1)
                1 / 0
        w.write({"ok": 1})
    assert read_back(path) == compact({"ok": 1}) + "\n"


@pytest.mark.parametrize(
    "bad",
    [{1, 2}, b"bytes", bytearray(b"x"), object(), {"k": {1}}, [1, {2, 3}], 1j],
)
def test_a_value_outside_the_input_types_raises_type_error(tmp_path, bad):
    path = tmp_path / "t.jsonl"
    with fjson.Writer(path) as w:
        with pytest.raises(TypeError):
            w.write(bad)
        with pytest.raises(TypeError):
            w.write({"a": [1, {"b": bad}]})
        w.write({"ok": 1})
        with w.record() as r:
            with pytest.raises(TypeError):
                r.member("k", bad)
        w.write({"ok": 2})
    assert read_back(path) == compact({"ok": 1}) + "\n" + compact({"ok": 2}) + "\n"


def test_a_key_that_is_not_text_raises_type_error(tmp_path):
    with fjson.Writer(tmp_path / "k.jsonl") as w:
        with pytest.raises(TypeError):
            w.write({1: "x"})
        with w.record() as r:
            with pytest.raises(TypeError):
                r.member(1, "x")
            with pytest.raises(TypeError):
                r.object(None)


def test_an_int_that_does_not_fit_64_bits_raises_overflow_error(tmp_path):
    with fjson.Writer(tmp_path / "k.jsonl") as w:
        w.write({"max": 2**64 - 1, "min": -(2**63)})
        with pytest.raises(OverflowError):
            w.write({"x": 2**64})
        with pytest.raises(OverflowError):
            w.write({"x": -(2**63) - 1})


def test_nan_and_infinity_are_written_as_trace_writer_writes_them(tmp_path):
    path = tmp_path / "n.jsonl"
    with fjson.Writer(path) as w:
        w.write({"a": float("nan"), "b": float("inf"), "c": float("-inf"), "d": 0.5})
    assert json.loads(read_back(path)) == {
        "a": "NaN",
        "b": "Infinity",
        "c": "-Infinity",
        "d": 0.5,
    }


def test_a_value_written_unchanged_makes_the_same_bytes(tmp_path):
    lines = [compact(DOC), compact({"i": [1, {"j": 2.5}], "s": "x\ny"}), compact([1, [2]])]
    src = tmp_path / "in.jsonl"
    src.write_text("".join(line + "\n" for line in lines), encoding="utf-8")
    out = tmp_path / "out.jsonl"
    with fjson.Writer(out) as w:
        for batch in fjson.read_records(src):
            for v in batch:
                w.write(v)
    assert out.read_bytes() == src.read_bytes()
    assert fjson.parse(lines[0]).raw == lines[0]
    assert fjson.parse(lines[0])["obj"].raw == compact(DOC["obj"])
    assert fjson.parse(lines[0])["name"].raw == '"demo"'
    nested = tmp_path / "nested.jsonl"
    with fjson.Writer(nested) as w:
        w.write({"wrapped": fjson.parse(lines[0])["list"], "more": [fjson.parse(lines[1])]})
    assert json.loads(read_back(nested)) == {"wrapped": DOC["list"], "more": [json.loads(lines[1])]}


def test_a_mutable_value_writes_its_changes_and_leaves_the_source(tmp_path):
    source = {"a": 1, "b": {"c": [1, 2, 3], "d": "x"}, "e": [{"f": 1}, {"f": 2}], "g": 0}
    value = fjson.parse(compact(source))
    m = value.mutable()
    m["a"] = 10
    m["new"] = {"k": [1, 2]}
    del m["g"]
    m["b"]["c"].append(4)
    m["b"]["c"][0] = "first"
    del m["b"]["d"]
    m["b"]["extra"] = None
    m["e"][1]["f"] = 20
    del m["e"][0]
    m["e"].append({"f": 3})
    expected = {
        "a": 10,
        "b": {"c": ["first", 2, 3, 4], "extra": None},
        "e": [{"f": 20}, {"f": 3}],
        "new": {"k": [1, 2]},
    }
    assert m.to_python() == expected
    assert m["b"]["c"][3].as_int() == 4
    assert len(m) == 4 and len(m["e"]) == 2
    assert value.to_python() == source
    assert value.raw == compact(source)
    out = tmp_path / "m.jsonl"
    with fjson.Writer(out) as w:
        w.write(m)
        w.write(value)
    lines = read_back(out).splitlines()
    assert json.loads(lines[0]) == expected
    assert lines[1] == compact(source)


def test_a_mutable_value_that_is_unchanged_writes_as_the_value_does(tmp_path):
    value = fjson.parse(compact(DOC))
    out = tmp_path / "m.jsonl"
    with fjson.Writer(out) as w:
        w.write(value.mutable())
        sub = value.mutable()
        sub["obj"]["a"]["b"]
        w.write(sub)
    assert read_back(out) == compact(DOC) + "\n" + compact(DOC) + "\n"


def test_mutable_value_errors():
    m = fjson.parse('{"a":1,"l":[1,2]}').mutable()
    with pytest.raises(KeyError):
        m["missing"]
    with pytest.raises(KeyError):
        del m["missing"]
    with pytest.raises(IndexError):
        m["l"][5]
    with pytest.raises(IndexError):
        del m["l"][5]
    with pytest.raises(IndexError):
        m["l"][2] = 1
    with pytest.raises(TypeError):
        m[0]
    with pytest.raises(TypeError):
        m["l"]["x"] = 1
    with pytest.raises(TypeError):
        m["a"].append(1)
    with pytest.raises(TypeError):
        m[1] = 2
    assert m.get("missing") is None
    assert m.get("a").as_int() == 1
    assert m["a"].as_int() == 1
    m["a"] = "text"
    assert m["a"].as_str() == "text"
    with pytest.raises(TypeError):
        m["a"].as_int()
    assert [k for k, _ in m.items()] == ["a", "l"]
    assert [v.as_int() for v in m["l"]] == [1, 2]


def test_a_value_of_the_other_format_is_walked_and_written_again(tmp_path):
    y = fyaml.parse("name: demo\ncount: 3\nitems:\n  - a\n  - b\nnothing: ~\n")
    out = tmp_path / "x.jsonl"
    with fjson.Writer(out) as w:
        w.write(y)
        w.write({"wrapped": y["items"], "m": y.mutable()})
    first, second = (json.loads(line) for line in read_back(out).splitlines())
    assert first == {"name": "demo", "count": "3", "items": ["a", "b"], "nothing": None}
    assert second == {"wrapped": ["a", "b"], "m": first}


def test_mappings_and_sequences_are_written(tmp_path):
    from collections import OrderedDict
    from types import MappingProxyType

    out = tmp_path / "s.jsonl"
    with fjson.Writer(out) as w:
        w.write(MappingProxyType({"a": (1, 2), "b": OrderedDict(c=[None])}))
        w.write(range(3))
    lines = read_back(out).splitlines()
    assert json.loads(lines[0]) == {"a": [1, 2], "b": {"c": [None]}}
    assert json.loads(lines[1]) == [0, 1, 2]


def test_a_value_nested_too_deep_raises_value_error(tmp_path):
    deep = []
    node = deep
    for _ in range(1200):
        node.append([])
        node = node[0]
    with fjson.Writer(tmp_path / "d.jsonl") as w:
        with pytest.raises(ValueError):
            w.write(deep)
        w.write({"ok": 1})


def test_a_writer_removes_its_output_when_the_block_raises(tmp_path):
    path = tmp_path / "gone.jsonl.gz"
    with pytest.raises(ZeroDivisionError):
        with fjson.Writer(path) as w:
            w.write({"a": 1})
            1 / 0
    assert not path.exists()


def test_writer_rejects_bad_options(tmp_path):
    with pytest.raises(ValueError):
        fjson.Writer(tmp_path / "a.jsonl", member_size=0)
    with pytest.raises(TypeError):
        fjson.Writer(123)
    with pytest.raises(DFTUtilsIOError):
        fjson.Writer(tmp_path / "no" / "such" / "dir.jsonl")
    with pytest.raises(ValueError):
        list(fjson.read_records(tmp_path / "a.jsonl", batch_bytes=0))


def test_a_record_larger_than_a_batch_makes_its_own_batch(tmp_path):
    path = tmp_path / "r.jsonl"
    write_lines(path, [{"i": i, "pad": "p" * 200} for i in range(20)])
    batches = list(fjson.read_records(path, batch_bytes=64))
    assert [len(b) for b in batches] == [1] * 20
    assert [b[0]["i"].as_int() for b in batches] == list(range(20))


NON_CANONICAL = [
    '{ "b" : 1 , "a" : [ 1.0e3 , 2 ] }',
    '{"u":"\\u00e9\\u2603 \\/ x","n":-0.0,"e":1E+2}',
    '[ 1 , {"k" :\t"v" } ]',
    '  {"big":18446744073709551615,"f":1.50,"z":0e0}  ',
    '{"nested":{"b":{ "c" : [ ] },"a":{}},"s":"a\\nb"}',
]


def test_a_record_written_unchanged_makes_the_same_bytes_for_any_input(tmp_path):
    src = tmp_path / "in.jsonl"
    src.write_text("".join(line + "\n" for line in NON_CANONICAL), encoding="utf-8")
    out = tmp_path / "out.jsonl"
    with fjson.Writer(out) as w:
        for batch in fjson.read_records(src, batch_bytes=1):
            for v in batch:
                w.write(v)
    assert out.read_bytes() == src.read_bytes()
    kept = [v for b in fjson.read_records(src) for v in b]
    assert [v.raw for v in kept] == NON_CANONICAL
    nested = tmp_path / "nested.jsonl"
    with fjson.Writer(nested) as w:
        w.write({"first": kept[0], "all": kept[1:3]})
    assert read_back(nested) == (
        '{"first":'
        + NON_CANONICAL[0]
        + ',"all":['
        + NON_CANONICAL[1]
        + ","
        + NON_CANONICAL[2]
        + "]}\n"
    )


def test_parse_and_read_keep_the_source_text_as_raw(tmp_path):
    text = ' \n{ "b" : 1 ,\n  "a" : [ 1.0e3 ] }\n\n'
    value = fjson.parse(text)
    assert value.raw == text.strip()
    path = tmp_path / "doc.json"
    path.write_text(text)
    assert fjson.read(path).raw == text.strip()
    one = tmp_path / "one.jsonl"
    with fjson.Writer(one) as w:
        w.write(fjson.parse('{ "a" :  1.0e3 }'))
        w.write(value)
    first, second = read_back(one).splitlines()
    assert first == '{ "a" :  1.0e3 }'
    assert json.loads(second) == {"b": 1, "a": [1000.0]}
    assert "\n" not in second


def test_an_unchanged_mutable_value_writes_the_source_bytes(tmp_path):
    line = NON_CANONICAL[0]
    m = fjson.parse(line).mutable()
    m["a"][0].as_float()
    out = tmp_path / "m.jsonl"
    with fjson.Writer(out) as w:
        w.write(m)
        m["a"][1] = 5
        w.write(m)
    first, second = read_back(out).splitlines()
    assert first == line
    assert json.loads(second) == {"b": 1, "a": [1000.0, 5]}


def test_every_element_of_a_large_array_is_indexed(tmp_path):
    count = 150_000
    value = fjson.parse("[" + ",".join(str(i) for i in range(count)) + "]")
    total = 0
    for i in range(len(value)):
        total += value[i].as_int()
    assert total == count * (count - 1) // 2
    assert value[-1].as_int() == count - 1
    assert value[-count].as_int() == 0
    with pytest.raises(IndexError):
        value[count]


class _BadItems(Mapping):
    """A Mapping whose items() does not give pairs."""

    def __init__(self, items):
        self._items = items

    def __getitem__(self, key):
        return 1

    def __iter__(self):
        return iter(["a"])

    def __len__(self):
        return 1

    def items(self):
        return self._items


@pytest.mark.parametrize("items", [[["a", 1]], [("a",)], [("a", 1, 2)], ["ab"], [1], [()]])
def test_a_mapping_whose_items_are_not_pairs_raises_type_error(tmp_path, items):
    bad = _BadItems(items)
    with fjson.Writer(tmp_path / "o.jsonl") as w:
        with pytest.raises(TypeError):
            w.write(bad)
        with pytest.raises(TypeError):
            w.write({"k": [bad]})
        w.write({"ok": 1})
    m = fjson.parse("{}").mutable()
    m["x"] = bad
    with pytest.raises(TypeError):
        m.to_python()
    with fjson.Writer(tmp_path / "p.jsonl") as w:
        with pytest.raises(TypeError):
            w.write(m)


def test_a_writer_dropped_without_close_removes_its_output(tmp_path):
    path = tmp_path / "dropped.jsonl.gz"
    w = fjson.Writer(path)
    w.write({"a": 1})
    del w
    gc.collect()
    assert not path.exists()


def test_two_threads_on_one_record_iterator_do_not_race(tmp_path):
    path = tmp_path / "big.jsonl"
    line = compact({"name": "event", "args": {"fd": 3, "pad": "z" * 80}}) + "\n"
    with open(path, "w") as f:
        chunk = line * 100_000
        for _ in range(30):
            f.write(chunk)
    it = fjson.read_records(path, batch_bytes=1 << 30)
    started = threading.Event()
    outcome = {}

    def reader():
        started.set()
        try:
            outcome["batch"] = next(it)
        except BaseException as e:
            outcome["error"] = e

    thread = threading.Thread(target=reader)
    thread.start()
    started.wait()
    time.sleep(0.05)
    try:
        next(it)
    except RuntimeError as e:
        assert "already running" in str(e)
    except StopIteration:
        pass
    thread.join()
    assert "error" not in outcome
    assert len(outcome["batch"]) > 0
