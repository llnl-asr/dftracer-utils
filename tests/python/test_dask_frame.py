"""DaskFrame: map, combine, shuffle, join and seams, in process and on a cluster.

Every test runs twice: with no client (in memory) and on a LocalCluster of four
worker processes. Each compares with the same operation on the concatenated frame,
so the two modes are also equal to each other.
"""

import inspect
import sys

import numpy as np
import pandas as pd
import pytest

pytest.importorskip("distributed")
cloudpickle = pytest.importorskip("cloudpickle")

from dftracer.utils import DataFrame, col  # noqa: E402
from dftracer.utils.dask import DaskFrame as ExportedDaskFrame  # noqa: E402
from dftracer.utils.dask_frame import DaskFrame, _join  # noqa: E402

# Workers are spawned processes that cannot import this module; ship its functions by value.
cloudpickle.register_pickle_by_value(sys.modules[__name__])


@pytest.fixture(scope="module")
def cluster():
    from dask.distributed import Client, LocalCluster

    lc = LocalCluster(n_workers=4, threads_per_worker=1, processes=True, silence_logs=40)
    client = Client(lc)
    yield client
    client.close()
    lc.close()


@pytest.fixture(params=["local", "cluster"])
def client(request):
    return None if request.param == "local" else request.getfixturevalue("cluster")


def sorted_pd(frame, keys):
    return frame.to_pandas().sort_values(keys).reset_index(drop=True)


def assert_same(a, b, keys, rtol=None):
    x, y = sorted_pd(a, keys), sorted_pd(b, keys)
    assert list(x.columns) == list(y.columns)
    if rtol is None:
        pd.testing.assert_frame_equal(x, y, check_dtype=False, check_exact=True)
    else:
        pd.testing.assert_frame_equal(x, y, check_dtype=False, rtol=rtol, atol=0)


def make_events(seed, n=1_000_000):
    r = np.random.default_rng(seed)
    return DataFrame.from_pandas(
        pd.DataFrame(
            {
                "name": r.choice(["read", "write", "open", "close"], n),
                "pid": r.integers(0, 64, n),
                "dur": r.integers(1, 10_000, n),
            }
        )
    )


def make_keyed(seed, n=400_000, keys=200_000):
    r = np.random.default_rng(seed)
    return DataFrame.from_pandas(
        pd.DataFrame(
            {
                "k": r.integers(0, keys, n),
                "x": r.integers(0, 1000, n),
                "y": r.integers(0, 50, n),
                "s": r.choice([f"s{i}" for i in range(20)], n),
            }
        )
    )


def make_small(seed, n=1000):
    r = np.random.default_rng(seed)
    return DataFrame.from_pandas(
        pd.DataFrame({"a": r.integers(0, 10, n), "x": r.integers(0, 100, n)})
    )


def make_series(seed):
    return DataFrame.from_pandas(
        pd.DataFrame({"x": np.random.default_rng(seed).normal(0, 1, 1000)})
    )


def per_group_exact(frame):
    g = frame.group_by("k")
    out = g.agg(nu=col("y").distinct(), n=col("x").count()).to_pandas().set_index("k")
    out["med"] = g.quantile(0.5).to_pandas().set_index("k")["x"]
    out["p90"] = g.quantile(0.9).to_pandas().set_index("k")["x"]
    return DataFrame.from_pandas(out.reset_index())


def per_group_sets(frame):
    return frame.group_by("k").agg(u=col("s").set_union())


def test_exported_from_dask_module():
    assert ExportedDaskFrame is DaskFrame


@pytest.mark.valgrind
def test_map_partitions_adds_a_column(client):
    f = DaskFrame.from_function(make_small, range(4), client)
    out = f.map_partitions(lambda p: p.with_columns(y=col("x") * 2))
    whole = f.to_frame().with_columns(y=col("x") * 2)
    pd.testing.assert_frame_equal(out.to_frame().to_pandas(), whole.to_pandas(), check_dtype=False)
    f.close()


