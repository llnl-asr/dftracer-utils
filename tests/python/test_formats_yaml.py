# SPDX-License-Identifier: MIT
"""The YAML reader and writer of dftracer.utils.formats."""

import gc
import gzip
import os
import subprocess
import sys
import textwrap
from collections.abc import Mapping

import pytest

from dftracer.utils.errors import DFTUtilsReaderError
from dftracer.utils.formats import json as fjson
from dftracer.utils.formats import yaml as fyaml

TEXT = """\
name: demo
count: 7
ratio: 1.5
enabled: true
nothing: ~
quoted: "caf\\u00e9 \\u2603"
single: 'it''s'
items:
  - a
  - 2
  - nested:
      deep: [1, 2, 3]
      map: {x: y}
mapping:
  key: value
  other:
    - p
    - q
"""

DATA = {
    "name": "demo",
    "count": "7",
    "ratio": "1.5",
    "enabled": "true",
    "nothing": None,
    "quoted": "caf\u00e9 \u2603",
    "single": "it's",
    "items": ["a", "2", {"nested": {"deep": ["1", "2", "3"], "map": {"x": "y"}}}],
    "mapping": {"key": "value", "other": ["p", "q"]},
}


def read_back(path):
    data = open(path, "rb").read()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    return data.decode("utf-8")


def records_of(path, **kwargs):
    return [v.to_python() for b in fyaml.read_records(path, **kwargs) for v in b]


@pytest.mark.valgrind
def test_parse_reads_every_kind():
    value = fyaml.parse(TEXT)
    assert value.kind is fyaml.Kind.OBJECT
    assert value["name"].kind is fyaml.Kind.SCALAR
    assert value["nothing"].kind is fyaml.Kind.NULL
    assert value["items"].kind is fyaml.Kind.ARRAY
    assert value["name"].as_str() == "demo"
    assert value["count"].as_int() == 7
    assert value["ratio"].as_float() == 1.5
    assert value["enabled"].as_bool() is True
    assert value["items"][2]["nested"]["deep"][1].as_int() == 2
    assert value["mapping"]["other"][-1].as_str() == "q"
    assert value.to_python() == DATA


def test_loads_returns_plain_data():
    assert fyaml.loads(TEXT) == DATA
    assert fyaml.loads("- a\n- b\n") == ["a", "b"]
    assert fyaml.loads("{a: [1, 2]}") == {"a": ["1", "2"]}


def test_a_scalar_is_text_and_as_readers_raise_value_error():
    value = fyaml.parse("s: abc\nn: 12\nnothing: ~\nbig: 99999999999999999999\n")
    with pytest.raises(ValueError):
        value["s"].as_int()
    with pytest.raises(ValueError):
        value["s"].as_float()
    with pytest.raises(ValueError):
        value["s"].as_bool()
    with pytest.raises(ValueError):
        value["nothing"].as_int()
    with pytest.raises(ValueError):
        value["nothing"].as_str()
    assert value["n"].as_str() == "12"
    assert value["n"].as_int() == 12
    with pytest.raises(ValueError):
        value["big"].as_int()
    with pytest.raises(TypeError):
        fyaml.parse("a: [1]\n")["a"].as_int()
    with pytest.raises(TypeError):
        fyaml.parse("a: {b: 1}\n")["a"].as_str()


def test_lookup_errors_and_defaults():
    value = fyaml.parse("a: 1\nlist: [10, 20, 30]\n")
    with pytest.raises(KeyError):
        value["missing"]
    assert value.get("missing") is None
    assert value.get("a").as_int() == 1
    assert value["list"][-1].as_int() == 30
    with pytest.raises(IndexError):
        value["list"][3]
    with pytest.raises(TypeError):
        value["list"]["a"]
    with pytest.raises(TypeError):
        value[0]
    assert [(k, v.to_python()) for k, v in value.items()] == [
        ("a", "1"),
        ("list", ["10", "20", "30"]),
    ]
    assert len(value) == 2 and len(value["list"]) == 3
    assert [v.as_int() for v in value["list"]] == [10, 20, 30]
    with pytest.raises(TypeError):
        len(value["a"])


