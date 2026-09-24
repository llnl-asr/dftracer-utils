"""Helpers bridging the C++ aggregation index to dfanalyzer.

These were previously vendored inside dfanalyzer; they belong here since they
only depend on the Indexer and Arrow plumbing.

Supported surface: the names in ``__all__`` are dfanalyzer's integration
contract and co-version with it - changing one means updating the matching
dfanalyzer release in the same bump. tests/python/test_dfanalyzer_contract.py
freezes the set so a rename or removal is caught here. Everything else
(``_``-prefixed) is internal and may change without notice.
"""

from __future__ import annotations

import glob
import json
import os
from dataclasses import dataclass
from typing import (
    TYPE_CHECKING,
    Callable,
    Dict,
    List,
    Optional,
    Sequence,
    Tuple,
    TypedDict,
    Union,
)

import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.compute as pc

from .arrow import decode_dictionary_columns, ipc_to_table
from .dask import (
    DaskAggregatedTraceViewer,
    distributed_index,
    register_auto_thread_plugin,
    resolve_local_staging,
)
from .indexer import AggregationConfig, _open_readonly_indexer

if TYPE_CHECKING:
    from dask.distributed import Client  # ty: ignore[unresolved-import]

    from .dask import DaskTraceViewer
    from .trace_viewer import TraceViewer

try:
    from dask.distributed import get_client
except ImportError:
    get_client = None  # ty: ignore[invalid-assignment]

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
    "index_path_for",
    "normalize_arrow_dtypes",
    "partial_arrow_view_groupby",
    "resolve_trace_inputs",
    "view_typed_frames",
]

_TRACE_SUFFIXES = (".pfw.gz",)


def _capsule_to_pandas(table):
    """Arrow table/capsule to pandas; empty DataFrame when None."""
    return pa.table(table).to_pandas() if table is not None else pd.DataFrame()


def _ipc_to_pandas(ipc_bytes: bytes):
    """Decode Arrow IPC bytes to pandas, casting dictionary columns to string."""
    return decode_dictionary_columns(ipc_to_table(ipc_bytes)).to_pandas()


# View group_by + agg that produces every dfanalyzer metric column below.
_VIEW_METRIC_AGGS = (
    "count",
    "sum:dur",
    "sum:size",
    "sumsq:dur",
    "sumsq:size",
    "min:dur",
    "max:dur",
    "min:size",
    "max:size",
    "min:ts",
    "max:te",  # te = ts + dur, so max:te is the group's end time
)


# Full-grain group_by for the typed read: the dims the HLM groups/derives on.
# Resolved file_name/host_name (not raw hashes) since the HLM uses names for the
# POSIX-category rules and proc_name.
# file_path (full resolved path), not file_name (basename): dfanalyzer's
# file_name column is the full path and its POSIX cat rules match path
# substrings like "/data".
_TYPED_GROUP_KEYS = (
    "cat",
    "name",
    "pid",
    "tid",
    "fhash",
    "hhash",
    "file_path",
    "host_name",
    "io_cat",
)
_FILE_GROUP_KEYS = ("fhash", "file_path")
_TYPED_AGGS = _VIEW_METRIC_AGGS + ("min:offset", "max:offset")


def typed_group_keys(file_buckets: Tuple[str, ...] = ()) -> Tuple[str, ...]:
    """Group keys for the typed read.

    With no `file_buckets` this is the per-file grain. Given substrings,
    `file_path` folds to whichever one it contains (and the redundant `fhash`
    key drops out), which collapses per-file rows while leaving the caller's
    file-name substring rules matchable on the folded row.
    """
    if not file_buckets:
        return _TYPED_GROUP_KEYS
    args = ", ".join("'%s'" % b.replace("'", "") for b in file_buckets)
    return tuple(
        "bucket(file_path, %s)" % args if k == "file_path" else k
        for k in _TYPED_GROUP_KEYS
        if k != "fhash"
    )


class TypedFrames(TypedDict):
    """The three dfanalyzer frames from one collect_typed() pass."""

    events: pd.DataFrame
    profiles: pd.DataFrame
    system: pd.DataFrame