def test_agg_partials_equal_the_whole(client):
    f = DaskFrame.from_function(make_events, range(4), client)
    keys = ["name", "pid"]
    got = f.group_by(keys).agg(
        n=col("dur").count(),
        s=col("dur").sum(),
        lo=col("dur").min(),
        hi=col("dur").max(),
        sq=col("dur").sumsq(),
    )
    whole = (
        f.to_frame()
        .group_by(keys)
        .agg(
            n=col("dur").count(),
            s=col("dur").sum(),
            lo=col("dur").min(),
            hi=col("dur").max(),
            sq=col("dur").sumsq(),
        )
    )
    # Counts, sums, min and max are exact. The sum of squares is rebuilt from the
    # central moments (n * mean^2 + M2), so it matches the whole frame to rounding
    # (about 1e-15 relative), not bit for bit as the raw sum of small integers did.
    exact = [*keys, "n", "s", "lo", "hi"]
    assert_same(got.to_frame().select(*exact), whole.select(*exact), keys)
    assert_same(got.to_frame().select(*keys, "sq"), whole.select(*keys, "sq"), keys, rtol=1e-12)
    f.close()


def test_agg_mean_var_std_from_partials(client):
    f = DaskFrame.from_function(make_events, range(4), client)
    got = f.group_by("name").agg(m=("dur", "mean"), v=("dur", "var"), sd=("dur", "std"))
    whole = (
        f.to_frame()
        .group_by("name")
        .agg(m=col("dur").mean(), v=col("dur").var(), sd=col("dur").std())
    )
    assert_same(got.to_frame(), whole, ["name"], rtol=1e-9)
    f.close()


def test_agg_refuses_a_non_combining_aggregate(client):
    f = DaskFrame.from_function(make_small, range(2), client)
    with pytest.raises(ValueError, match=r"median.*shuffle"):
        f.group_by("a").agg(m=("x", "median"))
    with pytest.raises(ValueError, match="shuffle"):
        f.group_by("a").agg(q=col("x").quantile(0.5))
    f.close()


@pytest.mark.valgrind
def test_reduce_is_a_tree(client):
    f = DaskFrame.from_function(make_small, range(5), client)
    assert f.reduce(len, sum, split_every=2) == 5000
    assert f.reduce(len, sum) == 5000
    one = DaskFrame.from_function(make_small, [0], client)
    assert one.reduce(len, sum) == 1000
    with pytest.raises(ValueError):
        f.reduce(len, sum, split_every=1)
    f.close()


def test_shuffle_puts_each_key_in_one_partition(client):
    f = DaskFrame.from_function(make_keyed, range(8), client)
    s = f.shuffle("k", 8)
    parts = s._gather(s._parts)
    assert sum(len(p) for p in parts) == 3_200_000
    keysets = [set(p.to_pandas()["k"]) for p in parts]
    assert sum(len(a & b) for i, a in enumerate(keysets) for b in keysets[i + 1 :]) == 0
    sizes = [len(p) for p in parts]
    assert (max(sizes) - min(sizes)) / min(sizes) < 0.02
    f.close()


def test_exact_per_group_operations_after_a_shuffle(client):
    f = DaskFrame.from_function(make_keyed, range(8), client)
    got = f.shuffle("k", 8).map_partitions(per_group_exact).to_frame()
    whole = per_group_exact(f.to_frame())
    x, y = sorted_pd(got, ["k"]), sorted_pd(whole, ["k"])
    assert len(x) == len(y) == len(set(f.to_frame().to_pandas()["k"]))
    assert (x["med"] == y["med"]).all() and (x["n"] == y["n"]).all() and (x["nu"] == y["nu"]).all()
    assert np.abs(x["p90"].values - y["p90"].values).max() <= 1e-9
    f.close()


def test_set_union_after_a_shuffle(client):
    f = DaskFrame.from_function(make_keyed, range(4), client)
    got = sorted_pd(f.shuffle("k", 4).map_partitions(per_group_sets).to_frame(), ["k"])
    whole = sorted_pd(per_group_sets(f.to_frame()), ["k"])
    assert len(got) == len(whole)
    assert all(set(a.split("\x1e")) == set(b.split("\x1e")) for a, b in zip(got["u"], whole["u"]))
    f.close()


def make_dim(_):
    r = np.random.default_rng(99)
    return DataFrame.from_pandas(
        pd.DataFrame({"k": np.arange(100_000), "w": r.integers(0, 10, 100_000)})
    )


def make_fact(seed):
    r = np.random.default_rng(seed)
    return DataFrame.from_pandas(
        pd.DataFrame({"k": r.integers(0, 100_000, 300_000), "v": r.integers(0, 100, 300_000)})
    )


