"""Helpers bridging the C++ aggregation index to dfanalyzer.

These were previously vendored inside dfanalyzer; they belong here since they
only depend on the Indexer and Arrow plumbing.
"""

from __future__ import annotations

import glob
import json
import os
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.compute as pc

from .dask import distributed_index, register_auto_thread_plugin, resolve_local_staging
from .indexer import AggregationConfig, Indexer

try:
    from dask.distributed import get_client
except ImportError:
    get_client = None  # ty: ignore[invalid-assignment]

__all__ = [
    "batches_to_ipc",
    "build_final_meta",
    "build_index_distributed",
    "build_partial_meta",
    "coerce_arrow_numerics_to_pandas_native",
    "coerce_profile_dtypes",
    "distributed_hlm",
    "ensure_index",
    "finalize_view_partials",
    "index_path_for",
    "ipc_to_pandas",
    "make_empty_hlm",
    "normalize_arrow_dtypes",
    "partial_arrow_view_groupby",
    "resolve_trace_inputs",
    "scan_to_ipc",
    "worker_hlm_partial",
]

_TRACE_SUFFIXES = (".pfw", ".pfw.gz")


def ipc_to_pandas(ipc_bytes: bytes):
    """Decode Arrow IPC bytes to pandas, casting dictionary columns to string."""
    reader = pa.ipc.open_stream(pa.BufferReader(ipc_bytes))
    table = reader.read_all()
    for i, field in enumerate(table.schema):
        if pa.types.is_dictionary(field.type):
            table = table.set_column(i, field.name, table.column(i).cast(pa.string()))
    return table.to_pandas()


def batches_to_ipc(batches_by_type: Dict[str, Any]) -> Dict[str, Optional[bytes]]:
    """Convert {type: [capsule, ...]} from the C extension into {type: IPC bytes}."""
    result: Dict[str, Optional[bytes]] = {}
    for data_type in ("events", "profiles", "system"):
        batches = [pa.record_batch(b) for b in batches_by_type.get(data_type, [])]
        if batches:
            sink = pa.BufferOutputStream()
            writer = pa.ipc.new_stream(sink, batches[0].schema)
            for batch in batches:
                writer.write_batch(batch)
            writer.close()
            result[data_type] = sink.getvalue().to_pybytes()
        else:
            result[data_type] = None
    return result


def scan_to_ipc(files, index_path, time_granularity, time_resolution, query):
    """Dask worker task: full-scan the aggregation CF for `files`, return IPC bytes."""
    indexer = Indexer(
        files=files,
        index_dir=os.path.dirname(index_path) if index_path else "",
        require_checkpoint=False,
        require_bloom=False,
        require_manifest=False,
        require_aggregation=False,
        force_rebuild=False,
    )
    all_batches = indexer.iter_arrow_dfanalyzer_all(
        time_granularity=time_granularity,
        time_resolution=time_resolution,
        query=query,
    )
    return batches_to_ipc(all_batches)