def _typed_event_frame(
    df: pd.DataFrame,
    time_resolution: float,
    time_origin: int = 0,
    bucket_us: int = 0,
    is_profile: bool = False,
) -> pd.DataFrame:
    """Map one collect_typed table (regular or aggregated) to the dfanalyzer
    event/profile schema, scaling durations by `time_resolution`.

    Time columns are made origin-relative to match the C++ scan: time_range is
    the bucket index (time_bucket - origin)//bucket_us; events keep their precise
    (ts-origin, te-origin) span, while profile rows align to the bucket grid
    (start = bucket - origin, end = start + bucket_us)."""
    if "count" not in df.columns:  # no aggregation tier / empty result
        return pd.DataFrame()
    out = pd.DataFrame(index=df.index)
    out["cat"] = df["cat"].str.lower() if "cat" in df else pd.Series("", index=df.index)
    out["func_name"] = df["name"] if "name" in df else pd.NA
    for c in ("pid", "tid", "io_cat"):
        if c in df.columns:
            out[c] = pd.to_numeric(df[c]).astype("int64")
    # Keep empty strings (not NA) for unresolved names, matching the C++ scan;
    # downstream string ops (posix category, proc_name split) are not NA-safe.
    # dfanalyzer's file_name is the full path (grouped as file_path).
    # file_name/file_hash are emitted even when folded away: dfanalyzer's dask
    # meta declares them, and a missing column is a metadata mismatch.
    if "file_path" in df.columns:
        out["file_name"] = df["file_path"].astype("string").fillna("")
    else:
        out["file_name"] = pd.Series("", index=df.index, dtype="string")
    if "host_name" in df.columns:
        out["host_name"] = df["host_name"].astype("string").fillna("")
    if "fhash" in df.columns:
        out["file_hash"] = df["fhash"].astype("string")
    else:
        out["file_hash"] = pd.Series("", index=df.index, dtype="string")
    if "hhash" in df.columns:
        out["host_hash"] = df["hhash"].astype("string")
    if {"pid", "tid"}.issubset(df.columns):
        host = df["host_name"].astype("string") if "host_name" in df.columns else pd.NA
        if "hhash" in df.columns:
            host = host.mask(host.isna() | (host == ""), df["hhash"].astype("string"))
        host = host.fillna("unknown").replace("", "unknown")
        out["proc_name"] = "app#" + host + "#" + df["pid"].astype(str) + "#" + df["tid"].astype(str)
    # epoch/step (or any aggregated extra key) ride through as-is.
    for c in df.columns:
        if c.startswith("arg_"):
            out[c[len("arg_") :]] = df[c]
    tr = float(time_resolution)
    out["count"] = df["count"].astype("int64")
    out["time"] = df["sum_dur"] / tr
    out["size"] = df["sum_size"].astype("int64")
    out["time_sq"] = df["sumsq_dur"] / (tr * tr)
    # float64: sum-of-squared-sizes overflows int53 for large transfers, which
    # breaks the checkpoint parquet round-trip (and it is a float metric).
    out["size_sq"] = df["sumsq_size"].astype("float64")
    out["time_min"] = df["min_dur"] / tr
    out["time_max"] = df["max_dur"] / tr
    # A group with no transfer size has no minimum or maximum (null from the
    # engine); the nullable Int64 keeps it, and the profile coercion maps it
    # to NA as dfanalyzer expects.
    out["size_min"] = df["min_size"].astype("Int64")
    out["size_max"] = df["max_size"].astype("Int64")
    # Folded-shape per-call columns the distributed HLM combines (min/max across
    # keys) instead of deriving; the derive path seeds them from the group total.
    out["time_call_min"] = out["time"]
    out["time_call_max"] = out["time"]
    out["size_call_min"] = out["size"]
    out["size_call_max"] = out["size"]
    if "min_offset" in df.columns:
        out["offset_min"] = df["min_offset"].astype("Int64")
        out["offset_max"] = df["max_offset"].astype("Int64")
    out["acc_pat"] = 0  # constant placeholder, matching the C++ scan
    out["file_nunique"] = 0  # declared by the scan meta; unused downstream
    origin = int(time_origin)
    if "time_bucket" in df.columns:
        bucket = pd.to_numeric(df["time_bucket"]).astype("int64")
        out["time_bucket"] = bucket
        out["time_range"] = ((bucket - origin) // bucket_us) if bucket_us else 0
        if is_profile:
            start = bucket - origin
            out["time_start"] = start.astype("int64")
            out["time_end"] = (start + int(bucket_us)).astype("int64")
        else:
            out["time_start"] = (df["min_ts"].astype("int64") - origin).astype("int64")
            out["time_end"] = (df["max_te"].astype("int64") - origin).astype("int64")
    else:
        out["time_range"] = 0
        out["time_start"] = (df["min_ts"].astype("int64") - origin).astype("int64")
        out["time_end"] = (df["max_te"].astype("int64") - origin).astype("int64")
    return out


def view_typed_frames(
    files: List[str],
    index_path: str,
    time_granularity: float = 1.0,
    time_resolution: float = 1e6,
    query: Optional[str] = None,
    client: "Optional[Client]" = None,
    group_keys: Optional[Tuple[str, ...]] = None,
) -> TypedFrames:
    """One-pass read of the aggregation index's three record families, each
    mapped to the dfanalyzer frame schema.

    Returns ``{"events", "profiles", "system"}`` of pandas DataFrames (events
    and profiles share the event schema; profiles additionally carry their
    extra-key dims; system carries one column per metric). Bucketed at
    ``time_granularity * time_resolution`` us. A Dask `client` delegates the read
    to DaskTraceViewer.collect_typed (shard-range fan-out across workers); the
    distribution lives there, not here.

    `group_keys` defaults to the per-file grain; pass `typed_group_keys(...)`
    to coarsen it. Coarsening is one-way, so a caller that needs per-file rows
    must not fold them away.
    """
    bucket_us = int(time_granularity * time_resolution)
    keys = group_keys or _TYPED_GROUP_KEYS
    if client is None:
        from dftracer.utils import TraceViewer

        tv: "Union[TraceViewer, DaskTraceViewer]" = TraceViewer(
            files, index_path=index_path or None
        )
    else:
        from .dask import DaskTraceViewer

        tv = DaskTraceViewer(files, index_path or "", client=client)
    if query:
        tv = tv.filter(query)
    typed = tv.group_by(*keys).time_bucket(bucket_us).agg(*_TYPED_AGGS).collect_typed()

    reg_pd, agg_pd = _capsule_to_pandas(typed["regular"]), _capsule_to_pandas(typed["aggregated"])
    # origin = global min time_bucket over events+profiles, matching the C++
    # scan's query_time_bounds().min_time_bucket (computed after any distributed
    # concat, so it is global regardless of shard fan-out).
    buckets = [
        f["time_bucket"].min()
        for f in (reg_pd, agg_pd)
        if "time_bucket" in f.columns and not f.empty
    ]
    origin = int(min(buckets)) if buckets else 0

    return {
        "events": _typed_event_frame(reg_pd, time_resolution, origin, bucket_us, is_profile=False),
        "profiles": _typed_event_frame(agg_pd, time_resolution, origin, bucket_us, is_profile=True),
        "system": _capsule_to_pandas(typed["counters"]),
    }


def _drop_ignored_files(
    df: "pd.DataFrame", ignored_file_patterns: Tuple[str, ...] = ()
) -> "pd.DataFrame":
    """Drop rows whose `file_name` contains one of `ignored_file_patterns`.

    Pass the patterns as bucket() arguments ahead of any other bucket, so a
    file that matches one folds to the pattern itself and stays matchable here.
    """
    if df.empty or not ignored_file_patterns or "file_name" not in df.columns:
        return df
    pattern = "|".join(ignored_file_patterns)
    return df[~df["file_name"].fillna("").str.contains(pattern, regex=True, na=False)]


def _frame_to_ipc(df: "pd.DataFrame") -> Optional[bytes]:
    """Serialize one pandas frame to Arrow IPC-stream bytes (None if empty)."""
    if df is None or getattr(df, "empty", True):
        return None
    table = pa.Table.from_pandas(df, preserve_index=False)
    sink = pa.BufferOutputStream()
    writer = pa.ipc.new_stream(sink, table.schema)
    writer.write_table(table)
    writer.close()
    return sink.getvalue().to_pybytes()


def _typed_read_to_ipc(
    files: List[str],
    index_path: str,
    time_granularity: float,
    time_resolution: float,
    query: Optional[str] = None,
    shard_begin: int = 0,
    shard_end: int = 0,
    group_keys: Optional[Tuple[str, ...]] = None,
    drop_file_patterns: Tuple[str, ...] = (),
    progress: Optional[Callable[[int, int], None]] = None,
) -> Dict[str, Optional[bytes]]:
    """One-pass typed read of the aggregation index mapped to the dfanalyzer
    {events, profiles, system} frames, as Arrow IPC bytes.

    Time is absolute (origin 0); build_read_frames derives the global origin and
    rebuckets time_range. This one body backs both
    the single-node budget scan and the per-worker distributed task,
    so the two read paths never diverge. `shard_end <= 0` reads all shards.
    """
    from dftracer.utils import TraceViewer

    bucket_us = int(time_granularity * time_resolution)
    tv: "TraceViewer" = TraceViewer(files, index_path=index_path or None)
    if query:
        tv = tv.filter(query)
    typed = (
        tv.group_by(*(group_keys or _TYPED_GROUP_KEYS))
        .time_bucket(bucket_us)
        .agg(*_TYPED_AGGS)
        .collect_typed(shard_begin, shard_end, progress)
    )

    events = _typed_event_frame(
        _capsule_to_pandas(typed["regular"]), time_resolution, 0, bucket_us, False
    )
    if drop_file_patterns:
        events = _drop_ignored_files(events, drop_file_patterns)
    frames = {
        "events": events,
        "profiles": _typed_event_frame(
            _capsule_to_pandas(typed["aggregated"]), time_resolution, 0, bucket_us, True
        ),
        "system": _capsule_to_pandas(typed["counters"]),
    }
    return {k: _frame_to_ipc(v) for k, v in frames.items()}


class ReadFrames(TypedDict):
    """Decoded read-path frames: traces (always present), profiles/system (or
    None), and the origins the caller needs to build a ReadTraceResult."""

    traces: "pd.DataFrame"
    profiles: Optional["pd.DataFrame"]
    system: Optional["pd.DataFrame"]
    time_origin: Optional[int]
    system_time_origin: Optional[int]


def build_read_frames(
    results: List[Dict[str, Optional[bytes]]],
    profile_output_columns: Dict[str, str],
    time_granularity: float,
    time_resolution: float,
    profile_time_granularity: float,
) -> ReadFrames:
    """Decode per-shard ``{events, profiles, system}`` IPC (from the View typed
    read) into the dfanalyzer pandas frames.

    The View emits absolute time; the returned frames are relativized to the
    global minimum ``time_bucket`` (``time_origin``), matching the old scan and
    keeping values small for the checkpoint parquet round-trip. ``traces`` is a
    typed-empty frame when there are no events, so downstream arithmetic works.
    """
    results = [r for r in results if r]

    def _decode(key: str) -> List["pd.DataFrame"]:
        out = []
        for r in results:
            b = r.get(key) if isinstance(r, dict) else None
            if b is not None:
                df = _ipc_to_pandas(b)
                if not df.empty:
                    out.append(df)
        return out

    ev, prof = _decode("events"), _decode("profiles")
    events_pd = pd.concat(ev, ignore_index=True) if ev else None
    raw_prof = pd.concat(prof, ignore_index=True) if prof else None

    buckets = [
        int(f["time_bucket"].min())
        for f in (events_pd, raw_prof)
        if f is not None and "time_bucket" in f.columns and not f.empty
    ]
    origin = min(buckets) if buckets else None
    bucket_us = int(time_granularity * time_resolution)

    def _relativize(f: "pd.DataFrame") -> None:
        if origin is None:
            return
        f["time_start"] = f["time_start"].astype("int64") - origin
        f["time_end"] = f["time_end"].astype("int64") - origin
        if bucket_us and "time_bucket" in f.columns:
            f["time_range"] = ((f["time_bucket"].astype("int64") - origin) // bucket_us).astype(
                "int64"
            )

    if events_pd is not None:
        _relativize(events_pd)
        traces = events_pd
    else:
        traces = pd.DataFrame({c: pd.Series(dtype=dt) for c, dt in profile_output_columns.items()})

    profiles: Optional["pd.DataFrame"] = None
    if raw_prof is not None:
        _relativize(raw_prof)
        window = int(profile_time_granularity * time_resolution)
        raw_prof = raw_prof.drop(columns=["time_end"], errors="ignore")
        profiles = _coerce_profile_dtypes(raw_prof, profile_output_columns, profile_window=window)

    sys = _decode("system")
    system_pd: Optional["pd.DataFrame"] = None
    sys_origin: Optional[int] = None
    if sys:
        system_pd = pd.concat(sys, ignore_index=True)
        system_pd["ts"] = system_pd["time_bucket"].astype("int64")
        sys_origin = int(system_pd["ts"].min())

    return {
        "traces": traces,
        "profiles": profiles,
        "system": system_pd,
        "time_origin": origin,
        "system_time_origin": sys_origin,
    }


def resolve_trace_inputs(
    trace_path: str,
    trace_groups: Optional[List[str]],
) -> Tuple[str, Optional[List[str]]]:
    """Resolve a trace path into (directory, files) for the Indexer.

    If trace_path is a directory containing manifest.json and trace_groups is
    set, glob only the subdirs for the requested groups. Otherwise return
    (directory, None) or ("", files).
    """
    if not os.path.isdir(trace_path):
        matched = glob.glob(trace_path) if "*" in trace_path else [trace_path]
        files = [f for f in matched if f.endswith(_TRACE_SUFFIXES)]
        return "", files

    manifest_path = os.path.join(trace_path, "manifest.json")
    if not os.path.isfile(manifest_path):
        if trace_groups:
            raise FileNotFoundError(
                f"trace_groups={trace_groups} requested but no manifest.json at "
                f"{manifest_path}. Provide a manifest.json, or unset "
                "trace_groups."
            )
        return trace_path, None

    with open(manifest_path, "r") as f:
        manifest = json.load(f)
    group_map = manifest.get("groups") or {}

    selected = trace_groups if trace_groups else sorted(group_map.keys())
    missing = [g for g in selected if g not in group_map]
    if missing:
        raise KeyError(
            f"trace_groups {missing} not found in manifest at {manifest_path}; "
            f"available groups: {sorted(group_map.keys())}"
        )

    files: List[str] = []
    for g in selected:
        subdir = os.path.join(trace_path, group_map[g])
        for suffix in _TRACE_SUFFIXES:
            files.extend(glob.glob(os.path.join(subdir, "*" + suffix)))
    return "", files


def _partial_agg_columns(full_cols, sum_cols, min_cols, max_cols, set_cols_items, dtype_of):
    """Metric column -> dtype map for a partial view aggregation, in the exact
    order Arrow's group_by+aggregate emits. `dtype_of(col)` resolves a metric
    column's dtype from the caller's source (a live frame or the dask meta)."""
    cols = {}
    for c in full_cols:
        cols[f"{c}_sum"] = dtype_of(c)
        cols[f"{c}_count"] = pd.ArrowDtype(pa.int64())
        cols[f"{c}_min"] = dtype_of(c)
        cols[f"{c}_max"] = dtype_of(c)
        cols[f"{c}_sumsq"] = pd.ArrowDtype(pa.float64())
    for c in sum_cols:
        cols[f"{c}_sum"] = dtype_of(c)
    for c in min_cols:
        cols[f"{c}_min"] = dtype_of(c)
    for c in max_cols:
        cols[f"{c}_max"] = dtype_of(c)
    for c, _ in set_cols_items:
        cols[f"{c}_unique"] = "object"
    return cols


def partial_arrow_view_groupby(
    df,
    view_type,
    full_cols,
    sum_cols,
    min_cols,
    max_cols,
    set_cols_items,
    flatten_fn,
):
    """Per-partition Arrow groupby emitting mergeable partial aggregates.

    `flatten_fn` flattens a grouped set-column series when its aggregation
    object exposes no `chunk` method.
    """
    view_type_in_index = (isinstance(df.index, pd.MultiIndex) and view_type in df.index.names) or (
        df.index.name == view_type
    )
    work = df.reset_index() if view_type_in_index else df
    if work.empty:
        # Derive dtypes from the input columns so an empty partition matches
        # the meta declared by the caller (which uses the same rule).
        def _col_dtype(col, default=pd.ArrowDtype(pa.float64())):
            if col in work.columns:
                return work[col].dtype
            return default

        cols = _partial_agg_columns(
            full_cols, sum_cols, min_cols, max_cols, set_cols_items, _col_dtype
        )
        out = pd.DataFrame({name: pd.Series(dtype=dt) for name, dt in cols.items()})
        out.index = pd.Index(
            [],
            name=view_type,
            dtype=_col_dtype(view_type, default=pd.ArrowDtype(pa.int64())),
        )
        return out

    arrow_keep = [view_type]
    for lst in (full_cols, sum_cols, min_cols, max_cols):
        for c in lst:
            if c in work.columns and c not in arrow_keep:
                arrow_keep.append(c)
    tbl = pa.Table.from_pandas(work[arrow_keep], preserve_index=False)

    agg_specs = []
    for c in full_cols:
        if c not in tbl.schema.names:
            continue
        col_arr = pc.cast(tbl.column(c), pa.float64())
        tbl = tbl.append_column(f"{c}__sq", pc.multiply(col_arr, col_arr))  # ty: ignore[unresolved-attribute]
        agg_specs += [
            (c, "sum"),
            (c, "count"),
            (c, "min"),
            (c, "max"),
            (f"{c}__sq", "sum"),
        ]
    for c in sum_cols:
        if c in tbl.schema.names:
            agg_specs.append((c, "sum"))
    for c in min_cols:
        if c in tbl.schema.names:
            agg_specs.append((c, "min"))
    for c in max_cols:
        if c in tbl.schema.names:
            agg_specs.append((c, "max"))

    if agg_specs:
        result = tbl.group_by([view_type]).aggregate(agg_specs)
        out = result.to_pandas(types_mapper=pd.ArrowDtype)
        rename = {f"{c}__sq_sum": f"{c}_sumsq" for c in full_cols}
        if rename:
            out = out.rename(columns=rename)
        out = out.set_index(view_type)
    else:
        uniq = work[view_type].drop_duplicates().reset_index(drop=True)
        out = pd.DataFrame(index=pd.Index(uniq, name=view_type))

    for col, agg in set_cols_items:
        if col not in work.columns:
            continue
        sgb = work.groupby(view_type)[col]
        chunk_fn = getattr(agg, "chunk", None)
        partial = chunk_fn(sgb) if chunk_fn is not None else sgb.apply(flatten_fn)
        partial.name = f"{col}_unique"
        out = out.join(partial, how="left")
    return out


def finalize_view_partials(df, full_cols):
    """Compute mean/std per view_type row from merged partials; drop helper cols."""
    if df.empty:
        return df
    out = df.copy()
    drop = []
    for c in full_cols:
        sum_c = f"{c}_sum"
        count_c = f"{c}_count"
        sq_c = f"{c}_sumsq"
        if sum_c not in out.columns or count_c not in out.columns:
            continue
        s = out[sum_c].astype("float64")
        n = out[count_c].astype("float64")
        mean_v = s / n
        out[f"{c}_mean"] = mean_v.astype(pd.ArrowDtype(pa.float64()))
        if sq_c in out.columns:
            sq = out[sq_c].astype("float64")
            # sample variance is undefined for n <= 1 -> std is NaN, matching
            # pandas .std(ddof=1); avoids a divide-by-zero on (n - 1).
            with np.errstate(invalid="ignore", divide="ignore"):
                var_v = (sq - (s * s) / n) / (n - 1)
            var_v = var_v.where(n > 1, np.nan)
            var_v = var_v.where(var_v.isna() | (var_v >= 0), 0)
            out[f"{c}_std"] = np.sqrt(var_v).astype(pd.ArrowDtype(pa.float64()))
            drop.append(sq_c)
        drop.append(count_c)
    if drop:
        out = out.drop(columns=drop)
    return out


def build_partial_meta(records, view_type, full_cols, sum_cols, min_cols, max_cols, set_cols_items):
    """Dask meta for the output of `partial_arrow_view_groupby`."""
    in_meta = records._meta

    def _dtype_of(col, default=pd.ArrowDtype(pa.float64())):
        if col in in_meta.columns:
            return in_meta[col].dtype
        if isinstance(in_meta.index, pd.MultiIndex) and col in in_meta.index.names:
            return in_meta.index.get_level_values(col).dtype
        return default

    cols = _partial_agg_columns(full_cols, sum_cols, min_cols, max_cols, set_cols_items, _dtype_of)
    meta = pd.DataFrame({name: pd.Series(dtype=dt) for name, dt in cols.items()})
    idx_dtype = _dtype_of(view_type, default=pd.ArrowDtype(pa.int64()))
    meta.index = pd.Index([], name=view_type, dtype=idx_dtype)
    return meta


def build_final_meta(merged, full_cols):
    """Dask meta for the output of `finalize_view_partials`."""
    cols = {}
    for c in merged.columns:
        if c.endswith("_count") and c[: -len("_count")] in full_cols:
            continue
        if c.endswith("_sumsq") and c[: -len("_sumsq")] in full_cols:
            continue
        cols[c] = merged._meta[c].dtype
    for c in full_cols:
        cols[f"{c}_mean"] = pd.ArrowDtype(pa.float64())
        cols[f"{c}_std"] = pd.ArrowDtype(pa.float64())
    meta = pd.DataFrame({name: pd.Series(dtype=dt) for name, dt in cols.items()})
    meta.index = pd.Index([], name=merged._meta.index.name, dtype=merged._meta.index.dtype)
    return meta


def normalize_arrow_dtypes(df):
    """Demote Arrow-backed category columns to object for downstream pandas ops."""
    for col in df.select_dtypes(include=["category"]).columns:
        df[col] = df[col].astype("object")
    return df


def count_index_files(
    files: List[str], index_path: str, ignored_patterns: Tuple[str, ...] = ()
) -> int:
    """Distinct data files in the index, skipping paths containing any of
    `ignored_patterns`.

    Counts from the index's hash table rather than the read, so it stays exact
    when the read is folded to a grain with no per-file rows. Callers that
    filter files out of the analysis must pass the same patterns here, or the
    count reports files the analysis never used.
    """
    table = _open_readonly_indexer(files, index_path).get_hash_table("file")
    if not ignored_patterns:
        return len(table)
    return sum(1 for name in table.values() if not any(p in name for p in ignored_patterns))


def index_path_for(trace_path: str) -> str:
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


def coerce_arrow_numerics_to_pandas_native(df):
    """Map pd.ArrowDtype int/float columns to pandas Int64/Float64."""
    if df.empty:
        return df
    for c in df.columns:
        dt = df[c].dtype
        if isinstance(dt, pd.ArrowDtype):
            pa_type = dt.pyarrow_dtype
            if pa.types.is_floating(pa_type):
                df[c] = df[c].astype("Float64")
            elif pa.types.is_integer(pa_type):
                df[c] = df[c].astype("Int64")
    return df


def _coerce_profile_dtypes(df, output_columns, profile_window=None):
    """Normalize C++ aggregator profile output to the `output_columns` schema.

    `output_columns` maps column name -> pandas dtype. When `profile_window` is
    given, `time_end` is derived as `time_start + profile_window`.
    """
    if df.empty:
        return df
    df = df.copy()
    for col, dtype in output_columns.items():
        if col not in df.columns:
            df[col] = pd.Series(pd.NA, index=df.index, dtype=dtype)
        elif dtype == "string":
            df[col] = df[col].astype("string").replace("", pd.NA)
        else:
            df[col] = df[col].astype(dtype)
    # A profile with no transfer size aggregates to 0; dfanalyzer treats absent
    # size as NA (a 0 would skew bandwidth), matching the old scan's null.
    for col in ("size", "size_min", "size_max"):
        if col in df.columns:
            df[col] = df[col].replace(0, pd.NA)
    if profile_window is not None:
        df["time_end"] = df["time_start"] + int(profile_window)
    return df


def build_index_distributed(
    directory="",
    files=None,
    index_path="",
    local_staging="",
    shared_staging="",
    client=None,
    aggregation=None,
    progress: Optional[Callable[[int, int, str], None]] = None,
):
    """Build the dftracer index across a Dask cluster.

    Workers build per-CF SSTs under `local_staging`, move them to
    `shared_staging` for the coordinator to bulk-ingest. When `aggregation` is
    given, the AGGREGATION + SYSTEM_METRICS tiers are filled in parallel.

    If `client` is None the active Dask client is looked up; if none exists
    (or `dask.distributed` is not installed), tasks run inline serially.

    `progress`, if given, is called with (files_done, total_files) as the
    parse fans out across workers.
    """
    if client is None and get_client is not None:
        try:
            client = get_client()
        except ValueError:
            client = None
    if client is not None:
        register_auto_thread_plugin()
    return distributed_index(
        directory=directory,
        files=files,
        index_path=index_path,
        local_staging=local_staging,
        shared_staging=shared_staging,
        client=client,
        aggregation_config=aggregation,
        progress=progress,
    )


def ensure_index(
    trace_path,
    trace_groups,
    time_interval_ms,
    client=None,
    progress: Optional[Callable[[int, int, str], None]] = None,
    group_by_file: bool = True,
):
    """Build (or refresh) the dftracer index for `trace_path` via Dask.

    Idempotent: dftracer-utils skips files whose tiers already exist, so repeat
    calls on the same path are cheap no-ops. With no active Dask client (or no
    `dask.distributed`), the build runs inline serially.

    `group_by_file=False` keeps the file hash out of the aggregation key, which
    is far smaller on traces touching many files but makes per-file queries
    impossible without re-reading the trace. It is part of the index identity,
    so changing it invalidates an existing aggregation.
    """
    if client is None and get_client is not None:
        try:
            client = get_client()
        except ValueError:
            client = None
    directory, files = resolve_trace_inputs(trace_path, trace_groups)
    if not directory and not files:
        return
    index_path = index_path_for(trace_path)
    local_staging = (
        resolve_local_staging(client) if client is not None else os.path.dirname(index_path)
    )
    build_index_distributed(
        directory=directory,
        files=files,
        index_path=index_path,
        local_staging=local_staging,
        shared_staging=os.path.dirname(index_path),
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
# post-agg; file_name is dfanalyzer's full path, so file_path.
_HLM_DIM_TO_KEY = {
    "file_name": "file_path",
    "host_name": "host_name",
    "cat": "cat",
    "func_name": "name",
    "io_cat": "io_cat",
    "pid": "pid",
    "tid": "tid",
}
_HLM_METRIC_AGGS = ("count", "sum:dur", "sum:size", "sumsq:dur", "sumsq:size")


@dataclass(frozen=True)
class HLMConfig:
    """The dfanalyzer HLM domain rules, supplied by the analyzer preset."""

    posix_cat_rules: Tuple[Tuple[str, str], ...] = ()
    ignored_file_patterns: Tuple[str, ...] = ()
    ignored_func_names: Tuple[str, ...] = ()
    ignored_func_patterns: Tuple[str, ...] = ()
    time_granularity: float = 1.0
    time_resolution: float = 1e6

    def ignored_func_predicate(self) -> Optional[str]:
        clauses: List[str] = []
        if self.ignored_func_names:
            names = ", ".join(f'"{n.replace(chr(34), "")}"' for n in self.ignored_func_names)
            clauses.append(f"name not in [{names}]")
        if self.ignored_func_patterns:
            pat = "|".join(self.ignored_func_patterns).replace('"', "")
            clauses.append(f'name !~ "{pat}"')
        return " and ".join(clauses) if clauses else None


class DFAnalyzerAggregatedTraceViewer(DaskAggregatedTraceViewer):
    """The dfanalyzer HLM expressed as one View aggregation.

    A DaskAggregatedTraceViewer subclass holding the HLM domain rules (ignored
    funcs/files, posix cat-suffix), so the read+aggregate is one View chain.
    """

    def __init__(
        self,
        files,
        index_dir: str = "",
        *,
        client=None,
        files_per_task: int = 1,
        _plan=None,
        hlm_config: Optional[HLMConfig] = None,
    ):
        super().__init__(
            files, index_dir, client=client, files_per_task=files_per_task, _plan=_plan
        )
        self._hlm_config = hlm_config or HLMConfig()

    # Both clone paths thread hlm_config: the base _agg_clone hardcodes
    # DaskAggregatedTraceViewer, so group_by()/agg() would otherwise drop it.
    def _clone(self, plan):
        return type(self)(
            self._files,
            self._index_dir,
            client=self._client,
            files_per_task=self._files_per_task,
            _plan=plan,
            hlm_config=self._hlm_config,
        )

    def _agg_clone(self, plan):
        return type(self)(
            self._files,
            self._index_dir,
            client=self._client,
            files_per_task=self._files_per_task,
            _plan=plan,
            hlm_config=self._hlm_config,
        )

    def hlm(self, view_types: Sequence[str]) -> "pd.DataFrame":
        df, vts = self._hlm_frame(view_types, "regular")
        return self._to_hlm(df, vts)

    def profile_hlm(self, view_types: Sequence[str]) -> "pd.DataFrame":
        df, vts = self._hlm_frame(view_types, "aggregated")
        return self._to_hlm(df, vts)

    def _hlm_frame(self, view_types, family):
        cfg = self._hlm_config
        vts = list(view_types)
        temporal = "time_range" in vts
        bucket_us = int(cfg.time_granularity * cfg.time_resolution)

        keys = [_HLM_DIM_TO_KEY[v] for v in vts if v in _HLM_DIM_TO_KEY]
        if "proc_name" in vts:
            keys += ["pid", "tid", "host_name", "hhash"]
        keys += ["cat", "io_cat", "name"]
        has_path = "file_path" in keys
        if not has_path:
            # Ignore patterns first so an ignored file folds to its pattern (dropped
            # below); cat rules follow so a kept file folds to its suffix substring.
            subs = list(cfg.ignored_file_patterns) + [p for p, _ in cfg.posix_cat_rules]
            if subs:
                args = ", ".join(f"'{s.replace(chr(39), '')}'" for s in subs)
                keys.append(f"bucket(file_path, {args})")

        aggs: List[str] = list(_HLM_METRIC_AGGS)
        if temporal:
            aggs.append("min:ts")

        v = self.phase("events") if family == "regular" else self
        pred = cfg.ignored_func_predicate()
        if pred:
            v = v.filter(pred)
        agg_view = v.group_by(*keys)
        if temporal:
            agg_view = agg_view.time_bucket(bucket_us)
        if family == "regular":
            tbl = agg_view.agg(*aggs).collect()
        else:
            tbl = agg_view.agg(*aggs).collect_typed().get(family)

        df = _capsule_to_pandas(tbl)
        # File-ignore is post-agg: file_path is null to a scan-time predicate
        # (resolved only after aggregation). An ignored file folds to its own
        # pattern, so this drop works whether file_path is the full path or a fold.
        if cfg.ignored_file_patterns and not df.empty and "file_path" in df.columns:
            pat = "|".join(cfg.ignored_file_patterns)
            keep = ~df["file_path"].fillna("").astype("string").str.contains(
                pat, regex=True, na=False
            )
            df = df[keep]
        return df, vts

    _METRIC_COLS = (
        "time",
        "count",
        "size",
        "time_sq",
        "size_sq",
        "time_call_min",
        "time_call_max",
        "size_call_min",
        "size_call_max",
    )
    _INT_COLS = frozenset({"count", "size", "size_call_min", "size_call_max"})

    def _to_hlm(self, df, vts) -> "pd.DataFrame":
        cfg = self._hlm_config
        idx_names = list(vts) + ["cat", "io_cat", "acc_pat", "func_name"]
        if df.empty or "count" not in df.columns:
            empty = pd.DataFrame(
                {
                    c: pd.Series(dtype="Int64" if c in self._INT_COLS else "Float64")
                    for c in self._METRIC_COLS
                }
            )
            empty.index = pd.MultiIndex.from_arrays([[]] * len(idx_names), names=idx_names)
            return empty
        tr = float(cfg.time_resolution)
        out = pd.DataFrame(index=df.index)

        for v in vts:
            if v == "proc_name":
                out[v] = self._compose_proc_name(df)
            elif v == "time_range":
                # time_range is the bucket index of min:ts relative to the global minimum.
                bucket_us = int(cfg.time_granularity * cfg.time_resolution)
                min_ts = df["min_ts"].astype("int64")
                out[v] = ((min_ts - min_ts.min()) // bucket_us).astype("int64")
            else:
                out[v] = df[_HLM_DIM_TO_KEY[v]]

        path = (
            df["file_path"]
            if "file_path" in df.columns
            else pd.Series([""] * len(df), index=df.index)
        )
        out["cat"] = self._apply_cat_suffix(df["cat"].astype("string"), path.astype("string"))
        out["io_cat"] = df["io_cat"].astype("int64")
        out["func_name"] = df["name"]
        out["acc_pat"] = 0

        # 0 -> NA (a 0-size open must not skew bandwidth); nullable Int64/Float64.
        # The _call_min/max columns seed from the group total, not per-event extremes.
        metrics = pd.DataFrame(index=df.index)
        metrics["time"] = df["sum_dur"] / tr
        metrics["count"] = df["count"]
        metrics["size"] = df["sum_size"]
        metrics["time_sq"] = df["sumsq_dur"] / (tr * tr)
        metrics["size_sq"] = df["sumsq_size"]
        metrics["time_call_min"] = metrics["time"]
        metrics["time_call_max"] = metrics["time"]
        metrics["size_call_min"] = metrics["size"]
        metrics["size_call_max"] = metrics["size"]
        metrics = metrics.replace(0, pd.NA)
        for c in metrics.columns:
            metrics[c] = metrics[c].astype("Int64" if c in self._INT_COLS else "Float64")
            out[c] = metrics[c]

        return out.set_index(idx_names)

    @staticmethod
    def _compose_proc_name(df: "pd.DataFrame") -> "pd.Series":
        if "host_name" in df.columns:
            host = df["host_name"].astype("string")
        else:
            host = pd.Series(pd.NA, index=df.index, dtype="string")
        if "hhash" in df.columns:
            host = host.mask(host.isna() | (host == ""), df["hhash"].astype("string"))
        host = host.fillna("unknown").replace("", "unknown")
        return "app#" + host + "#" + df["pid"].astype(str) + "#" + df["tid"].astype(str)

    def _apply_cat_suffix(self, cat: "pd.Series", path: "pd.Series") -> "pd.Series":
        rules = self._hlm_config.posix_cat_rules
        if not rules:
            return cat
        base = cat.str.contains("posix|stdio", na=False) & path.notna()
        out = cat.copy()
        p = path.fillna("")
        for sub, suffix in rules:
            out = out.mask(base & p.str.contains(sub, regex=False, na=False), out + suffix)
        return out
