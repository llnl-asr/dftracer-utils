# SPDX-License-Identifier: MIT
"""Deliberately wrong public API usage: every line below must be flagged."""

from typing import List, cast

from typing_extensions import assert_type

from dftracer.utils import DataFrame, GroupBy, LazyFrame, LazyResult
from dftracer.utils import collect_all as collect_all
from dftracer.utils.index import Indexer
from dftracer.utils.trace import Handle, TraceViewer

df = cast("DataFrame", None)
lf = cast("LazyFrame", None)
tv = cast("TraceViewer", None)
ix = cast("Indexer", None)
count = cast("LazyResult[int]", None)
handle = cast("Handle[int]", None)

assert_type(df.group_by("name"), DataFrame)  # no aggregates gives a GroupBy
assert_type(df.group_by("name", "count"), GroupBy)  # aggregates give a frame
assert_type(collect_all([lf]), List[int])  # plans collect to frames
assert_type(collect_all([count]), List[DataFrame])  # a lazy int collects to an int
assert_type(handle.result(), str)  # the handle holds an int
ix.manifest()[0]["nope"]  # no such key
ix.explain(1)  # the query is text
tv.sink(1)  # the path is text
ix.ensure().nope  # no such field
