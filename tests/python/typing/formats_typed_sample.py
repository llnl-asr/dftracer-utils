# SPDX-License-Identifier: MIT
"""Typed formats sample: mypy --strict and ty check this clean.

Covers the readers, the writers and builders, the overlay, the shared
protocol and the input and data types.
"""

from typing import Iterator, List, Tuple, cast

from typing_extensions import assert_type

from dftracer.utils.formats import Data, Input, ValueLike
from dftracer.utils.formats import json as fjson
from dftracer.utils.formats import yaml as fyaml

jv = fjson.parse("{}")
yv = fyaml.parse("a: 1")
assert_type(jv, fjson.Value)
assert_type(yv, fyaml.Value)
assert_type(fjson.read("a.json"), fjson.Value)
assert_type(fyaml.read("a.yaml"), fyaml.Value)
assert_type(fjson.loads("{}"), Data)
assert_type(fyaml.load("a.yaml"), Data)
assert_type(jv["a"], fjson.Value)
assert_type(jv[0], fjson.Value)
assert_type(jv.get("a"), "fjson.Value | None")
assert_type(jv.items(), Iterator[Tuple[str, fjson.Value]])
assert_type(iter(jv), Iterator[fjson.Value])
assert_type(len(jv), int)
assert_type(jv.kind, fjson.Kind)
assert_type(yv.kind, fyaml.Kind)
assert_type(jv.raw, str)
assert_type(jv.as_int(), int)
assert_type(jv.as_float(), float)
assert_type(jv.as_bool(), bool)
assert_type(jv.as_str(), str)
assert_type(jv.to_python(), Data)
assert_type(jv.mutable(), fjson.MutableValue)
assert_type(yv.mutable(), fyaml.MutableValue)

for batch in fjson.read_records("a.jsonl", batch_bytes=1 << 20):
    assert_type(batch, fjson.RecordBatch)
    assert_type(len(batch), int)
    assert_type(batch[0], fjson.Value)
    for record in batch:
        assert_type(record, fjson.Value)
for ybatch in fyaml.read_records("a.yaml"):
    assert_type(ybatch, fyaml.RecordBatch)

m = jv.mutable()
m["a"] = 1
m["b"] = {"c": [1, 2.5, "x", None, True]}
m[0] = jv
del m["a"]
m.append({"k": (1, 2)})
assert_type(m["a"], fjson.MutableValue)
assert_type(m.get("a"), "fjson.MutableValue | None")
assert_type(m.to_python(), Data)

with fjson.Writer("out.jsonl.gz", level=3, member_size=1 << 20, compress=None) as w:
    w.write({"a": [1, "b", None]})
    w.write(jv)
    w.write(yv)
    w.write(m)
    with w.record() as r:
        assert_type(r, fjson.ObjectBuilder)
        r.member("a", 1)
        r.member("b", jv)
        with r.object("o") as o:
            o.member("k", {"x": [1]})
        with r.array("l") as a:
            assert_type(a, fjson.ArrayBuilder)
            a.value(1)
            a.value(m)
            with a.object() as ao:
                ao.member("z", None)
            with a.array() as aa:
                aa.value("q")

with fyaml.Writer("out.yaml", records="documents", compress=False, indent=4) as yw:
    yw.write({"a": 1})
    with yw.record() as yr:
        yr.member("k", "v")

with fyaml.Writer("out2.yaml") as yw2:
    yw2.write(yv)


def takes_like(value: ValueLike) -> Data:
    first = value[0]
    assert_type(first, ValueLike)
    maybe = value.get("k")
    assert_type(maybe, "ValueLike | None")
    for key, item in value.items():
        assert_type(key, str)
        assert_type(item, ValueLike)
    for element in value:
        assert_type(element, ValueLike)
    return value.to_python() if len(value) else value.as_str()


takes_like(jv)
takes_like(yv)
takes_like(m)
takes_like(yv.mutable())
inputs: List[Input] = [None, True, 1, 2.5, "s", [1, "a"], {"k": (1, 2)}, jv, yv, m]
data: Data = {"a": [1, 2, {"b": None}]}
text = cast("Data", None)
