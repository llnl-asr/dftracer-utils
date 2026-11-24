# SPDX-License-Identifier: MIT
"""JSON and YAML readers and writers.

``formats.json`` and ``formats.yaml`` have the same names. ``read`` and
``parse`` give a ``Value``, a read-only view of the parsed text that keeps its
document or record batch alive; ``read_records`` streams a file a batch at a
time; ``Writer`` writes records. ``Input`` is what a writer takes, ``Data`` is
what ``to_python`` gives, and ``ValueLike`` is what a ``Value`` of either
format and a ``MutableValue`` have in common.

The modules are named ``json`` and ``yaml`` like the standard ones, so a file
that also uses the standard ``json`` imports this package as a module::

    from dftracer.utils import formats

    formats.json.read("trace.json")
"""

from __future__ import annotations

from typing import Dict, Iterator, List, Mapping, Protocol, Sequence, Union

__all__ = ["Data", "Input", "Scalar", "ValueLike"]

Scalar = Union[None, bool, int, float, str]

# What ``to_python`` gives: the plain data under a value.
Data = Union[Scalar, Dict[str, "Data"], List["Data"]]

# What a writer takes: a scalar, a mapping with str keys, a sequence, or a value
# of either format.
Input = Union[Scalar, Mapping[str, "Input"], Sequence["Input"], "ValueLike"]


class ValueLike(Protocol):
    """What a ``Value`` of either format and a ``MutableValue`` share, so one
    function can take any of them. ``kind`` is not here, because the two
    ``Kind`` enums differ."""

    def __getitem__(self, key: str | int) -> ValueLike: ...

    def get(self, key: str) -> ValueLike | None: ...

    def items(self) -> Iterator[tuple[str, ValueLike]]: ...

    def __iter__(self) -> Iterator[ValueLike]: ...

    def __len__(self) -> int: ...

    def as_int(self) -> int: ...

    def as_float(self) -> float: ...

    def as_bool(self) -> bool: ...

    def as_str(self) -> str: ...

    def to_python(self) -> Data: ...


from dftracer.utils.formats import json as json  # noqa: E402
from dftracer.utils.formats import yaml as yaml  # noqa: E402