@pytest.mark.parametrize("strategy", ["broadcast", "shuffle"])
def test_join_equals_the_single_frame_join(client, strategy):
    fact = DaskFrame.from_function(make_fact, range(8), client)
    dim = DaskFrame.from_function(make_dim, [0], client)
    got = fact.join(dim, on="k", strategy=strategy).to_frame()
    whole = fact.to_frame().join(dim.to_frame(), on="k")
    assert len(got) == len(whole) == 2_400_000
    gp, wp = got.to_pandas(), whole.to_pandas()
    assert int((gp.v * gp.w).sum()) == int((wp.v * wp.w).sum())
    fact.close()


@pytest.mark.valgrind
@pytest.mark.parametrize("strategy", ["broadcast", "shuffle"])
def test_left_join_keeps_unmatched_rows(client, strategy):
    left = DaskFrame.from_function(make_small, range(3), client)
    right = DataFrame.from_pandas(pd.DataFrame({"a": [0, 1, 2], "tag": ["p", "q", "r"]}))
    got = left.join(right, on="a", how="left", strategy=strategy).to_frame()
    whole = left.to_frame().join(right, on="a", how="left")
    assert len(got) == len(whole) == 3000
    assert (
        int(got.to_pandas()["tag"].isna().sum()) == int(whole.to_pandas()["tag"].isna().sum()) > 0
    )
    left.close()


def test_broadcast_rejects_a_kind_it_cannot_do():
    left = DaskFrame.from_function(make_small, range(2))
    with pytest.raises(ValueError, match="shuffle"):
        left.join(make_small(0), on="a", how="outer", strategy="broadcast")


def roll(frame):
    return frame.with_column("r", frame["x"].rolling(5).mean())


def diff1(frame):
    return frame.with_column("r", frame["x"].diff())


def shift1(frame):
    return frame.with_column("r", frame["x"].shift(1))


@pytest.mark.valgrind
@pytest.mark.parametrize(("fn", "k"), [(roll, 4), (diff1, 1), (shift1, 1)])
def test_overlap_makes_seams_exact(client, fn, k):
    f = DaskFrame.from_function(make_series, [1, 2], client)
    whole = fn(f.to_frame()).to_pandas()["r"].fillna(-1e9).values
    fixed = f.map_partitions(fn, overlap=k).to_pandas()["r"].fillna(-1e9).values
    naive = f.map_partitions(fn).to_pandas()["r"].fillna(-1e9).values
    # the first partition has no previous one, so its leading nulls are real in both
    assert np.allclose(fixed[:1000], whole[:1000]) and np.allclose(fixed, whole)
    assert not np.allclose(naive, whole)
    f.close()


def boom(_frame):
    raise ValueError("bad row")


def test_a_worker_error_reaches_the_caller(client):
    f = DaskFrame.from_function(make_small, range(2), client)
    with pytest.raises(ValueError, match="bad row"):
        f.map_partitions(boom).to_frame()
    f.close()


@pytest.mark.valgrind
def test_an_empty_partition_keeps_its_types(client):
    low = DataFrame.from_pandas(pd.DataFrame({"a": [1, 2], "x": [10, 20]}))
    high = DataFrame.from_pandas(pd.DataFrame({"a": [7, 8, 8], "x": [30, 40, 50]}))
    f = DaskFrame.from_frames([low, high], client)
    out = f.map_partitions(lambda p: p.filter(col("a") >= 5))  # the first partition is now empty
    parts = out._gather(out._parts)
    assert [len(p) for p in parts] == [0, 3]
    assert out.to_frame().dtypes == high.dtypes
    got = out.group_by("a").agg(n=col("x").count(), s=col("x").sum()).to_frame()
    assert sorted_pd(got, ["a"]).to_dict("list") == {"a": [7, 8], "n": [1, 2], "s": [30, 90]}
    f.close()


def test_close_releases_the_partitions(client):
    f = DaskFrame.from_function(make_small, range(2), client)
    f.close()
    assert f.npartitions == 0
    with pytest.raises(ValueError):
        f.to_frame()


def test_both_modes_agree(cluster):
    local = DaskFrame.from_function(make_events, range(2))
    dist = DaskFrame.from_function(make_events, range(2), cluster)
    a = local.group_by("name").agg(s=col("dur").sum(), v=("dur", "var")).to_frame()
    b = dist.group_by("name").agg(s=col("dur").sum(), v=("dur", "var")).to_frame()
    assert_same(a, b, ["name"], rtol=1e-12)
    dist.close()


