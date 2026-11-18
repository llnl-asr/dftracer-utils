# SPDX-License-Identifier: MIT
"""Resolve trace inputs and build or find the aggregation index."""

from __future__ import annotations

import fnmatch
import glob
import json
import os
from typing import (
    TYPE_CHECKING,
    Callable,
    Sequence,
)

from ..dask.index import DistributedIndexResult, distributed_index
from ..dftracer_utils_ext import INPUT_FILE_PATTERNS
from ..index import AggregationConfig, open_readonly_indexer

if TYPE_CHECKING:
    from dask.distributed import Client


def resolve_trace_inputs(
    trace_path: str,
    trace_groups: list[str] | None,
) -> tuple[str, list[str] | None]:
    """Resolve a trace path into (directory, files) for the Indexer.

    If trace_path is a directory containing manifest.json and trace_groups is
    set, glob only the subdirs for the requested groups. Otherwise return
    (directory, None) or ("", files).
    """
    if not os.path.isdir(trace_path):
        matched = glob.glob(trace_path) if "*" in trace_path else [trace_path]
        found = [
            f
            for f in matched
            if any(fnmatch.fnmatchcase(os.path.basename(f), p) for p in INPUT_FILE_PATTERNS)
        ]
        return "", found

    manifest_path = os.path.join(trace_path, "manifest.json")
    if not os.path.isfile(manifest_path):
        if trace_groups:
            raise FileNotFoundError(
                f"trace_groups={trace_groups} requested but no manifest.json at "
                f"{manifest_path}. Provide a manifest.json, or unset "
                "trace_groups."
            )
        return trace_path, None

    with open(manifest_path) as f:
        manifest = json.load(f)
    group_map = manifest.get("groups") or {}

    selected = trace_groups if trace_groups else sorted(group_map.keys())
    missing = [g for g in selected if g not in group_map]
    if missing:
        raise KeyError(
            f"trace_groups {missing} not found in manifest at {manifest_path}; "
            f"available groups: {sorted(group_map.keys())}"
        )

    files: list[str] = []
    for g in selected:
        subdir = os.path.join(trace_path, group_map[g])
        for pattern in INPUT_FILE_PATTERNS:
            files.extend(glob.glob(os.path.join(subdir, pattern)))
    return "", files


def active_client() -> Client | None:
    """The active Dask client, or None when ``dask.distributed`` is not installed
    or no client is active."""
    try:
        from dask.distributed import get_client
    except ImportError:
        return None
    try:
        return get_client()
    except ValueError:
        return None


def count_index_files(
    files: list[str], index_dir: str, ignored_patterns: tuple[str, ...] = ()
) -> int:
    """Distinct data files in the index, skipping paths containing any of
    `ignored_patterns`.

    Counts from the index's ``files`` row set rather than the read, so it stays exact
    when the read is folded to a grain with no per-file rows. Callers that
    filter files out of the analysis must pass the same patterns here, or the
    count reports files the analysis never used.
    """
    rows = open_readonly_indexer(files, index_dir).rowset("files").to_dict()
    table = {str(k): str(v) for k, v in zip(rows["fhash"], rows["path"])}
    if not ignored_patterns:
        return len(table)
    return sum(1 for name in table.values() if not any(p in name for p in ignored_patterns))


def index_dir_for(trace_path: str) -> str:
    """Convention: the dftracer index lives next to the traces as `.dftindex`.

    For a directory that's `<trace_path>/.dftindex`; for a file or glob it is
    `<dirname>/.dftindex` of the file (or first match).
    """
    if os.path.isdir(trace_path):
        return os.path.join(trace_path, ".dftindex")
    if "*" in trace_path:
        matches = sorted(glob.glob(trace_path))
        if matches:
            return os.path.join(os.path.dirname(matches[0]), ".dftindex")
    return os.path.join(os.path.dirname(trace_path) or ".", ".dftindex")


def build_index_distributed(
    files: str | os.PathLike[str] | Sequence[str | os.PathLike[str]],
    index_dir: str = "",
    client: Client | None = None,
    aggregation: AggregationConfig | None = None,
    progress: Callable[[int, int, str], None] | None = None,
) -> DistributedIndexResult:
    """Build the dftracer index across a Dask cluster.

    Workers write their files straight into the index. Every build fills in the
    aggregation tier; `aggregation` customizes its config and defaults to the
    standard one.

    If `client` is None the active Dask client is looked up; if none exists
    (or `dask.distributed` is not installed), tasks run inline serially.

    `progress`, if given, is called with (files_done, total_files) as the
    parse fans out across workers.
    """
    if client is None:
        client = active_client()
    if client is not None:
        from ..dask.worker import register_auto_thread_plugin

        register_auto_thread_plugin()
    return distributed_index(
        files,
        index_dir=index_dir,
        client=client,
        aggregation_config=aggregation,
        progress=progress,
    )


def ensure_index(
    trace_path: str,
    trace_groups: list[str] | None,
    time_interval_ms: float | str,
    client: Client | None = None,
    progress: Callable[[int, int, str], None] | None = None,
    group_by_file: bool = True,
) -> None:
    """Build (or refresh) the dftracer index for `trace_path` via Dask.

    Idempotent: dftracer-utils skips files whose tiers already exist, so repeat
    calls on the same path are cheap no-ops. With no active Dask client (or no
    `dask.distributed`), the build runs inline serially.

    `group_by_file=False` keeps the file hash out of the aggregation key, which
    is far smaller on traces touching many files but makes per-file queries
    impossible without re-reading the trace. It is part of the index identity,
    so changing it invalidates an existing aggregation.
    """
    if client is None:
        client = active_client()
    directory, files = resolve_trace_inputs(trace_path, trace_groups)
    if not directory and not files:
        return
    index_dir = index_dir_for(trace_path)
    build_index_distributed(
        directory or files or [],
        index_dir=index_dir,
        client=client,
        aggregation=AggregationConfig(
            time_interval_ms=time_interval_ms, group_by_file=group_by_file
        ),
        progress=progress,
    )


# Columns the in-scan fold can produce, so a caller can only ask for what the
# scan knows how to group on.
_SCAN_GROUP_COLUMNS = frozenset(
    {
        "cat",
        "func_name",
        "pid",
        "tid",
        "file_hash",
        "host_hash",
        "file_name",
        "host_name",
        "proc_name",
        "io_cat",
        "acc_pat",
        "time_range",
    }
)


# dfanalyzer view_type / HLM dim -> View group-key token. proc_name is composed
