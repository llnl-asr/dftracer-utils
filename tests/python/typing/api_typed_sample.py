# SPDX-License-Identifier: MIT
"""Typed public API sample: ty and mypy --strict check this clean.

Covers the group-by forms, ``collect_all`` and the session handles, the
``Expr`` namespaces and ``Indexer`` results.
"""

from typing import Dict, List, Set, Union, cast

from typing_extensions import assert_type

from dftracer.utils import DataFrame, Expr, GroupBy, LazyFrame, LazyResult, SinkSummary, col
from dftracer.utils import collect_all as collect_all
from dftracer.utils.index import ExplainEntry, Indexer, IndexStatus, ManifestEntry
from dftracer.utils.schemas import JsonValue
from dftracer.utils.trace import Handle, TraceViewer

df = cast("DataFrame", None)
lf = cast("LazyFrame", None)
tv = cast("TraceViewer", None)
ix = cast("Indexer", None)
count = cast("LazyResult[int]", None)

# group_by without aggregates is a lazy GroupBy; with aggregates it is a frame.
assert_type(df.group_by("name"), GroupBy)
assert_type(df.group_by(None), GroupBy)
assert_type(df.group_by("name", "count"), DataFrame)
assert_type(df.groupby("name"), GroupBy)
assert_type(df.groupby("name", "sum:dur"), DataFrame)
assert_type(df.group_by("name").agg(col("dur").sum()), DataFrame)

# collect_all follows what it is given.
assert_type(collect_all([lf, tv]), List[DataFrame])
assert_type(collect_all([count]), List[int])
mixed: List[Union[LazyFrame, LazyResult[int]]] = [lf, count]
assert_type(collect_all(mixed), List[Union[DataFrame, int]])

# The session hands back typed handles.
with tv.session() as session:
    frame_handle = session.collect(lf)
    value_handle = session.collect(count)
    sink_handle = session.sink_trace(tv, "out.pfw.gz")
    assert_type(frame_handle, Handle[DataFrame])
    assert_type(value_handle, Handle[int])
    assert_type(sink_handle, Handle[SinkSummary])
assert_type(frame_handle.result(), DataFrame)
assert_type(value_handle.result(), int)

# Expr namespaces are typed: str and dt build expressions.
assert_type(col("name").str.contains("x"), Expr)

# Indexer results are typed dicts and a status record.
assert_type(ix.ensure(), IndexStatus)
assert_type(ix.manifest(), List[ManifestEntry])
assert_type(ix.explain("size == 1"), List[ExplainEntry])
assert_type(ix.manifest()[0]["extensions"][0]["name"], str)
assert_type(ix.query_file_pids("f"), Set[int])

# A document read from JSON has the JSON value type.
tree: Dict[str, JsonValue] = tv.schema_tree()