def resolve_trace_inputs(
    trace_path: str,
    trace_groups: Optional[List[str]],
) -> Tuple[str, Optional[List[str]]]:
    """Resolve a trace path into (directory, files) for the Indexer.

    If trace_path is a directory containing manifest.json (dftracer_organize
    output) and trace_groups is set, glob only the subdirs for the requested
    groups. Otherwise return (directory, None) or ("", files).
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
                f"{manifest_path}. Run dftracer_organize to produce it, or unset "
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
        files.extend(glob.glob(os.path.join(subdir, "*.pfw.gz")))
        files.extend(glob.glob(os.path.join(subdir, "*.pfw")))
    return "", files


def make_empty_hlm(hlm_groupby, hlm_agg, bin_cols, int_index_cols, float_metric_cols):
    """Empty DataFrame matching the HLM meta schema.

    `int_index_cols` are groupby columns typed as Int64 (others are string);
    `float_metric_cols` are metric columns typed as Float64 (others Int64).
    """
    bin_set = set(bin_cols)
    int_index_cols = set(int_index_cols)
    float_metric_cols = set(float_metric_cols)
    data_cols = {}
    for col in hlm_agg:
        if col in hlm_groupby or col in bin_set:
            continue
        dtype = "Float64" if col in float_metric_cols else "Int64"
        data_cols[col] = pd.Series(dtype=dtype)
    meta = pd.DataFrame(data_cols)
    idx_arrays = []
    for col in hlm_groupby:
        dtype = "Int64" if col in int_index_cols else "string"
        idx_arrays.append(pd.array([], dtype=dtype))
    if idx_arrays:
        meta.index = pd.MultiIndex.from_arrays(idx_arrays, names=list(hlm_groupby))
    return meta


def worker_hlm_partial(
    ipc_result, data_type, hlm_groupby, hlm_agg, bin_cols, int_index_cols, float_metric_cols
):
    """Per-worker partial HLM from already-resident IPC bytes.

    Workers own disjoint PID sets and proc_name is always in hlm_groupby, so
    per-worker partials have disjoint keys and need no cross-worker merge.
    """
    empty = lambda: make_empty_hlm(  # noqa: E731
        hlm_groupby, hlm_agg, bin_cols, int_index_cols, float_metric_cols
    )

    ipc_bytes = ipc_result[data_type] if isinstance(ipc_result, dict) else None
    if ipc_bytes is None:
        return empty()
    reader = pa.ipc.open_stream(pa.BufferReader(ipc_bytes))
    table = reader.read_all()
    if table.num_rows == 0:
        return empty()

    for i, field in enumerate(table.schema):
        if pa.types.is_dictionary(field.type):
            table = table.set_column(i, field.name, table.column(i).cast(pa.string()))

    time_col = table.column("time")
    size_col = table.column("size")
    table = table.append_column("time_sq", pc.multiply(time_col, time_col))  # ty: ignore[unresolved-attribute]
    size_filled = pc.if_else(pc.is_null(size_col), pa.scalar(0, pa.int64()), size_col)  # ty: ignore[unresolved-attribute]
    table = table.append_column("size_sq", pc.multiply(size_filled, size_filled))  # ty: ignore[unresolved-attribute]
    table = table.append_column("time_call_min", time_col)
    table = table.append_column("time_call_max", time_col)
    table = table.append_column("size_call_min", size_col)
    table = table.append_column("size_call_max", size_col)

    available_groupby = [c for c in hlm_groupby if c in table.column_names]
    if not available_groupby:
        return empty()

    agg_specs = []
    for col, agg_fn in hlm_agg.items():
        if col in table.column_names and agg_fn in ("sum", "min", "max"):
            agg_specs.append((col, agg_fn))

    result = table.group_by(available_groupby).aggregate(agg_specs)

    rename = {f"{col}_{agg_fn}": col for col, agg_fn in agg_specs}
    result = result.rename_columns([rename.get(c, c) for c in result.column_names])

    cat_idx = result.schema.get_field_index("cat")
    if cat_idx >= 0:
        cat_col = result.column(cat_idx)
        if pa.types.is_string(cat_col.type) or pa.types.is_large_string(cat_col.type):
            result = result.set_column(cat_idx, "cat", pc.utf8_lower(cat_col))  # ty: ignore[unresolved-attribute]

    groupby_set = set(available_groupby)
    for i, field in enumerate(result.schema):
        if field.name in groupby_set:
            continue
        t_ = field.type
        if pa.types.is_integer(t_) or pa.types.is_floating(t_):
            col = result.column(i)
            zero = pa.scalar(0 if pa.types.is_integer(t_) else 0.0, t_)
            null = pa.scalar(None, t_)
            result = result.set_column(i, field.name, pc.if_else(pc.equal(col, zero), null, col))  # ty: ignore[unresolved-attribute]

    # Materialize to pandas with native nullable dtypes. ArrowDtype numeric
    # columns trip a pandas masked-arithmetic bug in downstream metrics.py.
    pdf = result.to_pandas(types_mapper=pd.ArrowDtype)
    for c in pdf.columns:
        if c in available_groupby:
            continue
        dt = pdf[c].dtype
        if isinstance(dt, pd.ArrowDtype):
            pa_type = dt.pyarrow_dtype
            if pa.types.is_floating(pa_type):
                pdf[c] = pdf[c].astype("Float64")
            elif pa.types.is_integer(pa_type):
                pdf[c] = pdf[c].astype("Int64")
    return pdf.set_index(available_groupby)


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

        empty_cols = {}
        for c in full_cols:
            empty_cols[f"{c}_sum"] = pd.Series(dtype=_col_dtype(c))
            empty_cols[f"{c}_count"] = pd.Series(dtype=pd.ArrowDtype(pa.int64()))
            empty_cols[f"{c}_min"] = pd.Series(dtype=_col_dtype(c))
            empty_cols[f"{c}_max"] = pd.Series(dtype=_col_dtype(c))
            empty_cols[f"{c}_sumsq"] = pd.Series(dtype=pd.ArrowDtype(pa.float64()))
        for c in sum_cols:
            empty_cols[f"{c}_sum"] = pd.Series(dtype=_col_dtype(c))
        for c in min_cols:
            empty_cols[f"{c}_min"] = pd.Series(dtype=_col_dtype(c))
        for c in max_cols:
            empty_cols[f"{c}_max"] = pd.Series(dtype=_col_dtype(c))
        for c, _ in set_cols_items:
            empty_cols[f"{c}_unique"] = pd.Series(dtype="object")
        out = pd.DataFrame(empty_cols)
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

    # Column order must exactly match what Arrow's group_by+aggregate emits.
    cols = {}
    for c in full_cols:
        cols[f"{c}_sum"] = _dtype_of(c)
        cols[f"{c}_count"] = pd.ArrowDtype(pa.int64())
        cols[f"{c}_min"] = _dtype_of(c)
        cols[f"{c}_max"] = _dtype_of(c)
        cols[f"{c}_sumsq"] = pd.ArrowDtype(pa.float64())
    for c in sum_cols:
        cols[f"{c}_sum"] = _dtype_of(c)
    for c in min_cols:
        cols[f"{c}_min"] = _dtype_of(c)
    for c in max_cols:
        cols[f"{c}_max"] = _dtype_of(c)
    for c, _ in set_cols_items:
        cols[f"{c}_unique"] = "object"

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


def coerce_profile_dtypes(df, output_columns, profile_window=None):
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
):
    """Build the dftracer index across a Dask cluster.

    Workers build per-CF SSTs under `local_staging`, move them to
    `shared_staging` for the coordinator to bulk-ingest. When `aggregation` is
    given, the AGGREGATION + SYSTEM_METRICS tiers are filled in parallel.

    If `client` is None the active Dask client is looked up; if none exists
    (or `dask.distributed` is not installed), tasks run inline serially.
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
    )


