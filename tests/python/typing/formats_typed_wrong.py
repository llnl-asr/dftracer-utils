# SPDX-License-Identifier: MIT
"""Deliberately wrong formats usage: every line below must be flagged."""

from typing import cast

from dftracer.utils.formats import Input
from dftracer.utils.formats import json as fjson

jv = cast("fjson.Value", None)
w = cast("fjson.Writer", None)
r = cast("fjson.ObjectBuilder", None)
a = cast("fjson.ArrayBuilder", None)

r.member(1, "x")  # a key is text
a.member("k", 1)  # an array builder has no members
a.value({1, 2})  # a set is not an input
w.write({1, 2})  # a set is not an input
w.write({"k": {3}})  # a set below a dict is not an input
jv.as_int(1)  # as_int takes nothing
jv.mutable().bogus()  # no such method
fjson.parse(b"{}")  # parse takes text
wrong: Input = {1, 2}  # a set is not an input
