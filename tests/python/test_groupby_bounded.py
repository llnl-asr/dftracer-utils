"""Many-group aggregation over long string keys: the results do not depend on how the rows are
streamed into the groups (blocks of rows, column batches)."""

import os

import numpy as np
import pandas as pd

from dftracer.utils import DataFrame, col

N = 1_200_000  # past the sizes where the many-group routes and the column batches begin
GROUPS = 5_000


def frame(n=N, groups=GROUPS, ncols=6, seed=0):
    rng = np.random.default_rng(seed)
    names = np.array([f"application-process-name-{i:06d}" for i in range(groups)], dtype=object)
    data = {"key": names[rng.integers(0, groups, n)], "t": rng.integers(0, 4, n)}
    for i in range(ncols):
        v = rng.normal(1e6, 3.0, n) if i % 2 == 0 else rng.integers(0, 1000, n).astype("float64")
        v[rng.random(n) < 0.05] = np.nan
        data[f"v{i}"] = v
    return pd.DataFrame(data)


def ours(pdf, aggs, keys=("key", "t")):
    return DataFrame.from_pandas(pdf).group_by(list(keys)).agg(**aggs).to_pandas()


def test_blocks_of_rows_keep_first_last_and_the_group_order():
    n = 400_000
    pdf = frame(n=n, groups=GROUPS, ncols=1)
    got = ours(pdf, {"f": col("v0").first(), "l": col("v0").last(), "n": col("v0").count()})
    want = (
        pdf.groupby(["key", "t"], sort=False)
        .agg(f=("v0", "first"), l=("v0", "last"), n=("v0", "count"))
        .reset_index()
    )
    assert list(got["key"]) == list(want["key"])
    assert list(got["t"]) == list(want["t"])
    assert np.array_equal(got["f"].to_numpy(dtype="float64"), want["f"].to_numpy(), equal_nan=True)
    assert np.array_equal(got["l"].to_numpy(dtype="float64"), want["l"].to_numpy(), equal_nan=True)
    assert got["n"].tolist() == want["n"].tolist()


def test_column_batches_equal_pandas_and_keep_the_column_order():
    pdf = frame()
    cols = [c for c in pdf.columns if c.startswith("v")]
    aggs = {}
    for c in cols:
        aggs[f"{c}_mean"] = col(c).mean()
        aggs[f"{c}_std"] = col(c).std()
        aggs[f"{c}_count"] = col(c).count()
    got = ours(pdf, aggs, keys=("key",))
    want = (
        pdf.groupby("key", sort=False)
        .agg(**{f"{c}_{h}": (c, h) for c in cols for h in ("mean", "std", "count")})
        .reset_index()
    )
    assert list(got.columns) == ["key"] + list(aggs)
    assert list(got["key"]) == list(want["key"])
    for name in aggs:
        g, w = got[name].to_numpy(dtype="float64"), want[name].to_numpy(dtype="float64")
        assert np.allclose(g, w, rtol=1e-9, atol=0, equal_nan=True), name


def test_a_big_mean_keeps_a_small_std():
    pdf = frame(ncols=1)
    got = ours(pdf, {"s": col("v0").std()}, keys=("key",))
    want = pdf.groupby("key", sort=False)["v0"].std()
    assert np.allclose(got["s"].to_numpy(dtype="float64"), want.to_numpy(), rtol=1e-9)


def test_rows_with_a_null_key_are_left_out_as_in_pandas():
    pdf = frame(n=400_000, ncols=1)
    pdf.loc[pdf.index % 7 == 0, "key"] = None
    got = ours(pdf, {"n": col("v0").count()}, keys=("key",))
    want = pdf.groupby("key", sort=False)["v0"].count()
    assert list(got["key"]) == list(want.index)
    assert got["n"].tolist() == want.tolist()


def _bits(frame_):
    """Every column as bytes, so equality is by bits and not by tolerance."""
    return [
        frame_[c].to_numpy(dtype="float64", na_value=np.nan).tobytes()
        if frame_[c].dtype.kind in "fiu" or str(frame_[c].dtype) in ("Float64", "Int64")
        else tuple(frame_[c])
        for c in frame_.columns
    ]


def test_the_same_group_by_gives_the_same_bits_on_every_run():
    import hashlib

    rng = np.random.default_rng(5)
    n = 700_000
    for label, groups in (("many groups", 150_000), ("few groups", 40)):
        keys = np.array([f"proc#{i:07d}#{i:07d}" for i in range(groups)], dtype=object)[
            rng.integers(0, groups, n)
        ]
        pdf = pd.DataFrame({"k": keys, "x": rng.normal(1e3, 7, n), "y": rng.integers(0, 1000, n)})
        df = DataFrame.from_pandas(pdf)
        seen = set()
        for _ in range(8):
            got = (
                df.group_by("k")
                .agg(
                    s=col("x").sum(),
                    m=col("x").mean(),
                    sd=col("x").std(),
                    c=col("x").count(),
                    ys=col("y").sum(),
                )
                .to_pandas()
            )
            seen.add(
                hashlib.md5(
                    b"".join(b if isinstance(b, bytes) else repr(b).encode() for b in _bits(got))
                ).hexdigest()
            )
        assert len(seen) == 1, f"{label}: {len(seen)} different results in 8 runs"


