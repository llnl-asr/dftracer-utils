# SPDX-License-Identifier: MIT
"""Typed store sample: ty checks this clean and each assert_type holds."""

from typing import Dict, Iterator, List, Optional, Tuple, Type

from typing_extensions import assert_type

from dftracer.utils.store import (
    Batch,
    BytesLike,
    Encodable,
    Namespace,
    RawBatch,
    RawNamespace,
    Store,
    register_type,
)

store = Store.open("/tmp/store_typed_sample")

raw = store.namespace("raw", raw=True)
assert_type(raw, RawNamespace)

folded = store.namespace("folded", fold="sum_i64")
assert_type(folded, RawNamespace)

runs: Namespace[Tuple[str, int], Dict[str, str]] = store.namespace("runs")
assert_type(runs, Namespace[Tuple[str, int], Dict[str, str]])

checked = store.namespace("checked", key_type=str, value_type=int)
assert_type(checked, Namespace[str, int])

raw_batch = store.batch(raw, None)
assert_type(raw_batch, RawBatch)
raw_batch.put(b"k", b"v")
raw_batch.fold(b"k", b"v")
raw_batch.erase_range(b"a", None)

batch = store.batch(runs, None)
assert_type(batch, Batch[Tuple[str, int], Dict[str, str]])
batch.put(("a", 1), {"k": "v"})
batch.erase(("a", 1))
batch.erase_range(("a", 1), ("a", 9))

view = store.snapshot()

assert_type(view.get(raw, None, b"k"), Optional[bytes])
assert_type(view.multi_get(raw, None, [b"k"]), List[Optional[bytes]])
assert_type(view.scan(raw, None, b"k"), Iterator[Tuple[bytes, bytes]])

assert_type(view.get(runs, None, ("a", 1)), Optional[Dict[str, str]])
assert_type(view.multi_get(runs, None, [("a", 1)]), List[Optional[Dict[str, str]]])
assert_type(
    view.scan(runs, None, ("a",)),
    Iterator[Tuple[Tuple[str, int], Dict[str, str]]],
)
assert_type(view.get(checked, None, "k"), Optional[int])
view.scan(checked, None, "pre")
view.scan(checked, None)


class RunKey:
    store_type_id = 1

    def __init__(self, run: str) -> None:
        self.run = run

    def to_store(self) -> Tuple[str]:
        return (self.run,)

    @classmethod
    def from_store(cls, value: object) -> "RunKey":
        return cls("x")


assert_type(register_type(RunKey), Type[RunKey])


class Outside:
    def __init__(self, n: int) -> None:
        self.n = n


def outside_from(value: object) -> Outside:
    return Outside(0)


assert_type(
    register_type(Outside, 2, lambda o: o.n, outside_from),
    Type[Outside],
)


def takes_bytes(data: BytesLike) -> None: ...


def _returns_list(_: object) -> List[int]:
    return [1]


def _returns_dict(_: object) -> Dict[str, int]:
    return {"a": 1}


def _returns_nested(_: object) -> Dict[int, Dict[str, List[float]]]:
    return {1: {"a": [1.0]}}


def _returns_tuple_key(_: object) -> Dict[Tuple[str, int], List[int]]:
    return {("a", 1): [1]}


class _Outside:
    pass


def _from_value(_: Encodable) -> _Outside:
    return _Outside()


register_type(_Outside, 901, _returns_list, _from_value)
register_type(_Outside, 901, _returns_dict, _from_value)
register_type(_Outside, 901, _returns_nested, _from_value)
register_type(_Outside, 901, _returns_tuple_key, _from_value)