@pytest.mark.parametrize("gz", [False, True])
def test_read_and_load_a_file(tmp_path, gz):
    path = tmp_path / ("doc.yaml.gz" if gz else "doc.yaml")
    if gz:
        path.write_bytes(gzip.compress(TEXT.encode()))
    else:
        path.write_text(TEXT, encoding="utf-8")
    assert fyaml.read(path).to_python() == DATA
    assert fyaml.read(str(path)).to_python() == DATA
    assert fyaml.load(path) == DATA


def test_a_missing_file_raises_file_not_found(tmp_path):
    with pytest.raises(FileNotFoundError):
        fyaml.read(tmp_path / "missing.yaml")
    with pytest.raises(FileNotFoundError):
        list(fyaml.read_records(tmp_path / "missing.yaml"))


@pytest.mark.parametrize(
    "text",
    ["a: &x 1\n", "a: *x\n", "a: !!str 1\n", "a: 1\na: 2\n", "a: [1, 2\n", "a:\n\t- b\n"],
)
def test_a_syntax_error_or_an_unsupported_feature_raises_a_reader_error(text):
    with pytest.raises(DFTUtilsReaderError):
        fyaml.parse(text)
    with pytest.raises(DFTUtilsReaderError):
        fyaml.loads(text)


def test_a_syntax_error_names_the_line(tmp_path):
    path = tmp_path / "bad.yaml"
    path.write_text("a: 1\nb: 2\nc: [1, 2\n")
    with pytest.raises(DFTUtilsReaderError, match=r"bad.yaml:\d+:\d+"):
        fyaml.read(path)
    seq = tmp_path / "bad_seq.yaml"
    seq.write_text("- a: 1\n- b: 2\n- c: *x\n- d: 4\n")
    with pytest.raises(DFTUtilsReaderError, match=r"bad_seq.yaml:3:"):
        list(fyaml.read_records(seq))


