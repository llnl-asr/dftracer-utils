# SPDX-License-Identifier: MIT
"""``jit.json(text)`` and ``jit.yaml(text)`` in a compiled body: a typed value with
``find``, ``as_int``, ``as_float``, ``as_str``, ``as_bool``, ``is_null`` and
iteration, lowered to the value tree of the format service (dftu.svc.json and
dftu.svc.yaml)."""

import gzip
import os
import shutil
import tempfile
from dataclasses import dataclass
from typing import Dict, Set

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

from .jit_common import assert_located, load_module

pa = pytest.importorskip("pyarrow")

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("value_trace")
    with gzip.open(os.path.join(d, "t.pfw.gz"), "wt", encoding="utf-8") as fh:
        for i in range(3):
            fh.write(
                f'{{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":{1000 + i},'
                f'"dur":{10 + i},"ph":"X","args":{{}}}}\n'
            )
    return str(d)


def _bytes(results):
    return {k: v for k, v in results.items() if isinstance(v, bytes)}


def _total(results, name):
    cols = pa.table(results[name]).to_pydict()
    return sum(cols["value"]) if cols["value"] else 0


_DOC = (
    '{"a": 7, "f": 2.5, "s": "hi", "ok": true, "off": false, "n": null, '
    '"arr": [1, 2, 3], "obj": {"x": 1, "y": {"z": "deep"}}, "big": 12345678901234}'
)


def _fin(tmp_path, body, name="values_fin", header=""):
    return load_module(
        tmp_path,
        name,
        f"""
        from dftracer.utils import jit
        {header}

        @jit.plugin
        class Fin:
            n: int = 0

            @jit.each_event
            def step(self, e):
                self.n += 1

            @jit.finalize
            def done(self):
{body}
        """,
    ).Fin


def _indent(text, n=16):
    return "\n".join(" " * n + ln for ln in text.strip("\n").splitlines())


def test_each_accessor(trace, tmp_path):
    body = _indent(
        f"""
d = jit.json({_DOC!r})
jit.emit("int", d.find("a").as_int())
jit.emit("float", d.find("f").as_float())
jit.emit("int_of_float", d.find("f").as_int())
jit.emit("float_of_int", d.find("a").as_float())
jit.emit("str", d.find("s").as_str())
jit.emit("true", d.find("ok").as_bool())
jit.emit("false", d.find("off").as_bool())
jit.emit("deep", d.find("obj").find("y").find("z").as_str())
jit.emit("nested", d.find("obj").find("x").as_int())
jit.emit("big", d.find("big").as_int())
jit.emit("len_arr", len(d.find("arr")))
jit.emit("len_obj", len(d.find("obj")))
jit.emit("len_root", len(d))
"""
    )
    got = _bytes(Plugins([_fin(tmp_path, body)]).run(trace).results)
    assert got == {
        "int": b"7",
        "float": b"2.5",
        "int_of_float": b"2",
        "float_of_int": b"7",
        "str": b"hi",
        "true": b"1",
        "false": b"0",
        "deep": b"deep",
        "nested": b"1",
        "big": b"12345678901234",
        "len_arr": b"3",
        "len_obj": b"2",
        "len_root": b"9",
    }


def test_is_null_and_the_default_rules(trace, tmp_path):
    body = _indent(
        f"""
d = jit.json({_DOC!r})
jit.emit("null_is_null", d.find("n").is_null())
jit.emit("missing_is_null", d.find("nope").is_null())
jit.emit("present_is_null", d.find("a").is_null())
jit.emit("missing_int", d.find("nope").as_int())
jit.emit("missing_int_default", d.find("nope").as_int(-4))
jit.emit("missing_float_default", d.find("nope").as_float(1.5))
jit.emit("missing_str", d.find("nope").as_str())
jit.emit("missing_str_default", d.find("nope").as_str("none"))
jit.emit("missing_bool", d.find("nope").as_bool())
jit.emit("missing_bool_default", d.find("nope").as_bool(True))
jit.emit("null_int_default", d.find("n").as_int(9))
jit.emit("str_as_int_default", d.find("s").as_int(3))
jit.emit("int_as_str_default", d.find("a").as_str("not text"))
jit.emit("chain_through_missing", d.find("nope").find("x").find("y").as_int(8))
jit.emit("find_on_a_number", d.find("a").find("x").is_null())
jit.emit("len_missing", len(d.find("nope")))
"""
    )
    got = _bytes(Plugins([_fin(tmp_path, body, "values_defaults")]).run(trace).results)
    assert got == {
        "null_is_null": b"1",
        "missing_is_null": b"1",
        "present_is_null": b"0",
        "missing_int": b"0",
        "missing_int_default": b"-4",
        "missing_float_default": b"1.5",
        "missing_str": b"",
        "missing_str_default": b"none",
        "missing_bool": b"0",
        "missing_bool_default": b"1",
        "null_int_default": b"9",
        "str_as_int_default": b"3",
        "int_as_str_default": b"not text",
        "chain_through_missing": b"8",
        "find_on_a_number": b"1",
        "len_missing": b"0",
    }


