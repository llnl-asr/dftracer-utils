# SPDX-License-Identifier: MIT
"""Deliberately-wrong typed store usage: ty must flag each case below."""

from typing import Dict, Tuple

from dftracer.utils.store import Encodable, Namespace, Store, register_type

store = Store.open("/tmp/store_typed_wrong")
runs: Namespace[Tuple[str, int], Dict[str, str]] = store.namespace("runs")
raw = store.namespace("raw", raw=True)
batch = store.batch(runs, None)
view = store.snapshot()

batch.put(("a", "not an int"), {"k": "v"})  # wrong key type in put
batch.put(("a", 1), {"k": 1})  # wrong value type in put
batch.fold(b"k", b"v")  # fold on a typed batch
view.get(runs, None, "a")  # wrong key type in get
view.get(raw, None, "a")  # str key on a raw namespace
store.namespace("one", key_type=str)  # only one of key_type and value_type


class Plain:
    pass


register_type(Plain)  # no store protocol


class Other:
    pass


def to_loose(_: Other) -> Dict[str, object]:
    return {}


def from_any(_: Encodable) -> Other:
    return Other()


register_type(Other, 902, to_loose, from_any)  # a value of unknown type
