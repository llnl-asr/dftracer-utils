#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Parity checks for the View aggregation: the aggregation tier against the
raw scan, and name, percentile, skew and group-key resolution."""

import dataclasses
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


def _viewer(tv, scan):
    # The tier cannot evaluate a filter on ts, so this one makes the view scan
    # every record.
    return tv.filter("ts >= 0") if scan else tv


def test_view_agg_tier_matches_raw_scan(tmp_path):
    """A TraceViewer over an aggregation-tier index is served from the tier and
    equals the raw scan of the same files, for tier-answerable groupings
    (cat excluded: the tier lowercases it). sumsq is event-exact via the stored
    m2, so it matches too."""

    from dftracer.utils import AggregationConfig, TraceViewer

    files = [str(tmp_path / f"t{k}.pfw.gz") for k in range(2)]
    for p in files:
        _make_trace(p)
    agg = str(tmp_path / "agg")
    with dftu_utils.Indexer(
        files=files,
        index_dir=agg,
        aggregation_config=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure()

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

    def collect(idx, scan=False):
        tv = _viewer(TraceViewer(files, index_dir=idx), scan).group_by(*keys).agg(*metrics)
        return tv.collect().to_pandas().sort_values(keys).reset_index(drop=True)

    tier, raw = collect(agg), collect(agg, scan=True)
    assert tier.shape == raw.shape and len(tier) > 0
    for c in tier.columns:
        if tier[c].dtype.kind in "if":
            assert (tier[c].astype(float) - raw[c].astype(float)).abs().max() < 1e-6, c
        else:
            assert (tier[c].astype(str).values == raw[c].astype(str).values).all(), c


def test_view_agg_tier_serves_offset_like_raw_scan(tmp_path):
    """min/max/sum of the offset arg come from the tier, not as null: offset is
    an arg, so the engine names its column args.offset, which the tier must map
    back to its stored offset stats. A group with no offset stays null."""

    from dftracer.utils import AggregationConfig, TraceViewer

    path = str(tmp_path / "t.pfw.gz")
    with gzip.open(path, "wt") as f:
        for i in range(30):
            name = ("read", "write", "open")[i % 3]
            offset = "" if name == "open" else ', "offset": %d' % (100 + i)
            f.write(
                '{"ph":"X","name":"%s","cat":"POSIX","pid":1,"tid":1,'
                '"ts":%d,"dur":10,"args":{"fhash":"f0"%s}}\n' % (name, 1000 + i, offset)
            )
    agg = str(tmp_path / "agg")
    with dftu_utils.Indexer(
        files=[path],
        index_dir=agg,
        aggregation_config=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure()

    def collect(idx, scan=False):
        tv = (
            _viewer(TraceViewer([path], index_dir=idx), scan)
            .group_by("name")
            .agg("min:offset", "max:offset", "sum:offset")
        )
        return tv.collect().to_pandas().sort_values("name").reset_index(drop=True)

    tier, raw = collect(agg), collect(agg, scan=True)
    assert raw.set_index("name").loc["read", "min_offset"] == 100
    assert tier.set_index("name").loc["read", "min_offset"] == 100
    assert tier.set_index("name").loc["write", "max_offset"] == 128
    assert tier.set_index("name")["min_offset"].isna().loc["open"]
    assert (
        tier.fillna(-1).drop(columns="name").astype(float).values
        == raw.fillna(-1).drop(columns="name").astype(float).values
    ).all()


def test_view_agg_tier_skew_kurt_pct_match_raw_scan(tmp_path):
    """skew/kurt (from persisted m3/m4) and percentiles (from the persisted
    DDSketch) come back from the aggregation CF tier and match the raw scan."""

    from dftracer.utils import AggregationConfig, TraceViewer

    files = [str(tmp_path / f"t{k}.pfw.gz") for k in range(2)]
    for p in files:
        _make_trace(p)
    agg = str(tmp_path / "agg")
    with dftu_utils.Indexer(
        files=files,
        index_dir=agg,
        aggregation_config=AggregationConfig(time_interval_ms=100000, compute_percentiles=True),
    ) as ix:
        ix.ensure()

    metrics = ["skew:dur", "kurt:dur", "p50:dur", "p90:dur"]

    def collect(idx, scan=False):
        tv = _viewer(TraceViewer(files, index_dir=idx), scan).group_by("name").agg(*metrics)
        return tv.collect().to_pandas().sort_values("name").reset_index(drop=True)

    tier, raw = collect(agg), collect(agg, scan=True)
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

    from dftracer.utils import AggregationConfig, TraceViewer

    files = [str(tmp_path / f"t{k}.pfw.gz") for k in range(2)]
    for p in files:
        _make_trace(p)
    agg = str(tmp_path / "agg")
    with dftu_utils.Indexer(
        files=files,
        index_dir=agg,
        aggregation_config=AggregationConfig(time_interval_ms=100000, compute_percentiles=True),
    ) as ix:
        ix.ensure()

    def collect(idx, scan=False):
        tv = (
            _viewer(TraceViewer(files, index_dir=idx), scan)
            .group_by("name")
            .agg("count", "hist:dur")
        )
        return tv.collect().to_pandas().sort_values("name").reset_index(drop=True)

    tier, raw = collect(agg), collect(agg, scan=True)
    assert tier["hist_dur"].dtype == object and len(tier) > 0
    for i in range(len(tier)):
        th, rh = list(tier["hist_dur"][i]), list(raw["hist_dur"][i])
        assert sum(b["count"] for b in th) == int(tier["count"][i])
        assert len(th) == len(rh)
        for tb, rb in zip(th, rh):
            assert tb["count"] == rb["count"]
            assert abs(tb["lo"] - rb["lo"]) < 1e-9
            assert abs(tb["hi"] - rb["hi"]) < 1e-9


def test_collect_index_records_one_pass_returns_all_record_families(tmp_path):
    """collect_index_records reads regular events, aggregated records (arbitrary extra
    keys), and counters (incl. system) from one tier pass."""
    import gzip

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
        aggregation_config=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure()

    res = TraceViewer([p], index_dir=idx).group_by("name", "pid", "tid").collect_index_records()
    assert isinstance(res, dftu_utils.IndexRecords)
    regular = res.regular.to_dict()
    aggregated = res.aggregated.to_dict()
    counters = res.counters.to_dict()

    assert regular["name"] == ["read"] and regular["count"] == [20.0]
    assert aggregated["name"] == ["train"] and aggregated["count"] == [3.0]
    # system counter: user_pct mean over 40..43.
    assert counters["name"] == ["cpu"]
    assert abs(counters["user_pct"][0] - 41.5) < 1e-9

    assert all(
        isinstance(frame, dftu_utils.DataFrame)
        for frame in (res.regular, res.aggregated, res.counters)
    )
    with pytest.raises(dataclasses.FrozenInstanceError):
        res.regular = res.counters  # type: ignore[misc]


def test_collect_index_records_cat_filter_matches_collect(tmp_path):
    """A cat filter must not be silently dropped by collect_index_records. The tier
    stores cat lowercased, so an equality/membership literal is folded to match
    (case-insensitively), and collect_index_records agrees with collect()."""
    import gzip

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
        aggregation_config=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure()

    def count(q):
        tv = TraceViewer([p], index_dir=idx).filter(q).group_by("name").agg("count")
        c = tv.collect()
        reg = tv.collect_index_records().regular
        cn = 0 if c is None else int(sum(c["count"].to_list()))
        rn = 0 if reg is None else int(sum(reg["count"].to_list()))
        return cn, rn

    # Both cases and the set form match, and collect_index_records never silently empties.
    for q in ('cat == "posix"', 'cat == "POSIX"', 'cat in ["POSIX", "STDIO"]'):
        cn, rn = count(q)
        assert cn == rn > 0, (q, cn, rn)


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

    from dftracer.utils import TraceViewer

    files = [str(tmp_path / f"t{p}.pfw.gz") for p in (1, 2)]
    for p, pid in zip(files, (1, 2)):
        _make_trace_with_fh(p, pid)
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(files=files, index_dir=idx) as ix:
        ix.ensure()

    def g(key):
        tv = TraceViewer(files, index_dir=idx).group_by(key).agg("count", "sum:dur")
        return tv.collect().to_pandas().set_index(key).sort_index()

    fp, fn, fh = g("file_path"), g("file_name"), g("fhash")
    assert list(fp.index) == ["/data/dir/file1.dat", "/data/dir/file2.dat"]
    assert list(fn.index) == ["file1.dat", "file2.dat"]
    # Same metrics regardless of the label (re-key is a bijection here).
    for frame in (fp, fn, fh):
        assert list(frame["count"]) == [50, 50]
        assert list(frame["sum_dur"]) == [1725.0, 1725.0]


def test_collect_index_records_resolves_name_group_keys(tmp_path):
    """collect_index_records resolves FileName/HostName group keys to names (not the
    stored hash), matching the generic collect() re-key path."""

    from dftracer.utils import AggregationConfig, TraceViewer

    files = [str(tmp_path / f"t{p}.pfw.gz") for p in (1, 2)]
    for p, pid in zip(files, (1, 2)):
        _make_trace_with_fh(p, pid)
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(
        files=files,
        index_dir=idx,
        aggregation_config=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure()

    records = TraceViewer(files, index_dir=idx).group_by("file_name").collect_index_records()
    reg = records.regular.to_pandas()
    assert sorted(reg["file_name"].unique()) == ["file1.dat", "file2.dat"]


def test_percentiles_from_view(tmp_path):
    """p50/p90/p99 come back as numeric columns, within DDSketch's ~1% error."""
    import gzip

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
        ix.ensure()
    df = (
        TraceViewer([p], index_dir=idx)
        .group_by("name")
        .agg("count", "p50:dur", "p90:dur", "p99:dur")
        .collect()
        .to_pandas()
    )
    assert list(df.columns) == ["name", "count", "p50_dur", "p90_dur", "p99_dur"]
    for col, exp in [("p50_dur", 500), ("p90_dur", 900), ("p99_dur", 990)]:
        assert abs(df[col][0] - exp) / exp < 0.02, col