def _two_pass_var(values):
    """The corrected two-pass sample variance of a float array (the reference)."""
    import math

    v = np.asarray(values, dtype=np.float64)
    n = len(v)
    d = v - math.fsum(v) / n
    s1, s2 = d.sum(), (d * d).sum()
    return (s2 - s1 * (s1 / n)) / (n - 1)


@pytest.mark.parametrize("offset", [1e9, 1e12])
def test_agg_var_std_are_accurate_with_a_large_offset(client, offset):
    # Two groups at the offset, split over uneven partitions: a single value, an empty
    # partition and a partition that holds only one of the groups. The merge of
    # (n, mean, M2) must equal the one-pass value to a relative 1e-9 on any partitioning.
    rng = np.random.default_rng(12)
    n = 400_000
    g = rng.integers(0, 2, n)
    x = offset + rng.normal(0.0, 1.0, n)
    cuts = [0, 1, 50_000, 50_001, 180_000, 399_999, n]

    def part(a, b):
        return DataFrame.from_dict({"g": g[a:b].tolist(), "x": x[a:b].tolist()})

    parts = [part(a, b) for a, b in zip(cuts, cuts[1:])]
    parts.append(parts[0].head(0))
    f = DaskFrame.from_frames(parts, client)
    out = (
        f.group_by("g").agg(v=("x", "var"), sd=("x", "std")).to_frame().to_pandas().sort_values("g")
    )
    for row in out.itertuples():
        want = _two_pass_var(x[g == row.g])
        assert abs(row.v - want) / want < 1e-9
        assert abs(row.sd - want**0.5) / want**0.5 < 1e-9
    f.close()


@pytest.mark.valgrind
def test_agg_var_of_a_group_with_one_value_is_null(client):
    f = DaskFrame.from_frames(
        [
            DataFrame.from_dict({"g": ["a", "b", "b"], "x": [5.0, 1.0, 3.0]}),
            DataFrame.from_dict({"g": ["b"], "x": [2.0]}),
        ],
        client,
    )
    out = (
        f.group_by("g")
        .agg(v=("x", "var"))
        .to_frame()
        .to_pandas()
        .sort_values("g")
        .reset_index(drop=True)
    )
    assert pd.isna(out["v"][0]) and out["v"][1] == pytest.approx(1.0)  # b: 1, 3, 2
    f.close()


# ---------------------------------------------------------------------------
# distributed-frame-extensions: join kinds, forward overlap, exact aggregates,
# a two-stage shuffle, the nulls_equal pass-through and near-constant variance.
# ---------------------------------------------------------------------------


def make_left(i):
    """Keys i*1000 .. i*1000 + 999: three of these cover keys 0 .. 2999."""
    return DataFrame.from_pandas(
        pd.DataFrame({"k": np.arange(i * 1000, (i + 1) * 1000), "a": np.arange(1000) + i * 1000})
    )


def make_table():
    """Keys 1500 .. 4499, so 1500 keys match the left side and 1500 are only here."""
    return DataFrame.from_pandas(pd.DataFrame({"k": np.arange(1500, 4500), "b": np.arange(3000)}))


def count_submits(monkeypatch):
    """Count every task a DaskFrame submits (in process, one ``_submit`` call is one task)."""
    calls = []
    orig = DaskFrame._submit

    def counting(self, fn, *args):
        calls.append(fn)
        return orig(self, fn, *args)

    monkeypatch.setattr(DaskFrame, "_submit", counting)
    return calls


@pytest.mark.parametrize("how", ["inner", "left", "right", "outer"])
def test_shuffle_join_kinds_equal_the_single_frame_join(client, how):
    left = DaskFrame.from_function(make_left, range(3), client)
    table = make_table()
    got = left.join(table, on="k", how=how, strategy="shuffle").to_frame()
    want = left.to_frame().join(table, on="k", how=how)
    assert_same(got, want, ["k"])
    left.close()


def test_outer_join_keeps_unmatched_rows_of_both_sides(client):
    left = DaskFrame.from_function(make_left, range(3), client)
    got = left.join(make_table(), on="k", how="outer", strategy="shuffle").to_frame().to_pandas()
    assert sorted(got["k"]) == list(range(4500))
    assert int(got["a"].isna().sum()) == 1500 and int(got["b"].isna().sum()) == 1500
    left.close()


