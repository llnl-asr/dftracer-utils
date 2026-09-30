"""var, std, skew and kurt stay accurate when the mean is large next to the spread.

Every check compares with a corrected two-pass reference (deviations from the
compensated mean, with the correction for the mean's own rounding), to a relative
1e-9 (skew and kurt: 1e-9 times the larger of 1 and the value, since they sit
near 0 for symmetric data).
"""

import gzip
import json
import math

import numpy as np
import pytest

from dftracer.utils import DataFrame, col

TOL = 1e-9


def two_pass(v):
    v = np.asarray(v, dtype=np.float64)
    n = len(v)
    mu = math.fsum(v) / n
    d = v - mu
    s1, s2, s3, s4 = d.sum(), (d * d).sum(), (d**3).sum(), (d**4).sum()
    e = s1 / n
    m2 = s2 - s1 * e
    m3 = s3 - 3 * e * s2 + 2 * n * e**3
    m4 = s4 - 4 * e * s3 + 6 * e * e * s2 - 3 * n * e**4
    var = m2 / (n - 1)
    c2 = m2 / n
    return {
        "var": var,
        "std": math.sqrt(var),
        "skew": (m3 / n) / c2**1.5,
        "kurt": (m4 / n) / c2**2 - 3.0,
    }


def rel(got, want):
    return abs(got - want) / abs(want)


def near(got, want, tol=TOL):
    return abs(got - want) <= tol * max(1.0, abs(want))


def normal(mean, sd, n, seed):
    return mean + sd * np.random.default_rng(seed).normal(0.0, 1.0, n)


def frame(values, key="a"):
    return DataFrame.from_dict({"g": [key] * len(values), "x": list(map(float, values))})


def group_value(f, op):
    return f.group_by("g").agg(r=getattr(col("x"), op)()).to_pandas()["r"][0]


N = 1_000_000


def test_large_offset_std_and_var():
    v = normal(1e9, 1.0, N, 0)
    ref = two_pass(v)
    f = frame(v)
    assert rel(f["x"].std(), ref["std"]) < TOL
    assert rel(f["x"].var(), ref["var"]) < TOL
    assert rel(group_value(f, "std"), ref["std"]) < TOL
    assert rel(group_value(f, "var"), ref["var"]) < TOL


def test_tiny_spread_std():
    v = normal(1e3, 1e-3, N, 1)
    ref = two_pass(v)
    f = frame(v)
    assert rel(f["x"].std(), ref["std"]) < TOL
    assert rel(group_value(f, "std"), ref["std"]) < TOL


def test_skew_and_kurt_with_an_offset():
    v = normal(1e9, 1.0, N, 3)
    ref = two_pass(v)
    f = frame(v)
    assert near(f["x"].skew(), ref["skew"])
    assert near(f["x"].kurt(), ref["kurt"])
    assert near(group_value(f, "skew"), ref["skew"])
    assert near(group_value(f, "kurt"), ref["kurt"])


@pytest.mark.parametrize("n", [100_000, 262_144, 262_145, 400_000])
def test_accuracy_holds_across_the_parallel_chunk_boundary(n):
    v = normal(1e9, 1.0, n, 8)
    ref = two_pass(v)
    f = frame(v)
    assert rel(f["x"].std(), ref["std"]) < TOL
    assert near(f["x"].skew(), ref["skew"])


def test_constant_columns_have_zero_std():
    assert DataFrame.from_dict({"x": [3.0, 3.0, 3.0, 3.0]})["x"].std() == 0.0
    assert frame([1e9] * N)["x"].std() == 0.0
    assert group_value(frame([1e9] * 1000), "std") == 0.0


def test_a_single_value_gives_nan():
    one = frame([5.0])
    assert math.isnan(one["x"].std()) and math.isnan(one["x"].var())
    assert math.isnan(group_value(one, "std"))
    one_of_nulls = DataFrame.from_dict({"g": ["a", "a", "a"], "x": [None, 2.0, None]})
    assert math.isnan(one_of_nulls["x"].std())
    assert math.isnan(one_of_nulls.group_by("g").agg(r=col("x").std()).to_pandas()["r"][0])


def test_nulls_are_skipped():
    d = DataFrame.from_dict({"x": [1.0, None, 3.0]})
    assert d["x"].std() == pytest.approx(math.sqrt(2.0), rel=1e-12)
    assert d["x"].var() == pytest.approx(2.0, rel=1e-12)


