# SPDX-License-Identifier: MIT
"""Typed Series constructors: mypy --strict checks this clean.

The element type of ``Series([...])`` follows the list through the typed
``__init__`` overloads. ty does not read a self-typed ``__init__``, so this
file is checked by mypy only.
"""

from typing_extensions import assert_type

from dftracer.utils import Series
from dftracer.utils.dftracer_utils_ext import Scalar

assert_type(Series([1, 2, 3]), "Series[int]")
assert_type(Series([1.5, 2.5]), "Series[float]")
assert_type(Series(["a", "b"]), "Series[str]")
assert_type(Series([True, False]), "Series[bool]")
assert_type(Series([1, 2], name="x"), "Series[int]")
assert_type(Series([1, 2]).abs(), "Series[int]")
assert_type(Series([1, 2]) + 1.5, "Series[float]")
assert_type(Series(["a"]).str.upper(), "Series[str]")
assert_type(Series([1, 2]).cast(Series([1]).dtype), Series[Scalar])
assert_type(Series(Series([1, 2])), Series[Scalar])
