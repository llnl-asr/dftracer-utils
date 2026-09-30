"""A group-by over many groups on a long string key or an integer key, large
enough for the hash-partitioned driver, equals pandas in values and in the
order the groups first appear."""

import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame, col

ROWS = 400_000  # above the partitioned drivers' row floor
GROUPS = 60_000  # above their probe's group floor


def make(key_kind: str, seed: int = 3) -> pd.DataFrame:
    rng = np.random.default_rng(seed)
    procs = GROUPS // 10
    if key_kind == "string":
        p = rng.integers(0, procs, ROWS)
        # 18 bytes and longer, past the packed driver's 16-byte string limit
        k1 = pd.array([f"app#host#{i}#{i}" for i in p], dtype="string")
    else:
        k1 = rng.integers(0, procs, ROWS) * 1_000_003
    x = pd.array(rng.random(ROWS) * 100, dtype="Float64")
    x[rng.random(ROWS) < 0.05] = pd.NA
    y = pd.array(rng.integers(-500, 500, ROWS), dtype="Int64")
    y[rng.random(ROWS) < 0.05] = pd.NA
    return pd.DataFrame({"k1": k1, "k2": rng.integers(0, 10, ROWS), "x": x, "y": y})


@pytest.mark.parametrize("key_kind", ["string", "int"])
def test_many_groups_equal_pandas(key_kind):
    d = make(key_kind)
    keys = ["k1", "k2"]
    want = (
        d.groupby(keys, sort=False)
        .agg(
            n=("x", "count"),
            sx=("x", "sum"),
            mx=("x", "mean"),
            lox=("x", "min"),
            hix=("x", "max"),
            sdx=("x", "std"),
            sy=("y", "sum"),
            loy=("y", "min"),
            hiy=("y", "max"),
        )
        .reset_index()
    )
    got = (
        DataFrame.from_pandas(d)
        .group_by(keys)
        .agg(
            n=col("x").count(),
            sx=col("x").sum(),
            mx=col("x").mean(),
            lox=col("x").min(),
            hix=col("x").max(),
            sdx=col("x").std(),
            sy=col("y").sum(),
            loy=col("y").min(),
            hiy=col("y").max(),
        )
        .to_pandas(nullable=True)
    )
    assert len(got) == len(want) > 50_000
    # same groups in the same first-seen order
    assert got["k1"].astype(str).tolist() == want["k1"].astype(str).tolist()
    assert got["k2"].astype("int64").tolist() == want["k2"].astype("int64").tolist()
    for c in ["n", "sx", "mx", "lox", "hix", "sdx", "sy", "loy", "hiy"]:
        a, b = got[c].astype("float64").to_numpy(), want[c].astype("float64").to_numpy()
        assert np.array_equal(np.isnan(a), np.isnan(b)), c
        m = ~np.isnan(a)
        assert np.allclose(a[m], b[m], rtol=1e-9, atol=1e-9), c