def test_integer_column_with_a_large_offset():
    rng = np.random.default_rng(2)
    iv = 1_700_000_000_000_000 + np.rint(rng.normal(0, 1000.0, N)).astype(np.int64)
    # exact reference: integer deviations from an integer near the mean
    ints = iv.tolist()
    k = sum(ints) // N  # Python integers: the sum overflows int64
    d = [x - k for x in ints]
    m2 = sum(x * x for x in d) - sum(d) ** 2 / N
    ref = math.sqrt(m2 / (N - 1))
    f = DataFrame.from_dict({"g": ["a"] * N, "x": iv.tolist()})
    assert rel(f["x"].std(), ref) < TOL
    assert rel(f.group_by("g").agg(r=col("x").std()).to_pandas()["r"][0], ref) < TOL


def test_many_groups_at_different_offsets():
    groups, per = 1000, 1000
    rng = np.random.default_rng(5)
    keys, vals, refs = [], [], {}
    for g in range(groups):
        v = g * 1e9 + rng.normal(0.0, 1.0, per)  # means from 0 to 1e12
        keys += [g] * per
        vals += v.tolist()
        refs[g] = two_pass(v)
    f = DataFrame.from_dict({"g": keys, "x": vals})
    out = f.group_by("g").agg(s=col("x").std(), v=col("x").var()).to_pandas()
    worst = max(
        max(rel(r.s, refs[int(r.g)]["std"]), rel(r.v, refs[int(r.g)]["var"]))
        for r in out.itertuples()
    )
    assert worst < TOL


def test_rolling_std_with_an_offset():
    v = normal(1e9, 1.0, 2000, 6)
    got = frame(v)["x"].rolling(5).std().to_list()
    for i in range(4, len(v)):
        assert rel(got[i], two_pass(v[i - 4 : i + 1])["std"]) < TOL


def test_ewm_std_is_shift_invariant():
    rng = np.random.default_rng(7)
    base = np.floor(rng.normal(500.0, 100.0, 2000))
    a = frame(base)["x"].ewm_std(0.3).to_list()
    b = frame(base + 1e9)["x"].ewm_std(0.3).to_list()
    assert all(rel(y, x) < TOL for x, y in zip(a[2:], b[2:]))


def test_merged_partitions_equal_one_pass():
    from dftracer.utils.dask_frame import DaskFrame

    v = normal(1e9, 1.0, N, 4)
    ref = two_pass(v)
    cuts = [0, 1, 200_000, 333_333, 333_334, 700_001, 900_000, N]  # one value, uneven
    parts = [frame(v[a:b]) for a, b in zip(cuts, cuts[1:])]
    parts.append(parts[0].head(0))  # and an empty partition
    df = DaskFrame.from_frames(parts)
    out = df.group_by("g").agg(s=("x", "std"), v=("x", "var")).to_frame().to_pandas()
    assert rel(out["s"][0], ref["std"]) < TOL
    assert rel(out["v"][0], ref["var"]) < TOL
    df.close()


# ---- the aggregation tier of an index gives the same variance as a scan ----------------


def write_trace(path, durs):
    with gzip.open(path, "wt") as f:
        for i, d in enumerate(durs):
            f.write(
                json.dumps(
                    {
                        "ph": "X",
                        "name": "read",
                        "cat": "POSIX",
                        "pid": 1,
                        "tid": 1,
                        "ts": 1_000 + i * 10,
                        "dur": int(d),
                        "args": {},
                    }
                )
                + "\n"
            )


def test_tier_and_scan_give_the_same_std(tmp_path):
    import dftracer.utils as du
    from dftracer.utils import AggregationConfig, TraceViewer

    durs = np.rint(normal(1e9, 1000.0, 20000, 11)).astype(np.int64)
    ref = two_pass(durs.astype(np.float64))
    path = str(tmp_path / "t.pfw.gz")
    write_trace(path, durs)
    agg, plain = str(tmp_path / "agg"), str(tmp_path / "plain")
    with du.Indexer(
        files=[path], index_dir=agg, require_aggregation=AggregationConfig(time_interval_ms=100000)
    ) as ix:
        ix.ensure_indexed()
    with du.Indexer(files=[path], index_dir=plain) as ix:
        ix.ensure_indexed()

    def run(idx):
        tv = TraceViewer([path], index_path=idx).group_by("name").agg("std:dur", "var:dur")
        r = tv.collect().to_pandas()
        return float(r["std_dur"][0]), float(r["var_dur"][0])

    tier_std, tier_var = run(agg)
    scan_std, scan_var = run(plain)
    assert rel(scan_std, ref["std"]) < TOL
    assert rel(tier_std, ref["std"]) < TOL
    assert rel(tier_var, ref["var"]) < TOL
    assert rel(scan_var, ref["var"]) < TOL
