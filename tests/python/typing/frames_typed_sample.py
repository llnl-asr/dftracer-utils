# SPDX-License-Identifier: MIT
"""Typed DataFrame, LazyFrame and TraceViewer sample: ty and mypy --strict
check this clean.

The frames subclass the native types, so a method that keeps the type returns
``Self`` and a column is a ``Series``. ``filter`` still takes the untyped
``Expr``; ``Expr`` becomes generic in a later step.
"""

from typing import Dict, List, Sequence, Tuple, cast

from typing_extensions import assert_type

from dftracer.utils import DataFrame, LazyFrame, Series, TraceViewer
from dftracer.utils.dftracer_utils_ext import Scalar
from dftracer.utils.enums import DType

df = cast("DataFrame", None)
lf = cast("LazyFrame", None)
tv = cast("TraceViewer", None)
mask = cast("Series[bool]", None)

# DataFrame: access and shape.
assert_type(df["x"], Series[Scalar])
assert_type(df[["x", "y"]], DataFrame)
assert_type(df[mask], DataFrame)
assert_type(df.columns, List[str])
assert_type(df.shape, Tuple[int, int])
assert_type(len(df), int)
assert_type(df.num_rows, int)
assert_type("x" in df, bool)
assert_type(df.to_dict(), Dict[str, Sequence[object]])

# DataFrame: methods that keep the type return Self.
assert_type(df.head(), DataFrame)
assert_type(df.tail(2), DataFrame)
assert_type(df.reverse(), DataFrame)
assert_type(df.drop_nulls(), DataFrame)
assert_type(df.fill_null(0), DataFrame)
assert_type(df.slice(0, 2), DataFrame)
assert_type(df.take([0, 1]), DataFrame)
assert_type(df.unique(), DataFrame)
assert_type(df.sort_by("x"), DataFrame)
assert_type(df.filter(mask), DataFrame)
assert_type(df.select("x"), DataFrame)
assert_type(df.rename({"x": "y"}), DataFrame)
assert_type(df.with_column("z", df["x"]), DataFrame)
assert_type(df.copy(), DataFrame)
assert_type(df.share(), DataFrame)

# DataFrame: results of other kinds.
assert_type(df.lazy(), LazyFrame)
assert_type(df.to_bytes(), bytes)
assert_type(df.to_ipc(), bytes)
assert_type(df.column_index("x"), int)
assert_type(df.keys(), List[str])

# LazyFrame: a plan returns a plan, collect returns a frame.
assert_type(lf.head(), LazyFrame)
assert_type(lf.tail(), LazyFrame)
assert_type(lf.reverse(), LazyFrame)
assert_type(lf.drop_nulls(), LazyFrame)
assert_type(lf.slice(0, 2), LazyFrame)
assert_type(lf.sort_by("x"), LazyFrame)
assert_type(lf.select("x"), LazyFrame)
assert_type(lf.drop("x"), LazyFrame)
assert_type(lf.share(), LazyFrame)
assert_type(lf.collect(), DataFrame)
assert_type(lf.columns, List[str])
assert_type(lf.schema, Dict[str, DType])
assert_type(lf.explain(), str)
assert_type(lf.join(lf, on="x"), LazyFrame)

# TraceViewer: the builders keep the viewer, the other ops give a plain plan.
assert_type(tv.filter("dur > 1"), TraceViewer)
assert_type(tv.select("name"), TraceViewer)
assert_type(tv.head(3), TraceViewer)
assert_type(tv.sort_by("ts"), TraceViewer)
assert_type(tv.phase("events"), TraceViewer)
assert_type(tv.time_range(0.0, 1.0), TraceViewer)
assert_type(tv.group_by("name"), TraceViewer)
assert_type(tv.lazy(), LazyFrame)
assert_type(tv.collect(), DataFrame)
assert_type(tv.sort("ts"), LazyFrame)
assert_type(tv.join(lf, on="name"), LazyFrame)
assert_type(tv.columns, List[str])
assert_type(tv.explain(), str)