@pytest.mark.parametrize("how", ["right", "outer"])
def test_auto_strategy_picks_the_shuffle_for_right_and_outer(client, how):
    left = DaskFrame.from_function(make_left, range(3), client)
    table = make_table()
    for other in (table, DaskFrame.from_frames([table], client)):
        got = left.join(other, on="k", how=how).to_frame()
        assert_same(got, left.to_frame().join(table, on="k", how=how), ["k"])
    left.close()


@pytest.mark.parametrize("how", ["right", "outer"])
def test_broadcast_refuses_right_and_outer_before_any_task(monkeypatch, how):
    left = DaskFrame.from_function(make_left, range(2))
    calls = count_submits(monkeypatch)
    with pytest.raises(ValueError, match=r"unmatched.*strategy='shuffle'"):
        left.join(make_table(), on="k", how=how, strategy="broadcast")
    assert calls == []


def lead2(frame):
    return frame.with_column("r", frame["x"].shift(-2))


def centered5(frame):
    x = frame["x"]
    return frame.with_column("r", (x.shift(2) + x.shift(1) + x + x.shift(-1) + x.shift(-2)) / 5)


def three_series_parts(client):
    rng = np.random.default_rng(7)
    parts = [DataFrame.from_pandas(pd.DataFrame({"x": rng.normal(0, 1, 500)})) for _ in range(3)]
    return DaskFrame.from_frames(parts, client)


@pytest.mark.valgrind
def test_overlap_next_makes_a_lead_exact_across_seams(client):
    f = three_series_parts(client)
    want = lead2(f.to_frame()).to_pandas()
    got = f.map_partitions(lead2, overlap_next=2).to_frame().to_pandas()
    pd.testing.assert_frame_equal(got, want)
    # Without the overlap the last two rows of the first two partitions are wrong (null).
    plain = f.map_partitions(lead2).to_frame().to_pandas()
    assert not plain.equals(want)
    f.close()


def test_overlap_next_with_overlap_gives_a_centered_window(client):
    f = three_series_parts(client)
    want = centered5(f.to_frame()).to_pandas()
    got = f.map_partitions(centered5, overlap=2, overlap_next=2).to_frame().to_pandas()
    pd.testing.assert_frame_equal(got, want)
    f.close()


def test_overlap_rejects_a_negative_size():
    f = DaskFrame.from_frames([make_small(0)])
    with pytest.raises(ValueError, match="0 or more"):
        f.map_partitions(lambda x: x, overlap_next=-1)


def exact_want(whole):
    g = whole.group_by("k")
    out = g.agg(tot=col("x").sum()).to_pandas().set_index("k")
    out["med"] = g.quantile(0.5).to_pandas().set_index("k")["x"]
    out["p90"] = g.quantile(0.9).to_pandas().set_index("k")["x"]
    nu = whole.select("k", "y").drop_duplicates().group_by("k").agg(nu=col("y").count())
    out["nu"] = nu.to_pandas().set_index("k")["nu"]
    return out.reset_index().sort_values("k").reset_index(drop=True)


def test_exact_agg_runs_median_quantile_nunique_and_sum_in_one_call(client):
    f = DaskFrame.from_function(make_keyed, range(8), client)
    got = (
        f.group_by("k")
        .agg(
            exact=True,
            med=("x", "median"),
            p90=("x", "quantile", 0.9),
            nu=("y", "nunique"),
            tot=col("x").sum(),
        )
        .to_frame()
        .to_pandas()
        .sort_values("k")
        .reset_index(drop=True)
    )
    want = exact_want(f.to_frame())
    assert len(got) == len(want) == len(set(want["k"]))
    for c in ("med", "p90", "tot"):
        assert np.abs(got[c].to_numpy(float) - want[c].to_numpy(float)).max() <= 1e-9
    assert (got["nu"].to_numpy() == want["nu"].to_numpy()).all()
    f.close()