def test_iteration_over_arrays_and_members(trace, tmp_path):
    body = _indent(
        f"""
d = jit.json({_DOC!r})
total = 0
for item in d.find("arr"):
    total += item.as_int()
jit.emit("array_sum", total)
keys = 0
vals = 0
for key, value in d.find("obj").items():
    keys += len(key)
    vals += 1
jit.emit("key_chars", keys)
jit.emit("members", vals)
count = 0
for key, value in d.find("arr").items():
    count += 1
jit.emit("items_of_an_array", count)
nothing = 0
for item in d.find("nope"):
    nothing += 1
jit.emit("iter_missing", nothing)
matched = 0
for key, value in d.find("obj").items():
    if key == "x":
        matched += 1
jit.emit("matched_keys", matched)
"""
    )
    got = _bytes(Plugins([_fin(tmp_path, body, "values_iter")]).run(trace).results)
    assert got == {
        "array_sum": b"6",
        "key_chars": b"2",
        "members": b"2",
        "items_of_an_array": b"0",
        "iter_missing": b"0",
        "matched_keys": b"1",
    }


def test_yaml_scalars_are_text(trace, tmp_path):
    body = _indent(
        """
d = jit.yaml("a: 7\\nname: hello\\nempty:\\nlist:\\n  - 1\\n  - two\\n")
jit.emit("a_text", d.find("a").as_str())
jit.emit("a_int", d.find("a").as_int(-1))
jit.emit("name", d.find("name").as_str())
jit.emit("empty_null", d.find("empty").is_null())
count = 0
for item in d.find("list"):
    count += 1
jit.emit("list_len", count)
jit.emit("second", d.find("list").find("0").is_null())
"""
    )
    got = _bytes(Plugins([_fin(tmp_path, body, "values_yaml")]).run(trace).results)
    assert got == {
        "a_text": b"7",
        "a_int": b"-1",
        "name": b"hello",
        "empty_null": b"1",
        "list_len": b"2",
        "second": b"1",
    }


def test_a_parse_error_reaches_the_caller_with_the_position(trace, tmp_path):
    body = _indent(
        """
d = jit.json('{"a": 1,\\n "b": }')
jit.emit("a", d.find("a").as_int())
"""
    )
    with pytest.raises(jit.ParseError) as info:
        Plugins([_fin(tmp_path, body, "values_bad")]).run(trace)
    err = info.value
    assert isinstance(err, jit.Error)
    assert err.message.startswith("json: 2:")
    assert err.file is not None and err.line is not None and err.col is not None


def test_a_yaml_parse_error_is_a_parse_error(trace, tmp_path):
    body = _indent(
        """
d = jit.yaml("a: [1, 2")
jit.emit("a", 1)
"""
    )
    with pytest.raises(jit.ParseError, match="yaml: 1:"):
        Plugins([_fin(tmp_path, body, "values_bad_yaml")]).run(trace)


def test_except_parse_error_goes_on_with_the_next_text(trace, tmp_path):
    source = """
from dataclasses import dataclass
from dftracer.utils import jit

@dataclass(frozen=True)
class Cfg:
    path: str = ""

@jit.plugin
class Parses:
    config: Cfg
    good: int = 0
    bad: int = 0
    total: int = 0

    @jit.each_event
    def step(self, e):
        with jit.open(self.config.path) as f:
            for line in f:
                try:
                    d = jit.json(line)
                    self.good += 1
                    self.total += d.find("v").as_int()
                except jit.ParseError:
                    self.bad += 1
"""
    jsonl = tmp_path / "mixed.jsonl"
    jsonl.write_text('{"v": 5}\n{"v": \n{"v": 7}\nnot json\n', encoding="utf-8")
    module = load_module(tmp_path, "values_catch", source)
    results = Plugins([module.Parses], config={"Parses": {"path": str(jsonl)}}).run(trace).results
    assert _total(results, "good") == 2 * 3
    assert _total(results, "bad") == 2 * 3
    assert _total(results, "total") == 12 * 3