def ensure_index(trace_path, trace_groups, time_interval_ms, client=None):
    """Build (or refresh) the dftracer index for `trace_path` via Dask.

    Idempotent: dftracer-utils skips files whose tiers already exist, so repeat
    calls on the same path are cheap no-ops. With no active Dask client (or no
    `dask.distributed`), the build runs inline serially.
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
        aggregation=AggregationConfig(time_interval_ms=time_interval_ms),
    )


def distributed_hlm(
    data_type,
    view_types,
    traces,
    worker_ipc_futures,
    worker_scan_args,
    dask_client,
    hlm_agg_base,
    hlm_extra_cols,
    int_index_cols,
    float_metric_cols,
):
    """Distributed high-level-metrics aggregation over per-worker IPC bytes.

    Submits one `worker_hlm_partial` task per worker, pinned to the worker that
    already holds the IPC bytes, and assembles a Dask DataFrame. Returns None
    when no worker IPC futures exist.
    """
    import dask
    import dask.dataframe as dd

    if not worker_ipc_futures:
        return None

    hlm_groupby = list(dict.fromkeys(list(view_types) + list(hlm_extra_cols)))
    bin_cols = [col for col in traces.columns if "_bin_" in col]

    hlm_agg = dict(hlm_agg_base)
    hlm_agg.update({col: "sum" for col in bin_cols})
    hlm_agg["time_sq"] = "sum"
    hlm_agg["size_sq"] = "sum"
    hlm_agg["time_call_min"] = "min"
    hlm_agg["time_call_max"] = "max"
    hlm_agg["size_call_min"] = "min"
    hlm_agg["size_call_max"] = "max"

    worker_addrs = [a for (a, _, _) in (worker_scan_args or [])]
    if len(worker_addrs) < len(worker_ipc_futures):
        worker_addrs += [None] * (len(worker_ipc_futures) - len(worker_addrs))

    partial_futures = []
    for addr, ipc_future in zip(worker_addrs, worker_ipc_futures):
        fut = dask_client.submit(
            worker_hlm_partial,
            ipc_future,
            data_type,
            list(hlm_groupby),
            dict(hlm_agg),
            list(bin_cols),
            int_index_cols,
            float_metric_cols,
            workers=[addr] if addr else None,
            pure=False,
        )
        partial_futures.append(fut)

    partial_delayed = [dask.delayed(f) for f in partial_futures]
    meta = make_empty_hlm(hlm_groupby, hlm_agg, bin_cols, int_index_cols, float_metric_cols)
    return dd.from_delayed(partial_delayed, meta=meta)
