# SPDX-License-Identifier: MIT
"""Deliberately wrong frame usage: every line below must be flagged."""

from typing import cast

from typing_extensions import assert_type

from dftracer.utils import DataFrame, LazyFrame, Series, TraceViewer

df = cast("DataFrame", None)
lf = cast("LazyFrame", None)
tv = cast("TraceViewer", None)

df.nope()  # no such method
lf.nope()  # no such method
tv.nope()  # no such method
df.head("a")  # n is an int
lf.collect(batch_size="a")  # batch_size is an int
tv.time_range("a", "b")  # numbers
lf.to_pandas()  # a plan has no to_pandas
lf.columns = []  # the columns of a plan are read-only
df.shape = (1, 1)  # so is the shape

as_lazy: LazyFrame = df  # a DataFrame is not a plan
as_frame: DataFrame = lf  # nor is a plan a frame
as_series: Series[int] = df["x"]  # a column holds any scalar
as_viewer: TraceViewer = lf  # a plan is not a viewer

assert_type(df.head(), LazyFrame)  # head keeps the frame
assert_type(lf.collect(), LazyFrame)  # collect gives a frame
assert_type(tv.sort("ts"), TraceViewer)  # sort is a plain plan
assert_type(df["x"], Series[int])  # a column is untyped
