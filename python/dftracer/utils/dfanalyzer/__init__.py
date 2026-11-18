# SPDX-License-Identifier: MIT
"""Helpers bridging the C++ aggregation index to dfanalyzer.

These were previously vendored inside dfanalyzer; they belong here since they
only depend on the Indexer and Arrow plumbing.

Supported surface: the names in ``__all__`` are dfanalyzer's integration
contract and co-version with it - changing one means updating the matching
dfanalyzer release in the same bump. tests/python/test_dfanalyzer_contract.py
freezes the set so a rename or removal is caught here. Everything else
is internal and may change without notice; import it from its submodule
(``index``, ``partials`` or ``frames``).
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from .._lazy import lazy_names

if TYPE_CHECKING:
    from .frames import DFAnalyzerAggregatedTraceViewer as DFAnalyzerAggregatedTraceViewer
    from .frames import HLMConfig as HLMConfig
    from .frames import ReadFrames as ReadFrames
    from .frames import TypedFrames as TypedFrames
    from .frames import build_read_frames as build_read_frames
    from .frames import (
        coerce_arrow_numerics_to_pandas_native as coerce_arrow_numerics_to_pandas_native,
    )
    from .frames import typed_group_keys as typed_group_keys
    from .frames import view_typed_frames as view_typed_frames
    from .index import build_index_distributed as build_index_distributed
    from .index import count_index_files as count_index_files
    from .index import ensure_index as ensure_index
    from .index import index_dir_for as index_dir_for
    from .index import resolve_trace_inputs as resolve_trace_inputs
    from .partials import build_final_meta as build_final_meta
    from .partials import build_partial_meta as build_partial_meta
    from .partials import finalize_view_partials as finalize_view_partials
    from .partials import merge_view_partials as merge_view_partials
    from .partials import normalize_arrow_dtypes as normalize_arrow_dtypes
    from .partials import partial_arrow_view_groupby as partial_arrow_view_groupby

__all__ = [
    "DFAnalyzerAggregatedTraceViewer",
    "HLMConfig",
    "build_read_frames",
    "build_final_meta",
    "build_index_distributed",
    "build_partial_meta",
    "count_index_files",
    "coerce_arrow_numerics_to_pandas_native",
    "ensure_index",
    "typed_group_keys",
    "finalize_view_partials",
    "index_dir_for",
    "normalize_arrow_dtypes",
    "partial_arrow_view_groupby",
    "resolve_trace_inputs",
    "view_typed_frames",
]

_SUBMODULE = {
    "DFAnalyzerAggregatedTraceViewer": "frames",
    "HLMConfig": "frames",
    "ReadFrames": "frames",
    "TypedFrames": "frames",
    "build_read_frames": "frames",
    "coerce_arrow_numerics_to_pandas_native": "frames",
    "typed_group_keys": "frames",
    "view_typed_frames": "frames",
    "build_index_distributed": "index",
    "count_index_files": "index",
    "ensure_index": "index",
    "index_dir_for": "index",
    "resolve_trace_inputs": "index",
    "build_final_meta": "partials",
    "build_partial_meta": "partials",
    "finalize_view_partials": "partials",
    "merge_view_partials": "partials",
    "normalize_arrow_dtypes": "partials",
    "partial_arrow_view_groupby": "partials",
}

__getattr__, __dir__ = lazy_names(__name__, globals(), _SUBMODULE)