def test_a_whole_file_over_the_memory_budget_is_refused(tmp_path):
    path = tmp_path / "big.yaml"
    path.write_text("x: " + "a" * 600_000 + "\n")
    script = textwrap.dedent(
        """
        import sys
        from dftracer.utils.formats import yaml
        try:
            yaml.read(sys.argv[1])
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


def sequence_text(count):
    return "".join(
        f"- id: {i}\n  tags:\n    - t{i}\n    - u\n  meta:\n    pad: {'x' * 40}\n"
        for i in range(count)
    )


def sequence_records(count):
    return [{"id": str(i), "tags": [f"t{i}", "u"], "meta": {"pad": "x" * 40}} for i in range(count)]


@pytest.mark.parametrize("gz", [False, True])
def test_read_records_streams_the_elements_of_a_sequence(tmp_path, gz):
    path = tmp_path / ("s.yaml.gz" if gz else "s.yaml")
    text = sequence_text(400)
    path.write_bytes(gzip.compress(text.encode()) if gz else text.encode())
    batches = list(fyaml.read_records(path, batch_bytes=3000))
    assert len(batches) > 5
    assert sum(len(b) for b in batches) == 400
    assert [v.to_python() for b in batches for v in b] == sequence_records(400)
    one = list(fyaml.read_records(path))
    assert len(one) == 1 and len(one[0]) == 400
    assert one[0][399]["id"].as_int() == 399
    assert one[0][-1]["id"].as_int() == 399


def test_read_records_streams_the_documents_of_a_stream(tmp_path):
    path = tmp_path / "d.yaml"
    docs = [f"---\nid: {i}\nitems:\n  - a{i}\n  - b\n" for i in range(300)]
    path.write_text("".join(docs))
    batches = list(fyaml.read_records(path, batch_bytes=1500))
    assert len(batches) > 5
    got = [v.to_python() for b in batches for v in b]
    assert got == [{"id": str(i), "items": [f"a{i}", "b"]} for i in range(300)]


def test_read_records_of_a_plain_document_is_one_record(tmp_path):
    path = tmp_path / "w.yaml"
    path.write_text(TEXT)
    got = records_of(path, batch_bytes=64)
    assert got == [DATA]


def test_a_kept_record_keeps_its_batch_alive(tmp_path):
    path = tmp_path / "s.yaml"
    path.write_text(sequence_text(300))
    kept = None
    seen = 0
    for batch in fyaml.read_records(path, batch_bytes=2000):
        if kept is None:
            kept = batch[0]
        seen += len(batch)
    assert seen == 300
    assert kept["id"].as_int() == 0
    assert kept["meta"]["pad"].as_str() == "x" * 40
    assert kept.to_python() == sequence_records(1)[0]


def test_a_value_holds_its_batch_and_a_dropped_one_does_not(tmp_path):
    path = tmp_path / "s.yaml"
    path.write_text(sequence_text(2))
    (batch,) = list(fyaml.read_records(path))
    base = sys.getrefcount(batch)
    value = batch[0]
    assert sys.getrefcount(batch) == base + 1
    child = value["id"]
    assert sys.getrefcount(batch) == base + 2
    del value, child
    assert sys.getrefcount(batch) == base


_PEAK_SCRIPT = textwrap.dedent(
    """
    import resource, sys
    from dftracer.utils.formats import yaml
    path, batch_bytes = sys.argv[1], int(sys.argv[2])
    def peak():
        if sys.platform == "darwin":
            return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        with open("/proc/self/status") as status:
            return next(int(l.split()[1]) for l in status if l.startswith("VmHWM:")) * 1024
    for _ in yaml.read_records(path, batch_bytes=1 << 20):
        break
    before = peak()
    count = 0
    for batch in yaml.read_records(path, batch_bytes=batch_bytes):
        count += len(batch)
    print(count, before, peak())
    """
)


@pytest.mark.skipif(sys.platform == "win32", reason="peak memory is read with resource")
def test_read_records_holds_about_one_batch(tmp_path):
    path = tmp_path / "big.yaml.gz"
    text = sequence_text(1)
    count = 1_600_000
    with gzip.open(path, "wt") as f:
        chunk = text * 100_000
        for _ in range(count // 100_000):
            f.write(chunk)
    size = count * len(text)
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
    assert after - before < 3 * batch_bytes
    assert after - before < size // 3


RECORDS = [
    {"name": "demo", "n": 3, "list": [1, "two", {"k": None}], "text": "a: b # c\nline"},
    {"empty": {}, "arr": [], "nest": {"x": {"y": [True, 2.5]}}},
    ["a", ["b", "c"], {"d": 1}],
    {"tricky": ["yes", "no", "null", "1", "1.5", "", " pad ", "- dash", "k: v"]},
]


def as_text(data):
    if isinstance(data, dict):
        return {k: as_text(v) for k, v in data.items()}
    if isinstance(data, list):
        return [as_text(v) for v in data]
    if data is None:
        return None
    if isinstance(data, bool):
        return "true" if data else "false"
    return str(data)


@pytest.mark.parametrize("name", ["out.yaml", "out.yaml.gz"])
@pytest.mark.parametrize("mode", ["sequence", "documents"])
def test_a_written_file_reads_back(tmp_path, name, mode):
    path = tmp_path / name
    with fyaml.Writer(path, records=mode) as w:
        for r in RECORDS:
            w.write(r)
    raw = path.read_bytes()
    assert (raw[:2] == b"\x1f\x8b") == name.endswith(".gz")
    text = read_back(path)
    assert text.startswith("- " if mode == "sequence" else "---\n")
    expected = [as_text(r) for r in RECORDS]
    assert records_of(path) == expected
    assert records_of(path, batch_bytes=40) == expected
    if mode == "sequence":
        assert fyaml.load(path) == expected


def test_a_written_file_round_trips_through_read_and_write_again(tmp_path):
    first = tmp_path / "a.yaml"
    second = tmp_path / "b.yaml"
    third = tmp_path / "c.yaml"
    with fyaml.Writer(first) as w:
        for r in RECORDS:
            w.write(r)
    for source, target in ((first, second), (second, third)):
        with fyaml.Writer(target) as w:
            for b in fyaml.read_records(source):
                for v in b:
                    w.write(v)
    expected = [as_text(r) for r in RECORDS]
    assert records_of(second) == records_of(third) == expected
    assert third.read_bytes() == second.read_bytes()


def test_compress_and_indent_options(tmp_path):
    plain = tmp_path / "a.yaml.gz"
    with fyaml.Writer(plain, compress=False, indent=4) as w:
        w.write({"a": {"b": 1}})
    assert plain.read_bytes() == b"- a:\n      b: 1\n"
    packed = tmp_path / "b.yaml"
    with fyaml.Writer(packed, compress=True) as w:
        w.write({"a": 1})
    assert gzip.decompress(packed.read_bytes()) == b"- a: 1\n"


def test_record_builders_write_one_record(tmp_path):
    path = tmp_path / "b.yaml"
    with fyaml.Writer(path) as w:
        with w.record() as r:
            r.member("name", "demo")
            with r.object("args") as a:
                a.member("fd", 7)
                with a.array("sizes") as s:
                    s.value(1)
                    with s.object() as o:
                        o.member("deep", None)
            with r.array("tail") as t:
                t.value("x")
        w.write({"after": 1})
    assert records_of(path) == [
        {
            "name": "demo",
            "args": {"fd": "7", "sizes": ["1", {"deep": None}]},
            "tail": ["x"],
        },
        {"after": "1"},
    ]


def test_builders_raise_when_called_in_the_wrong_order(tmp_path):
    path = tmp_path / "o.yaml"
    w = fyaml.Writer(path)
    with w.record() as r:
        inner = r.array("a")
        with pytest.raises(RuntimeError):
            r.member("b", 1)
        with pytest.raises(RuntimeError):
            w.write({"x": 1})
        with inner:
            inner.value(1)
        r.member("b", 2)
    with pytest.raises(RuntimeError):
        r.member("after", 1)
    w.close()
    with pytest.raises(RuntimeError):
        w.write({"x": 1})
    assert records_of(path) == [{"a": ["1"], "b": "2"}]


@pytest.mark.parametrize("bad", [{1, 2}, b"bytes", object(), {"k": {1}}, 1j])
def test_a_value_outside_the_input_types_raises_type_error(tmp_path, bad):
    path = tmp_path / "t.yaml"
    with fyaml.Writer(path) as w:
        with pytest.raises(TypeError):
            w.write(bad)
        with pytest.raises(TypeError):
            w.write({"a": [1, {"b": bad}]})
        w.write({"ok": 1})
    assert records_of(path) == [{"ok": "1"}]


def test_a_key_that_is_not_text_raises_type_error(tmp_path):
    with fyaml.Writer(tmp_path / "k.yaml") as w:
        with pytest.raises(TypeError):
            w.write({1: "x"})


def test_nan_and_infinity_are_written_as_strings(tmp_path):
    path = tmp_path / "n.yaml"
    with fyaml.Writer(path) as w:
        w.write({"a": float("nan"), "b": float("inf"), "c": float("-inf")})
    assert records_of(path) == [{"a": "NaN", "b": "Infinity", "c": "-Infinity"}]


def test_writer_rejects_bad_options(tmp_path):
    with pytest.raises(ValueError):
        fyaml.Writer(tmp_path / "a.yaml", records="lines")
    with pytest.raises(ValueError):
        fyaml.Writer(tmp_path / "a.yaml", indent=0)
    with pytest.raises(TypeError):
        fyaml.Writer(123)
    with pytest.raises(ValueError):
        list(fyaml.read_records(tmp_path / "a.yaml", batch_bytes=-1))


def test_a_mutable_value_writes_its_changes_and_leaves_the_source(tmp_path):
    text = "a: 1\nb:\n  c: [1, 2, 3]\n  d: x\ne:\n  - f: 1\n  - f: 2\ng: 0\n"
    value = fyaml.parse(text)
    before = value.to_python()
    m = value.mutable()
    m["a"] = 10
    m["new"] = {"k": [1, 2]}
    del m["g"]
    m["b"]["c"].append(4)
    m["b"]["c"][0] = "first"
    del m["b"]["d"]
    m["e"][1]["f"] = 20
    del m["e"][0]
    m["e"].append({"f": 3})
    expected = {
        "a": 10,
        "b": {"c": ["first", "2", "3", 4]},
        "e": [{"f": 20}, {"f": 3}],
        "new": {"k": [1, 2]},
    }
    assert m.to_python() == expected
    assert value.to_python() == before
    out = tmp_path / "m.yaml"
    with fyaml.Writer(out) as w:
        w.write(m)
        w.write(value)
    assert records_of(out) == [as_text(expected), before]


def test_a_value_of_the_other_format_is_walked_and_written_again(tmp_path):
    j = fjson.parse('{"name":"demo","n":3,"list":[1.5,true,null,{"k":"v"}]}')
    out = tmp_path / "x.yaml"
    with fyaml.Writer(out) as w:
        w.write(j)
        w.write({"wrapped": j["list"], "m": j.mutable()})
    first, second = records_of(out)
    assert first == {
        "name": "demo",
        "n": "3",
        "list": ["1.5", "true", None, {"k": "v"}],
    }
    assert second == {"wrapped": first["list"], "m": first}


def test_a_writer_removes_its_output_when_the_block_raises(tmp_path):
    path = tmp_path / "gone.yaml.gz"
    with pytest.raises(ZeroDivisionError):
        with fyaml.Writer(path) as w:
            w.write({"a": 1})
            1 / 0
    assert not path.exists()


SEQUENCE_SOURCE = (
    "-   name: a\n"
    "    list:\n"
    "      - 1\n"
    "      -    2   # two\n"
    "- {x: 1,   y: [1,2]}   # trailing comment\n"
    "\n"
    '- k: "q\\u00e9"\n'
    "  # inner comment\n"
    "  z:   ~\n"
    "-\n"
    "  plain:    text\n"
)

DOCUMENTS_SOURCE = "---\na:   1\nb: [1,  2]\n--- \n# note\nc: 'x'\n\n---\n- p\n-    q\n"


@pytest.mark.parametrize(
    ("mode", "source"), [("sequence", SEQUENCE_SOURCE), ("documents", DOCUMENTS_SOURCE)]
)
@pytest.mark.parametrize("batch_bytes", [1, 1 << 20])
def test_a_record_written_unchanged_makes_the_same_bytes_for_any_input(
    tmp_path, mode, source, batch_bytes
):
    src = tmp_path / "in.yaml"
    src.write_text(source, encoding="utf-8")
    out = tmp_path / "out.yaml"
    with fyaml.Writer(out, records=mode) as w:
        for batch in fyaml.read_records(src, batch_bytes=batch_bytes):
            for v in batch:
                w.write(v)
    assert out.read_bytes() == src.read_bytes()
    raws = [v.raw for b in fyaml.read_records(src) for v in b]
    assert "".join(r + "\n" for r in raws) == source


def test_a_record_of_the_other_style_is_written_again_in_the_style_of_the_writer(tmp_path):
    src = tmp_path / "in.yaml"
    src.write_text(SEQUENCE_SOURCE, encoding="utf-8")
    out = tmp_path / "out.yaml"
    with fyaml.Writer(out, records="documents") as w:
        for b in fyaml.read_records(src):
            for v in b:
                w.write(v)
    assert out.read_text().startswith("---\n")
    assert records_of(out) == records_of(src)


def test_parse_and_read_keep_the_source_text_as_raw(tmp_path):
    text = "a:   1\nb: [1,  2]   # c\n"
    assert fyaml.parse(text).raw == text
    path = tmp_path / "d.yaml"
    path.write_text(text)
    assert fyaml.read(path).raw == text
    assert fyaml.parse(text)["a"].raw == "1"


def test_an_unchanged_mutable_record_writes_the_source_bytes(tmp_path):
    src = tmp_path / "in.yaml"
    src.write_text(SEQUENCE_SOURCE, encoding="utf-8")
    (batch,) = list(fyaml.read_records(src))
    m = batch[0].mutable()
    m["list"][0].as_int()
    out = tmp_path / "out.yaml"
    with fyaml.Writer(out) as w:
        w.write(m)
        m["list"][0] = "changed"
        w.write(m)
    text = out.read_text()
    assert text.startswith("-   name: a\n    list:\n      - 1\n      -    2   # two\n- ")
    assert records_of(out)[1]["list"][0] == "changed"


def test_every_element_of_a_large_array_is_indexed():
    count = 100_000
    value = fyaml.parse("[" + ",".join(str(i) for i in range(count)) + "]")
    total = 0
    for i in range(len(value)):
        total += value[i].as_int()
    assert total == count * (count - 1) // 2
    assert value[-1].as_int() == count - 1


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
    with fyaml.Writer(tmp_path / "o.yaml") as w:
        with pytest.raises(TypeError):
            w.write(bad)
        with pytest.raises(TypeError):
            w.write({"k": [bad]})
        w.write({"ok": 1})
    m = fyaml.parse("a: 1").mutable()
    m["x"] = bad
    with pytest.raises(TypeError):
        m.to_python()
    with fyaml.Writer(tmp_path / "p.yaml") as w:
        with pytest.raises(TypeError):
            w.write(m)


def test_a_writer_dropped_without_close_removes_its_output(tmp_path):
    path = tmp_path / "dropped.yaml.gz"
    w = fyaml.Writer(path)
    w.write({"a": 1})
    del w
    gc.collect()
    assert not path.exists()
