"""Freeze the dftracer-utils surface dfanalyzer depends on.

dfanalyzer co-versions with dftracer-utils and imports a fixed set of helpers
from ``dftracer.utils.dfanalyzer`` plus a few names from ``dftracer.utils.dask``.
This test pins that surface so a rename, removal, or signature change is caught
here - a prompt that the matching dfanalyzer release must move in the same bump.
If you intend to change the surface, update the frozen sets below and ship the
dfanalyzer change alongside it.
"""

import inspect

import pytest

pytest.importorskip("pyarrow")

import dftracer.utils.dask as dfa_dask  # noqa: E402
import dftracer.utils.dfanalyzer as dfa  # noqa: E402

# The exact public surface of dftracer.utils.dfanalyzer that dfanalyzer imports.
DFANALYZER_SURFACE = {
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
    "index_path_for",
    "normalize_arrow_dtypes",
    "partial_arrow_view_groupby",
    "resolve_trace_inputs",
    "view_typed_frames",
}

# Names dfanalyzer imports from dftracer.utils.dask.
DASK_SURFACE = {
    "DaskTraceViewer",
    "DaskAggregatedTraceViewer",
    "ProgressAggregator",
    "register_auto_thread_plugin",
    "assign_files_by_pid",
}


def test_dfanalyzer_all_matches_the_frozen_contract():
    assert set(dfa.__all__) == DFANALYZER_SURFACE


def test_dask_all_covers_the_dfanalyzer_contract():
    # Every name dfanalyzer imports from dask must be an exported, importable
    # part of the dask surface.
    assert DASK_SURFACE <= set(dfa_dask.__all__)
    for name in DASK_SURFACE:
        assert hasattr(dfa_dask, name), name


def test_every_public_name_is_importable_and_callable():
    for name in DFANALYZER_SURFACE:
        obj = getattr(dfa, name)
        assert callable(obj), name


def test_load_bearing_signatures_are_stable():
    # Parameter names dfanalyzer passes by keyword or relies on positionally.
    assert list(inspect.signature(dfa.typed_group_keys).parameters) == ["file_buckets"]
    assert list(inspect.signature(dfa.index_path_for).parameters) == ["trace_path"]

    # The distributed typed-read entry point: dfanalyzer calls it positionally as
    # (time_granularity, time_resolution, group_keys, drop_file_patterns).
    ipc = inspect.signature(dfa_dask.DaskTraceViewer.collect_typed_ipc_futures)
    params = [p for p in ipc.parameters if p != "self"]
    assert params[:4] == [
        "time_granularity",
        "time_resolution",
        "group_keys",
        "drop_file_patterns",
    ]
