# SPDX-License-Identifier: MIT
"""Deliberately wrong Series usage: every line below must be flagged."""

from typing import cast

from typing_extensions import assert_type

from dftracer.utils import Series

ints = cast("Series[int]", None)
floats = cast("Series[float]", None)
strs = cast("Series[str]", None)

ints.str.upper()  # the string accessor is for Series[str]
ints.nope()  # no such method
ints.sum(1)  # sum takes no argument
ints.head("a")  # n is an int
ints["a"]  # an int or a slice
ints + "x"  # not a number
ints.take("x")  # indices, not text
ints.is_in(5)  # a list or a Series
ints.rolling("a")  # window is an int
ints.fillna([1])  # a scalar
ints.name = 5  # a str or None
ints.cast(1.5)  # a dtype
strs.rename(3)  # a str or None
strs.str.len().str.upper()  # len is an int column
strs.str.startswith(1)  # the prefix is a str
strs.str.zfill("x")  # the width is an int
strs.str.replace_all("a")  # both strings

as_str: "Series[str]" = ints  # a Series[int] is not a Series[str]
as_int: "Series[int]" = floats  # a Series[float] is not a Series[int]
as_bool: "Series[bool]" = ints  # nor a Series[bool]

assert_type(ints.abs(), "Series[str]")  # abs keeps the type
assert_type(ints > 1, "Series[int]")  # a comparison is a mask
assert_type(ints + 1.5, "Series[int]")  # a float promotes
assert_type(ints.cast(None), "Series[int]")  # not a dtype