def test_skew_kurtosis_from_view(tmp_path):
    """skew/kurtosis (from persisted m3/m4 power sums) match numpy population
    moments, and survive spill."""
    import gzip

    import numpy as np

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
        ix.ensure()
    df = (
        TraceViewer([p], index_dir=idx).group_by("name").agg("skew:dur", "kurt:dur").collect()
    ).to_pandas()
    a = np.array(vals, dtype=float)
    mu = a.mean()
    cm2 = ((a - mu) ** 2).mean()
    exp_skew = ((a - mu) ** 3).mean() / cm2**1.5
    exp_kurt = ((a - mu) ** 4).mean() / cm2**2 - 3
    assert abs(df["skew_dur"][0] - exp_skew) < 1e-4
    assert abs(df["kurt_dur"][0] - exp_kurt) < 1e-4


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

    from dftracer.utils import AggregationConfig, TraceViewer

    paths = [f"/data/shard{i}.npz" for i in range(6)]
    p = str(tmp_path / "t.pfw.gz")
    _make_trace_with_paths(p, 1, paths)

    keyed, fileless = str(tmp_path / "keyed"), str(tmp_path / "fileless")
    for idx, by_file in ((keyed, True), (fileless, False)):
        with dftu_utils.Indexer(
            files=[p],
            index_dir=idx,
            aggregation_config=AggregationConfig(time_interval_ms=100000, group_by_file=by_file),
        ) as ix:
            ix.ensure()

    def totals(idx):
        tv = TraceViewer([p], index_dir=idx).group_by("name").agg("count", "sum:dur")
        return tv.collect_index_records().regular.to_pandas()

    keyed_rows, fileless_rows = totals(keyed), totals(fileless)

    assert len(fileless_rows) > 0, "fileless index must still serve a non-file query"
    assert fileless_rows["count"].sum() == keyed_rows["count"].sum()
    assert fileless_rows["sum_dur"].sum() == keyed_rows["sum_dur"].sum()


