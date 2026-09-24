#!/usr/bin/env python3
"""Parity checks for the View-based dfanalyzer read path.

_typed_read_to_ipc and view_typed_frames are both the View's typed read mapped to
the dfanalyzer schema (the former is its Arrow-IPC form); these tests pin the
schema mapping, the tier-vs-raw-scan equivalence, and name/percentile resolution.
"""

import gzip

import pytest

import dftracer.utils as dftu_utils

try:
    import dask  # noqa: F401

    DASK_AVAILABLE = True
except ImportError:
    DASK_AVAILABLE = False


def _make_trace(path):
    with gzip.open(path, "wt") as f:
        for i in range(300):
            name, dur, ret = [("read", 10 + i, 100), ("write", 20 + i, 200), ("open", 5, 0)][i % 3]
            extra = "" if name == "open" else ', "ret": %d' % ret
            f.write(
                '{"ph":"X","name":"%s","cat":"POSIX","pid":1,"tid":1,'
                '"ts":%d,"dur":%d,"args":{"fhash":"f%d"%s}}\n' % (name, 1000 + i, dur, i % 2, extra)
            )


def test_typed_read_to_ipc_is_the_ipc_form_of_view_typed_frames(tmp_path):
    """_typed_read_to_ipc emits {events, profiles, system} Arrow IPC in the
    dfanalyzer schema; decoding it reproduces view_typed_frames' event totals,
    since one is just the serialized form of the other."""
    from dftracer.utils import AggregationConfig
    from dftracer.utils.dfanalyzer import _ipc_to_pandas, _typed_read_to_ipc, view_typed_frames

    files = [str(tmp_path / f"t{k}.pfw.gz") for k in range(2)]
    for p in files:
        _make_trace(p)
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(
        files=files,
        index_dir=idx,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()

    # Granularity must match the tier's interval (100000 ms == 100 s): coarsening
    # only reads coarser than the stored bucket, never finer.
    scan = _typed_read_to_ipc(files, idx, 100.0, 1e6, None)
    assert set(scan) == {"events", "profiles", "system"}
    assert scan["events"] is not None

    ev = _ipc_to_pandas(scan["events"])
    frame = view_typed_frames(files, idx, time_granularity=100.0)["events"]
    # count is origin-independent, so the IPC (absolute time) and the frame
    # (origin-relative) agree on it exactly.
    assert int(ev["count"].sum()) == int(frame["count"].sum()) > 0
    assert {"cat", "func_name", "count", "time", "time_start"}.issubset(ev.columns)


def test_view_agg_tier_matches_raw_scan(tmp_path):
    """A TraceViewer over an aggregation-tier index is served from the tier and
    equals the raw-scan View over a plain index, for tier-answerable groupings
    (cat excluded: the tier lowercases it). sumsq is event-exact via the stored
    m2, so it matches too."""
    import pyarrow as pa

    from dftracer.utils import AggregationConfig, TraceViewer

    files = [str(tmp_path / f"t{k}.pfw.gz") for k in range(2)]
    for p in files:
        _make_trace(p)
    agg, noagg = str(tmp_path / "agg"), str(tmp_path / "noagg")
    with dftu_utils.Indexer(
        files=files,
        index_dir=agg,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()
    with dftu_utils.Indexer(files=files, index_dir=noagg) as ix:
        ix.ensure_indexed()

    keys = ["name", "io_cat", "pid", "tid"]
    metrics = [
        "count",
        "sum:dur",
        "sumsq:dur",
        "min:dur",
        "max:dur",
        "sum:size",
        "min:size",
        "max:size",
        "min:ts",
        "max:te",
    ]

    def collect(idx):
        tv = TraceViewer(files, index_path=idx).group_by(*keys).agg(*metrics)
        return pa.table(tv.collect()).to_pandas().sort_values(keys).reset_index(drop=True)

    tier, raw = collect(agg), collect(noagg)
    assert tier.shape == raw.shape and len(tier) > 0
    for c in tier.columns:
        if tier[c].dtype.kind in "if":
            assert (tier[c].astype(float) - raw[c].astype(float)).abs().max() < 1e-6, c
        else:
            assert (tier[c].astype(str).values == raw[c].astype(str).values).all(), c


def test_view_agg_tier_skew_kurt_pct_match_raw_scan(tmp_path):
    """skew/kurt (from persisted m3/m4) and percentiles (from the persisted
    DDSketch) come back from the aggregation CF tier and match the raw-scan
    View over a plain index."""
    import pyarrow as pa

    from dftracer.utils import AggregationConfig, TraceViewer

    files = [str(tmp_path / f"t{k}.pfw.gz") for k in range(2)]
    for p in files:
        _make_trace(p)
    agg, noagg = str(tmp_path / "agg"), str(tmp_path / "noagg")
    with dftu_utils.Indexer(
        files=files,
        index_dir=agg,
        require_aggregation=AggregationConfig(time_interval_ms=100000, compute_percentiles=True),
    ) as ix:
        ix.ensure_indexed()
    with dftu_utils.Indexer(files=files, index_dir=noagg) as ix:
        ix.ensure_indexed()

    metrics = ["skew:dur", "kurt:dur", "p50:dur", "p90:dur"]

    def collect(idx):
        tv = TraceViewer(files, index_path=idx).group_by("name").agg(*metrics)
        return pa.table(tv.collect()).to_pandas().sort_values("name").reset_index(drop=True)

    tier, raw = collect(agg), collect(noagg)
    assert tier.shape == raw.shape and len(tier) > 0
    for c in ["skew_dur", "kurt_dur"]:
        assert (tier[c].astype(float) - raw[c].astype(float)).abs().max() < 1e-6, c
    # DDSketch is approximate; both paths use it, so allow its relative error.
    for c in ["p50_dur", "p90_dur"]:
        rel = (tier[c].astype(float) - raw[c].astype(float)).abs() / raw[c].astype(float).clip(
            lower=1
        )
        assert rel.max() < 0.02, c


def test_view_histogram_tier_matches_raw_scan(tmp_path):
    """hist:dur comes back as an arrow list<struct<lo,hi,count>> column; the
    tier (from the persisted DDSketch) and the raw scan produce identical
    buckets, and the bucket counts sum to the event count."""
    import pyarrow as pa

    from dftracer.utils import AggregationConfig, TraceViewer

    files = [str(tmp_path / f"t{k}.pfw.gz") for k in range(2)]
    for p in files:
        _make_trace(p)
    agg, noagg = str(tmp_path / "agg"), str(tmp_path / "noagg")
    with dftu_utils.Indexer(
        files=files,
        index_dir=agg,
        require_aggregation=AggregationConfig(time_interval_ms=100000, compute_percentiles=True),
    ) as ix:
        ix.ensure_indexed()
    with dftu_utils.Indexer(files=files, index_dir=noagg) as ix:
        ix.ensure_indexed()

    def collect(idx):
        tv = TraceViewer(files, index_path=idx).group_by("name").agg("count", "hist:dur")
        return pa.table(tv.collect()).to_pandas().sort_values("name").reset_index(drop=True)

    tier, raw = collect(agg), collect(noagg)
    assert tier["hist_dur"].dtype == object and len(tier) > 0
    for i in range(len(tier)):
        th, rh = list(tier["hist_dur"][i]), list(raw["hist_dur"][i])
        assert sum(b["count"] for b in th) == int(tier["count"][i])
        assert len(th) == len(rh)
        for tb, rb in zip(th, rh):
            assert tb["count"] == rb["count"]
            assert abs(tb["lo"] - rb["lo"]) < 1e-9
            assert abs(tb["hi"] - rb["hi"]) < 1e-9


def test_collect_typed_one_pass_returns_all_record_families(tmp_path):
    """collect_typed reads regular events, aggregated records (arbitrary extra
    keys), and counters (incl. system) from one tier pass."""
    import gzip

    import pyarrow as pa

    from dftracer.utils import AggregationConfig, TraceViewer

    p = str(tmp_path / "t.pfw.gz")
    with gzip.open(p, "wt") as f:
        for i in range(20):
            f.write(
                '{"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,'
                '"ts":%d,"dur":%d}\n' % (1000 + i, 10 + i)
            )
        for e in range(3):
            f.write(
                '{"ph":"C","name":"train","cat":"app","pid":1,"tid":1,'
                '"ts":%d,"dur":5,"args":{"epoch":%d,"step":0}}\n' % (2000 + e, e)
            )
        for i in range(4):
            f.write(
                '{"ph":"C","name":"cpu","cat":"sys","pid":0,"tid":0,'
                '"ts":%d,"args":{"user_pct":%d}}\n' % (3000 + i, 40 + i)
            )
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(
        files=[p],
        index_dir=idx,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()

    res = TraceViewer([p], index_path=idx).group_by("name", "pid", "tid").collect_typed()
    assert set(res) == {"regular", "aggregated", "counters"}
    regular = pa.table(res["regular"]).to_pydict()
    aggregated = pa.table(res["aggregated"]).to_pydict()
    counters = pa.table(res["counters"]).to_pydict()

    assert regular["name"] == ["read"] and regular["count"] == [20.0]
    assert aggregated["name"] == ["train"] and aggregated["count"] == [3.0]
    # system counter: user_pct mean over 40..43.
    assert counters["name"] == ["cpu"]
    assert abs(counters["user_pct"][0] - 41.5) < 1e-9


def test_collect_typed_cat_filter_matches_collect(tmp_path):
    """A cat filter must not be silently dropped by collect_typed. The tier
    stores cat lowercased, so an equality/membership literal is folded to match
    (case-insensitively), and collect_typed agrees with collect()."""
    import gzip

    import pyarrow as pa
    import pyarrow.compute  # noqa: F401

    from dftracer.utils import AggregationConfig, TraceViewer

    p = str(tmp_path / "t.pfw.gz")
    with gzip.open(p, "wt") as f:
        for i in range(30):
            cat = "POSIX" if i % 3 else "STDIO"
            f.write(
                '{"ph":"X","name":"read","cat":"%s","pid":1,"tid":1,'
                '"ts":%d,"dur":%d}\n' % (cat, 1000 + i, 10 + i)
            )
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(
        files=[p],
        index_dir=idx,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()

    def count(q):
        tv = TraceViewer([p], index_path=idx).filter(q).group_by("name").agg("count")
        c = tv.collect()
        reg = tv.collect_typed()["regular"]
        cn = 0 if c is None else int(pa.compute.sum(pa.table(c)["count"]).as_py())
        rn = 0 if reg is None else int(pa.compute.sum(pa.table(reg)["count"]).as_py())
        return cn, rn

    # Both cases and the set form match, and collect_typed never silently empties.
    for q in ('cat == "posix"', 'cat == "POSIX"', 'cat in ["POSIX", "STDIO"]'):
        cn, rn = count(q)
        assert cn == rn > 0, (q, cn, rn)


def test_view_typed_frames_maps_all_three_to_dfanalyzer_schema(tmp_path):
    """view_typed_frames maps collect_typed's regular/aggregated/counters onto
    the dfanalyzer event/profile/system frames in one pass."""
    import gzip

    from dftracer.utils import AggregationConfig
    from dftracer.utils.dfanalyzer import view_typed_frames

    p = str(tmp_path / "t.pfw.gz")
    with gzip.open(p, "wt") as f:
        for i in range(20):
            f.write(
                '{"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,'
                '"ts":%d,"dur":%d,"args":{"ret":%d}}\n' % (1000 + i, 10 + i, 100 + i)
            )
        for e in range(3):
            f.write(
                '{"ph":"C","name":"train","cat":"app","pid":1,"tid":1,'
                '"ts":%d,"dur":5,"args":{"epoch":%d,"step":0}}\n' % (2000 + e, e)
            )
        for i in range(4):
            f.write(
                '{"ph":"C","name":"cpu","cat":"sys","pid":0,"tid":0,'
                '"ts":%d,"args":{"user_pct":%d}}\n' % (3000 + i, 40 + i)
            )
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(
        files=[p],
        index_dir=idx,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()

    fr = view_typed_frames([p], idx, time_granularity=100.0)
    ev, prof, sysf = fr["events"], fr["profiles"], fr["system"]

    assert set(["cat", "func_name", "count", "time", "time_start", "time_bucket"]).issubset(
        ev.columns
    )
    assert ev["func_name"].tolist() == ["read"] and int(ev["count"].iloc[0]) == 20
    # proc_name/file_hash/host_hash derived for the base HLM.
    assert set(["proc_name", "file_hash", "host_hash"]).issubset(ev.columns)
    assert ev["proc_name"].iloc[0].startswith("app#") and ev["proc_name"].iloc[0].endswith("#1#1")
    assert prof["func_name"].tolist() == ["train"] and int(prof["count"].iloc[0]) == 3
    assert sysf["name"].tolist() == ["cpu"]
    assert abs(float(sysf["user_pct"].iloc[0]) - 41.5) < 1e-9


@pytest.mark.skipif(not DASK_AVAILABLE, reason="Dask not available")
def test_view_typed_frames_distributed_matches_single_node(tmp_path):
    """The Dask-fanned typed read (disjoint shard ranges per worker) equals the
    single-node read, for events, profiles, and system."""
    import gzip

    from dask.distributed import Client, LocalCluster

    from dftracer.utils import AggregationConfig
    from dftracer.utils.dfanalyzer import view_typed_frames

    p = str(tmp_path / "t.pfw.gz")
    with gzip.open(p, "wt") as f:
        for i in range(300):
            f.write(
                '{"ph":"X","name":"read%d","cat":"POSIX","pid":%d,"tid":1,'
                '"ts":%d,"dur":%d}\n' % (i % 9, i % 6, 1000 + i, 10 + i)
            )
        for i in range(30):
            f.write(
                '{"ph":"C","name":"cpu","cat":"sys","pid":0,"tid":0,"ts":%d,'
                '"args":{"user_pct":%d,"hhash":"h%d"}}\n' % (3000 + i * 100, 40 + i, i % 4)
            )
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(
        files=[p],
        index_dir=idx,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()

    single = view_typed_frames([p], idx, time_granularity=100.0)
    cluster = LocalCluster(processes=False, n_workers=3, threads_per_worker=1)
    client = Client(cluster)
    try:
        dist = view_typed_frames([p], idx, time_granularity=100.0, client=client)
    finally:
        client.close()
        cluster.close()

    for name in ("events", "system"):
        assert len(single[name]) == len(dist[name]), name
    assert int(single["events"]["count"].sum()) == int(dist["events"]["count"].sum())
    assert set(single["system"]["name"]) == set(dist["system"]["name"])


def _make_trace_with_fh(path, pid):
    with gzip.open(path, "wt") as f:
        fh = f"f{pid}"
        f.write(
            '{"name":"FH","ph":"M","pid":%d,"tid":1,'
            '"args":{"name":"/data/dir/file%d.dat","value":"%s"}}\n' % (pid, pid, fh)
        )
        for i in range(50):
            f.write(
                '{"ph":"X","name":"read","cat":"POSIX","pid":%d,"tid":1,'
                '"ts":%d,"dur":%d,"args":{"ret":100,"fhash":"%s"}}\n' % (pid, 1000 + i, 10 + i, fh)
            )


def test_resolved_name_group_keys(tmp_path):
    """FilePath/FileName/HostName resolve the hash to a name after aggregation
    (a bijective re-key), so metrics are unchanged and file_name is the basename
    of file_path."""
    import pyarrow as pa

    from dftracer.utils import TraceViewer

    files = [str(tmp_path / f"t{p}.pfw.gz") for p in (1, 2)]
    for p, pid in zip(files, (1, 2)):
        _make_trace_with_fh(p, pid)
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(files=files, index_dir=idx) as ix:
        ix.ensure_indexed()

    def g(key):
        tv = TraceViewer(files, index_path=idx).group_by(key).agg("count", "sum:dur")
        return pa.table(tv.collect()).to_pandas().set_index(key).sort_index()

    fp, fn, fh = g("file_path"), g("file_name"), g("fhash")
    assert list(fp.index) == ["/data/dir/file1.dat", "/data/dir/file2.dat"]
    assert list(fn.index) == ["file1.dat", "file2.dat"]
    # Same metrics regardless of the label (re-key is a bijection here).
    for frame in (fp, fn, fh):
        assert list(frame["count"]) == [50, 50]
        assert list(frame["sum_dur"]) == [1725.0, 1725.0]


def test_collect_typed_resolves_name_group_keys(tmp_path):
    """collect_typed resolves FileName/HostName group keys to names (not the
    stored hash), matching the generic collect() re-key path."""
    import pyarrow as pa

    from dftracer.utils import AggregationConfig, TraceViewer

    files = [str(tmp_path / f"t{p}.pfw.gz") for p in (1, 2)]
    for p, pid in zip(files, (1, 2)):
        _make_trace_with_fh(p, pid)
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(
        files=files,
        index_dir=idx,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()

    typed = TraceViewer(files, index_path=idx).group_by("file_name").collect_typed()
    reg = pa.table(typed["regular"]).to_pandas()
    assert sorted(reg["file_name"].unique()) == ["file1.dat", "file2.dat"]


def test_percentiles_from_view(tmp_path):
    """p50/p90/p99 come back as numeric columns, within DDSketch's ~1% error."""
    import gzip

    import pyarrow as pa

    from dftracer.utils import TraceViewer

    p = str(tmp_path / "t.pfw.gz")
    with gzip.open(p, "wt") as f:
        for i in range(1000):  # dur = 1..1000
            f.write(
                '{"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,'
                '"ts":%d,"dur":%d}\n' % (1000 + i, i + 1)
            )
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(files=[p], index_dir=idx) as ix:
        ix.ensure_indexed()
    df = pa.table(
        TraceViewer([p], index_path=idx)
        .group_by("name")
        .agg("count", "p50:dur", "p90:dur", "p99:dur")
        .collect()
    ).to_pandas()
    assert list(df.columns) == ["name", "count", "p50_dur", "p90_dur", "p99_dur"]
    for col, exp in [("p50_dur", 500), ("p90_dur", 900), ("p99_dur", 990)]:
        assert abs(df[col][0] - exp) / exp < 0.02, col


def test_skew_kurtosis_from_view(tmp_path):
    """skew/kurtosis (from persisted m3/m4 power sums) match numpy population
    moments, and survive spill."""
    import gzip

    import numpy as np
    import pyarrow as pa

    from dftracer.utils import TraceViewer

    p = str(tmp_path / "t.pfw.gz")
    vals = [i * i for i in range(1, 501)]  # right-skewed
    with gzip.open(p, "wt") as f:
        for i, v in enumerate(vals):
            f.write(
                '{"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,'
                '"ts":%d,"dur":%d}\n' % (1000 + i, v)
            )
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(files=[p], index_dir=idx) as ix:
        ix.ensure_indexed()
    df = pa.table(
        TraceViewer([p], index_path=idx).group_by("name").agg("skew:dur", "kurt:dur").collect()
    ).to_pandas()
    a = np.array(vals, dtype=float)
    mu = a.mean()
    cm2 = ((a - mu) ** 2).mean()
    exp_skew = ((a - mu) ** 3).mean() / cm2**1.5
    exp_kurt = ((a - mu) ** 4).mean() / cm2**2 - 3
    assert abs(df["skew_dur"][0] - exp_skew) < 1e-4
    assert abs(df["kurt_dur"][0] - exp_kurt) < 1e-4


def test_view_typed_frames_folds_files_into_buckets(tmp_path):
    """group_by_file=False collapses the per-file rows into one, preserving the
    additive metrics and the global extremes, and still emits the file columns
    (empty) that dfanalyzer's dask meta declares."""
    from dftracer.utils import AggregationConfig
    from dftracer.utils.dfanalyzer import typed_group_keys, view_typed_frames

    n_files = 8
    p = str(tmp_path / "t.pfw.gz")
    with gzip.open(p, "wt") as f:
        for k in range(n_files):
            f.write(
                '{"name":"FH","ph":"M","pid":1,"tid":1,'
                '"args":{"name":"/data/f%d.dat","value":"h%d"}}\n' % (k, k)
            )
        for k in range(n_files):
            for i in range(5):
                f.write(
                    '{"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,'
                    '"ts":%d,"dur":%d,"args":{"ret":%d,"fhash":"h%d"}}\n'
                    % (1000 + i, 10 + k, 100 + k, k)
                )
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(
        files=[p],
        index_dir=idx,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()

    fine = view_typed_frames([p], idx, time_granularity=100.0)["events"]
    folded = view_typed_frames(
        [p], idx, time_granularity=100.0, group_keys=typed_group_keys(("/data", "/ckpt"))
    )["events"]

    assert len(fine) == n_files, f"expected one row per file, got {len(fine)}"
    assert len(folded) == 1, f"expected the files folded into one row, got {len(folded)}"

    for col in ("count", "time", "size"):
        assert abs(fine[col].sum() - folded[col].sum()) <= 1e-9 * max(1.0, abs(fine[col].sum()))
    assert fine["time_min"].min() == folded["time_min"].min()
    assert fine["time_max"].max() == folded["time_max"].max()

    assert set(fine.columns) == set(folded.columns)
    # file_name carries the matched bucket, not the full path and not blank:
    # the caller's file-name substring rules still have to fire on it.
    assert set(folded["file_name"]) <= {"/data", "/ckpt"}
    assert folded["file_name"].str.contains("/data").any()
    assert set(fine["file_name"]) != {""}


def test_drop_ignored_files_matches_folded_and_unfolded_names():
    """The drop runs after the read, so it must match whether the read kept the
    full path or folded it to the bucket the ignore pattern supplied."""
    import pandas as pd

    from dftracer.utils.dfanalyzer import _drop_ignored_files

    df = pd.DataFrame(
        {
            "cat": ["posix"] * 4,
            "file_name": ["/data/a.npz", "/data", "/tmp/junk.log", "/tmp"],
            "count": [1, 2, 3, 4],
        }
    )
    out = _drop_ignored_files(df, ("/tmp",))

    assert set(out["file_name"]) == {"/data/a.npz", "/data"}
    assert out["count"].sum() == 3

    # No patterns is a no-op, not a blanket drop.
    assert len(_drop_ignored_files(df, ())) == 4


def _make_trace_with_paths(path, pid, paths):
    """One trace whose events cycle through `paths`, each with its own fhash."""
    with gzip.open(path, "wt") as f:
        for i, p in enumerate(paths):
            f.write(
                '{"name":"FH","ph":"M","pid":%d,"tid":1,'
                '"args":{"name":"%s","value":"f%d_%d"}}\n' % (pid, p, pid, i)
            )
        for i, p in enumerate(paths):
            for j in range(10):
                f.write(
                    '{"ph":"X","name":"read","cat":"POSIX","pid":%d,"tid":1,'
                    '"ts":%d,"dur":%d,"args":{"ret":100,"fhash":"f%d_%d"}}\n'
                    % (pid, 1000 + j, 10 + j, pid, i)
                )


def test_fileless_index_serves_non_file_queries(tmp_path):
    """An index built with group_by_file=False keeps no file dimension. The
    tier must still answer queries that do not group by file; only file-grouped
    ones fall back, since the fold cannot be reversed."""
    import pyarrow as pa

    from dftracer.utils import AggregationConfig, TraceViewer

    paths = [f"/data/shard{i}.npz" for i in range(6)]
    p = str(tmp_path / "t.pfw.gz")
    _make_trace_with_paths(p, 1, paths)

    keyed, fileless = str(tmp_path / "keyed"), str(tmp_path / "fileless")
    for idx, by_file in ((keyed, True), (fileless, False)):
        with dftu_utils.Indexer(
            files=[p],
            index_dir=idx,
            require_aggregation=AggregationConfig(time_interval_ms=100000, group_by_file=by_file),
        ) as ix:
            ix.ensure_indexed()

    def totals(idx):
        tv = TraceViewer([p], index_path=idx).group_by("name").agg("count", "sum:dur")
        return pa.table(tv.collect_typed()["regular"]).to_pandas()

    keyed_rows, fileless_rows = totals(keyed), totals(fileless)

    assert len(fileless_rows) > 0, "fileless index must still serve a non-file query"
    assert fileless_rows["count"].sum() == keyed_rows["count"].sum()
    assert fileless_rows["sum_dur"].sum() == keyed_rows["sum_dur"].sum()


def test_group_key_transforms_coarsen_without_losing_totals(tmp_path):
    """A group key may carry a value transform. Each one folds the values
    differently, but every fold is a pure regrouping, so the totals hold."""
    import pyarrow as pa

    from dftracer.utils import TraceViewer

    paths = [f"/data/train/f{i}.npz" for i in range(6)] + [f"/ckpt/s{i}.bin" for i in range(4)]
    p = str(tmp_path / "t.pfw.gz")
    _make_trace_with_paths(p, 1, paths)
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(files=[p], index_dir=idx) as ix:
        ix.ensure_indexed()

    def group(expr):
        tv = TraceViewer([p], index_path=idx).group_by(expr).agg("count")
        df = pa.table(tv.collect()).to_pandas()
        return sorted(df[df.columns[0]].tolist()), int(df["count"].sum())

    plain, total = group("file_path")
    assert len(plain) == len(paths)

    for expr, expected in (
        ("dirname(file_path)", ["/ckpt", "/data/train"]),
        ("bucket(file_path, '/data', '/ckpt')", ["/ckpt", "/data"]),
    ):
        values, folded_total = group(expr)
        assert values == expected, expr
        assert folded_total == total, f"{expr} changed the total"

    # basename keeps one group per file here, so it must not fold anything.
    base, base_total = group("basename(file_path)")
    assert len(base) == len(paths) and base_total == total

    lowered, lowered_total = group("lower(cat)")
    assert lowered == ["posix"] and lowered_total == total


def test_group_key_transform_applies_on_both_read_paths(tmp_path):
    """The transform must fold identically whether the tier or a raw scan
    serves the query, or the same query answers differently depending on
    whether an aggregation index happens to exist."""
    import pyarrow as pa

    from dftracer.utils import AggregationConfig, TraceViewer

    paths = [f"/data/train/f{i}.npz" for i in range(6)]
    p = str(tmp_path / "t.pfw.gz")
    _make_trace_with_paths(p, 1, paths)

    scan_idx, tier_idx = str(tmp_path / "scan"), str(tmp_path / "tier")
    with dftu_utils.Indexer(files=[p], index_dir=scan_idx) as ix:
        ix.ensure_indexed()
    with dftu_utils.Indexer(
        files=[p],
        index_dir=tier_idx,
        require_aggregation=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure_indexed()

    def group(idx):
        tv = TraceViewer([p], index_path=idx).group_by("dirname(file_path)").agg("count")
        df = pa.table(tv.collect()).to_pandas()
        return sorted(df[df.columns[0]].tolist()), int(df["count"].sum())

    assert group(scan_idx) == group(tier_idx)