def test_json_lines_read_from_a_file_fold_into_plain_state(trace, tmp_path):
    lines = tmp_path / "events.jsonl"
    rows = [
        {"name": "read", "bytes": 10, "tags": ["a", "b"], "ok": True},
        {"name": "write", "bytes": 5, "tags": ["b"], "ok": False},
        {"name": "read", "bytes": 1, "tags": [], "ok": True},
    ]
    import json

    lines.write_text("".join(json.dumps(r) + "\n" for r in rows), encoding="utf-8")

    @dataclass(frozen=True)
    class Cfg:
        path: str = ""

    @jit.plugin
    class Fold:
        config: Cfg
        bytes_by_name: Dict[str, int] = {}
        tags: Set[str] = set()
        passed: int = 0

        @jit.each_event
        def step(self, e):
            with jit.open(self.config.path) as f:
                for line in f:
                    d = jit.json(line)
                    name = d.find("name").as_str()
                    self.bytes_by_name[name] = (
                        self.bytes_by_name.get(name, 0) + d.find("bytes").as_int()
                    )
                    for tag in d.find("tags"):
                        self.tags.add(tag.as_str())
                    if d.find("ok").as_bool():
                        self.passed += 1

    results = Plugins([Fold], config={"Fold": {"path": str(lines)}}).run(trace).results
    table = pa.table(results["bytes_by_name"]).to_pydict()
    assert dict(zip(table["k0"], table["value"])) == {"read": 11 * 3, "write": 5 * 3}
    assert sorted(pa.table(results["tags"]).to_pydict()["value"][0].split("\x1e")) == ["a", "b"]
    assert _total(results, "passed") == 2 * 3


def test_many_rows_from_a_loop_grow_the_batch_buffers(trace, tmp_path):
    big = tmp_path / "many.txt"
    big.write_text("".join(f"line{i}\n" for i in range(700)), encoding="utf-8")

    @dataclass(frozen=True)
    class Cfg:
        path: str = ""

    @jit.plugin
    class Many:
        config: Cfg
        total: int = 0
        names: Set[str] = set()

        @jit.each_event
        def step(self, e):
            with jit.open(self.config.path) as f:
                for line in f:
                    self.total += 1
                    self.names.add(line)

    results = Plugins([Many], config={"Many": {"path": str(big)}}).run(trace).results
    assert _total(results, "total") == 700 * 3
    assert len(pa.table(results["names"]).to_pydict()["value"][0].split("\x1e")) == 700


def test_a_value_is_reassigned_in_a_loop_without_leaking(trace, tmp_path):
    jsonl = tmp_path / "rows.jsonl"
    jsonl.write_text("".join(f'{{"v": {i}}}\n' for i in range(300)), encoding="utf-8")

    @dataclass(frozen=True)
    class Cfg:
        path: str = ""

    @jit.plugin
    class Loop:
        config: Cfg
        total: int = 0

        @jit.each_event
        def step(self, e):
            with jit.open(self.config.path) as f:
                for line in f:
                    d = jit.json(line)
                    self.total += d.find("v").as_int()

    results = Plugins([Loop], config={"Loop": {"path": str(jsonl)}}).run(trace).results
    assert _total(results, "total") == sum(range(300)) * 3


# ---- compile errors --------------------------------------------------------


def _err(source):
    with tempfile.TemporaryDirectory() as tmp:
        with pytest.raises(jit.JitError) as info:
            load_module(tmp, "bad_value", source)
    assert_located(info.value)
    return info.value


_HEAD = """
from dftracer.utils import jit

@jit.plugin
class Bad:
    n: int = 0

    @jit.each_event
    def step(self, e):
"""


def test_a_json_value_is_not_a_number_or_a_string():
    err = _err(
        _HEAD
        + """
        d = jit.json("{}")
        self.n += d.find("a")
"""
    )
    assert "JSON value" in err.reason
    assert err.source is not None and "d.find" in err.source


def test_the_text_is_a_string():
    err = _err(
        _HEAD
        + """
        d = jit.json(5)
"""
    )
    assert "text of jit.json is a string" in err.reason


def test_the_key_of_find_is_a_literal():
    err = _err(
        _HEAD
        + """
        d = jit.json("{}")
        x = d.find(e.name).as_int()
"""
    )
    assert "string literal or a str config field" in err.reason


def test_an_unknown_method_of_a_value_is_a_compile_error():
    err = _err(
        _HEAD
        + """
        d = jit.json("{}")
        x = d.get("a")
"""
    )
    assert err.line is not None and err.source is not None and "d.get" in err.source


def test_a_value_has_no_truth_value():
    err = _err(
        _HEAD
        + """
        d = jit.json("{}")
        if d.find("a"):
            self.n += 1
"""
    )
    assert "JSON value" in err.reason


def test_a_local_keeps_one_type():
    err = _err(
        _HEAD
        + """
        x = 1
        x = "text"
"""
    )
    assert "keeps one type" in err.reason


def test_an_unknown_name_is_a_compile_error_with_the_location():
    err = _err(
        _HEAD
        + """
        self.n += nope
"""
    )
    assert "unknown name nope" in err.reason
    assert err.source is not None and "nope" in err.source