def test_group_key_transforms_coarsen_without_losing_totals(tmp_path):
    """A group key may carry a value transform. Each one folds the values
    differently, but every fold is a pure regrouping, so the totals hold."""

    from dftracer.utils import TraceViewer

    paths = [f"/data/train/f{i}.npz" for i in range(6)] + [f"/ckpt/s{i}.bin" for i in range(4)]
    p = str(tmp_path / "t.pfw.gz")
    _make_trace_with_paths(p, 1, paths)
    idx = str(tmp_path / "idx")
    with dftu_utils.Indexer(files=[p], index_dir=idx) as ix:
        ix.ensure()

    def group(expr):
        tv = TraceViewer([p], index_dir=idx).group_by(expr).agg("count")
        df = tv.collect().to_pandas()
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

    from dftracer.utils import AggregationConfig, TraceViewer

    paths = [f"/data/train/f{i}.npz" for i in range(6)]
    p = str(tmp_path / "t.pfw.gz")
    _make_trace_with_paths(p, 1, paths)

    scan_idx, tier_idx = str(tmp_path / "scan"), str(tmp_path / "tier")
    with dftu_utils.Indexer(files=[p], index_dir=scan_idx) as ix:
        ix.ensure()
    with dftu_utils.Indexer(
        files=[p],
        index_dir=tier_idx,
        aggregation_config=AggregationConfig(time_interval_ms=100000),
    ) as ix:
        ix.ensure()

    def group(idx):
        tv = TraceViewer([p], index_dir=idx).group_by("dirname(file_path)").agg("count")
        df = tv.collect().to_pandas()
        return sorted(df[df.columns[0]].tolist()), int(df["count"].sum())

    assert group(scan_idx) == group(tier_idx)