def test_exact_agg_accepts_a_quantile_agg_and_stays_partitioned(client):
    f = DaskFrame.from_function(make_keyed, range(3), client)
    out = f.group_by("k").agg(exact=True, q=col("x").quantile(0.25))
    assert out.npartitions == f.npartitions
    got = out.to_frame().to_pandas().sort_values("k").reset_index(drop=True)
    want = f.to_frame().group_by("k").quantile(0.25).to_pandas().sort_values("k")
    assert np.abs(got["q"].to_numpy(float) - want["x"].to_numpy(float)).max() <= 1e-9
    f.close()


@pytest.mark.valgrind
def test_exact_agg_set_union_equals_the_whole(client):
    f = DaskFrame.from_function(lambda i: make_keyed(i, 20_000, 500), range(4), client)
    got = f.group_by("k").agg(exact=True, u=("s", "set_union")).to_frame().to_pandas()
    want = f.to_frame().group_by("k").agg(u=col("s").set_union()).to_pandas()
    a = {r.k: set(r.u.split("\x1e")) for r in got.itertuples()}
    b = {r.k: set(r.u.split("\x1e")) for r in want.itertuples()}
    assert a == b
    f.close()


def test_agg_without_exact_is_still_refused_and_says_how():
    f = DaskFrame.from_function(make_small, range(2))
    with pytest.raises(ValueError, match=r"median.*shuffle.*exact=True"):
        f.group_by("a").agg(m=("x", "median"))
    with pytest.raises(ValueError, match="needs its level"):
        f.group_by("a").agg(exact=True, q=("x", "quantile"))


def keyed_small(i):
    r = np.random.default_rng(i)
    return DataFrame.from_pandas(
        pd.DataFrame({"k": r.integers(0, 5000, 200), "v": r.integers(0, 100, 200)})
    )


def sorted_rows(frame):
    pdf = frame.to_pandas()
    return pdf.sort_values(list(pdf.columns)).reset_index(drop=True)


@pytest.mark.parametrize("m", [64, 128])
def test_two_stage_shuffle_equals_one_stage_with_fewer_tasks(monkeypatch, m):
    f = DaskFrame.from_frames([keyed_small(i) for i in range(m)])
    calls = count_submits(monkeypatch)
    one = f.shuffle("k", m, stages=1)
    tasks_one = len(calls)
    calls.clear()
    two = f.shuffle("k", m, stages=2)
    tasks_two = len(calls)
    calls.clear()
    auto = f.shuffle("k", m)  # m * m is above the threshold: two stages
    assert len(calls) == tasks_two < tasks_one
    assert tasks_one == m + m * m + m
    for a, b, c in zip(one._parts, two._parts, auto._parts):
        pd.testing.assert_frame_equal(sorted_rows(a), sorted_rows(b))
        pd.testing.assert_frame_equal(sorted_rows(b), sorted_rows(c))
    assert sum(len(p) for p in two._parts) == 200 * m
    print(f"shuffle {m}x{m}: one stage {tasks_one} tasks, two stages {tasks_two} tasks")


def test_small_shuffles_stay_one_stage(monkeypatch):
    f = DaskFrame.from_frames([keyed_small(i) for i in range(8)])
    calls = count_submits(monkeypatch)
    f.shuffle("k", 8)
    assert len(calls) == 8 + 8 * 8 + 8  # 64 splits: below the threshold


def test_two_stage_shuffle_on_a_cluster_equals_in_process(cluster):
    parts = [keyed_small(i) for i in range(64)]
    local = DaskFrame.from_frames(parts).shuffle("k", 64, stages=2)
    dist = DaskFrame.from_frames(parts, cluster).shuffle("k", 64, stages=2)
    for a, b in zip(local._parts, dist._gather(dist._parts)):
        pd.testing.assert_frame_equal(sorted_rows(a), sorted_rows(b))
    dist.close()


def test_join_of_a_one_stage_side_with_a_two_stage_side_is_correct(client):
    left = DaskFrame.from_function(keyed_small, range(16), client)
    right = DaskFrame.from_function(lambda i: keyed_small(100 + i).select("k"), range(16), client)
    a = left.shuffle("k", 8, stages=1)
    b = right.shuffle("k", 8, stages=2)
    got = a._like([a._submit(_join, x, y, "k", "inner", {}) for x, y in zip(a._parts, b._parts)])
    want = left.to_frame().join(right.to_frame(), on="k", how="inner")
    pd.testing.assert_frame_equal(sorted_rows(got.to_frame()), sorted_rows(want))
    left.close()
    right.close()