def test_moment_cells_give_the_bits_of_the_general_state():
    # std/var alone ride in the packed moment words; a skew beside them makes the
    # state a general FieldStat one. Their std, var, mean and sum must be the same bits.
    pdf = frame(n=300_000, groups=2_000, ncols=4)
    pdf["i0"] = np.random.default_rng(3).integers(-5_000, 5_000, len(pdf))
    pdf.loc[::11, "i0"] = pd.NA
    pdf["i0"] = pdf["i0"].astype("Int64")
    cols = ["v0", "v1", "v2", "i0"]
    packed = {}
    general = {}
    for c in cols:
        for name, agg in (
            ("sd", col(c).std()),
            ("va", col(c).var()),
            ("me", col(c).mean()),
            ("su", col(c).sum()),
            ("mn", col(c).min()),
            ("mx", col(c).max()),
            ("n", col(c).count()),
        ):
            packed[f"{c}_{name}"] = agg
            general[f"{c}_{name}"] = agg
    general["skew_extra"] = col("v3").skew()
    a = ours(pdf, packed, keys=("key",))
    b = ours(pdf, general, keys=("key",))
    for name in packed:
        x = a[name].to_numpy(dtype="float64", na_value=np.nan)
        y = b[name].to_numpy(dtype="float64", na_value=np.nan)
        assert x.tobytes() == y.tobytes(), name
    pd_sd = pdf.groupby("key", sort=False)["v0"].std().to_numpy()
    assert np.allclose(a["v0_sd"].to_numpy(dtype="float64"), pd_sd, rtol=1e-6, equal_nan=True)


def _both(pdf, aggs, keys, **kw):
    """The group-by in place and with the stitch path forced: the oracle."""
    df = DataFrame.from_pandas(pdf)
    got = df.group_by(list(keys), **kw).agg(**aggs)
    os.environ["DFTRACER_UTILS_GROUPBY_STITCH"] = "1"
    try:
        ref = df.group_by(list(keys), **kw).agg(**aggs)
    finally:
        del os.environ["DFTRACER_UTILS_GROUPBY_STITCH"]
    return got, ref


def _same(got, ref):
    assert got.columns == ref.columns
    assert [str(d) for d in got.dtypes] == [str(d) for d in ref.dtypes]
    for name in got.columns:
        # repr keeps the sign of a zero and a NaN: a bit-level comparison of the values and the nulls
        assert repr(got[name].to_list()) == repr(ref[name].to_list()), name
        assert got[name].null_count == ref[name].null_count, name


def _analyzer_aggs(cols):
    return {
        f"{c}_{a}": getattr(col(c), a)()
        for c in cols
        for a in ("sum", "min", "max", "mean", "std", "var", "count")
    }


def test_in_place_finalize_equals_the_stitch_path_on_long_string_keys():
    pdf = frame(n=600_000, groups=40_000, ncols=4, seed=3)
    got, ref = _both(pdf, _analyzer_aggs(["v0", "v1", "v2", "v3"]), ("key", "t"))
    assert len(got) > 40_000
    _same(got, ref)


def test_in_place_finalize_equals_the_stitch_path_on_integer_keys_and_null_keys():
    pdf = frame(n=600_000, groups=40_000, ncols=3, seed=4)
    pdf["ikey"] = pd.array(np.arange(len(pdf)) % 30_000, dtype="Int64")
    pdf.loc[::97, "ikey"] = pd.NA
    pdf["skey"] = pdf["key"].astype(object)
    pdf.loc[::89, "skey"] = None
    for keys in (("ikey",), ("skey", "t")):
        for dropna in (True, False):
            got, ref = _both(pdf, _analyzer_aggs(["v0", "v1", "v2"]), keys, dropna=dropna)
            _same(got, ref)


def test_in_place_finalize_falls_back_for_text_and_list_aggregates():
    pdf = frame(n=600_000, groups=40_000, ncols=2, seed=5)
    pdf["s"] = np.array(["p", "q", "r"], dtype=object)[np.arange(len(pdf)) % 3]
    aggs = {
        "v0_sum": col("v0").sum(),
        "f": col("s").first(),
        "u": col("s").set_union(),
        "v1_mean": col("v1").mean(),
    }
    got, ref = _both(pdf, aggs, ("key", "t"))
    _same(got, ref)
