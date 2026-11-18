# SPDX-License-Identifier: MIT
"""The pandas frames dfanalyzer reads from a view."""

from __future__ import annotations

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

import pandas as pd
import pyarrow as pa

from ..arrow import decode_dictionary_columns, ipc_to_table
from ..dask.viewer import DaskAggregatedTraceViewer, DaskTraceViewer
from ..trace.viewer import TraceViewer

if TYPE_CHECKING:
    from dask.distributed import Client  # ty: ignore[import-not-found]


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
        if "host_name" in df.columns:
            host = df["host_name"].astype("string")
        else:
            host = pd.Series(pd.NA, index=df.index, dtype="string")
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
    index_dir: str,
    time_granularity: float = 1.0,
    time_resolution: float = 1e6,
    duql: Optional[str] = None,
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
        tv: "Union[TraceViewer, DaskTraceViewer]" = TraceViewer(files, index_dir=index_dir or None)
    else:
        tv = DaskTraceViewer(files, index_dir or "", client=client)
    if duql:
        tv = tv.filter(duql)
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


def typed_read_to_ipc(
    files: List[str],
    index_dir: str,
    time_granularity: float,
    time_resolution: float,
    duql: Optional[str] = None,
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
    bucket_us = int(time_granularity * time_resolution)
    tv: "TraceViewer" = TraceViewer(files, index_dir=index_dir or None)
    if duql:
        tv = tv.filter(duql)
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