def test_stages_must_be_one_or_two_and_two_needs_four_outputs():
    f = DaskFrame.from_frames([keyed_small(0), keyed_small(1)])
    with pytest.raises(ValueError, match="1 or 2"):
        f.shuffle("k", 4, stages=3)
    with pytest.raises(ValueError, match="at least 4"):
        f.shuffle("k", 3, stages=2)


HAS_NULLS_EQUAL = "nulls_equal" in inspect.signature(DataFrame.join).parameters


@pytest.mark.skipif(HAS_NULLS_EQUAL, reason="the engine's join has the nulls_equal option")
def test_nulls_equal_without_the_engine_option_raises_before_any_task(monkeypatch):
    left = DaskFrame.from_function(make_left, range(2))
    calls = count_submits(monkeypatch)
    with pytest.raises(NotImplementedError, match="nulls_equal"):
        left.join(make_table(), on="k", nulls_equal=True)
    assert calls == []


def make_null_keys(i):
    keys = ["a", None, "b", None, "c"] if i == 0 else ["b", None, "d"]
    return DataFrame.from_dict({"k": keys, "v": list(range(len(keys)))})


@pytest.mark.skipif(
    not HAS_NULLS_EQUAL,
    reason="DataFrame.join has no nulls_equal option yet (change join-null-keys)",
)
@pytest.mark.parametrize("strategy", ["broadcast", "shuffle"])
@pytest.mark.parametrize("nulls_equal", [True, False])
def test_nulls_equal_is_passed_to_the_engine_join(client, strategy, nulls_equal):
    left = DaskFrame.from_function(make_null_keys, range(2), client)
    right = DataFrame.from_dict({"k": ["b", None, "z"], "w": [1, 2, 3]})
    got = left.join(right, on="k", how="inner", strategy=strategy, nulls_equal=nulls_equal)
    want = left.to_frame().join(right, on="k", how="inner", nulls_equal=nulls_equal)  # type: ignore
    assert_same(got.to_frame(), want, ["v", "w"])
    left.close()


def test_agg_std_var_of_near_constant_data_across_four_partitions_near_constant(client):
    # 1e9 + N(0, 1): the spread is a billionth of the mean. A sum-of-squares merge gives NaN or
    # garbage here; the (n, mean, M2) merge stays within a relative 1e-9 of the two-pass value.
    rng = np.random.default_rng(0)
    x = 1e9 + rng.normal(0.0, 1.0, 4_000_000)
    parts = [
        DataFrame.from_dict(
            {"g": ["a"] * 1_000_000, "x": x[i * 1_000_000 : (i + 1) * 1_000_000].tolist()}
        )
        for i in range(4)
    ]
    f = DaskFrame.from_frames(parts, client)
    out = f.group_by("g").agg(v=("x", "var"), sd=("x", "std")).to_frame().to_pandas()
    want = _two_pass_var(x)
    assert not np.isnan(out["v"][0]) and not np.isnan(out["sd"][0])
    assert abs(out["v"][0] - want) / want < 1e-9
    assert abs(out["sd"][0] - want**0.5) / want**0.5 < 1e-9
    f.close()


# ---- a frame with no partitions is refused by name


def _empty():
    return DaskFrame.from_frames([])


def _one():
    return DaskFrame.from_frames([DataFrame.from_dict({"k": [1, 2], "v": [3, 4]})])


@pytest.mark.parametrize(
    "name, call",
    [
        ("reduce", lambda e: e.reduce(lambda f: f, lambda a: a[0])),
        ("group_by().agg", lambda e: e.group_by("k").agg(s=("v", "sum"))),
        ("shuffle", lambda e: e.shuffle(["k"], 2)),
        ("join", lambda e: e.join(_one(), on="k")),
        ("map_partitions", lambda e: e.map_partitions(lambda f: f)),
    ],
)
def test_no_partitions_is_refused_by_name(name, call):
    with pytest.raises(ValueError, match=name.replace("(", r"\(").replace(")", r"\)")) as e:
        call(_empty())
    assert "no partitions" in str(e.value)


def test_no_partitions_join_other_side_is_refused():
    with pytest.raises(ValueError, match="join") as e:
        _one().join(_empty(), on="k")
    assert "no partitions" in str(e.value)


def test_no_partitions_npartitions_and_close():
    e = _empty()
    assert e.npartitions == 0
    e.close()
