# SPDX-License-Identifier: MIT
"""Deliberately-wrong typed DataFrame-ops usage: ty must flag every line below.

Each marked call is a real authoring mistake the typed surface now catches
statically: a bad enum value, a mistyped spec function name, a wrong-arity spec,
and a wrong-typed spec argument.
"""

from typing import cast

from dftracer.utils import DataFrame

df = cast(DataFrame, None)

df.join(df, "k", "innr")  # not a valid how literal
df.gap_fill(["pid"], "ts", 10, "v", "bogus")  # not a valid mode literal
df.asof(df, "ts", direction="sideways")  # not a valid direction literal

df.window(specs=[("row_numbre", "rn")])  # misspelled spec function name
df.window(specs=[("lag", "dur", "out")])  # missing the int offset (wrong arity)
df.window(specs=[("ntile", "two", "q")])  # n must be int, not str
