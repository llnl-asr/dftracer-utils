"""A join does not match null keys by default (the SQL rule); with
``nulls_equal=True`` it matches them as pandas does."""

import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame
from dftracer.utils.pandas import merge as pd_style_merge


def rows(df):
    """Sorted row tuples, numbers as float and nulls as None, order ignored."""
    out = []
    for r in df.itertuples(index=False):
        out.append(
            tuple(
                None
                if pd.isna(v)
                else (float(v) if isinstance(v, (int, float, np.integer, np.floating)) else v)
                for v in r
            )
        )
    return sorted(out, key=repr)


def proposal_frames():
    left = pd.DataFrame({"k": ["a", None, "c"], "x": [1, 2, 3]})
    right = pd.DataFrame({"k": ["a", None], "y": [10, 20]})
    return left, right


def test_the_proposal_repro():
    left, right = proposal_frames()
    assert len(left.merge(right, on="k")) == 2  # pandas: a and the null key match
    ul, ur = DataFrame.from_pandas(left), DataFrame.from_pandas(right)
    assert len(ul.merge(ur, on="k").to_pandas()) == 1  # the SQL rule: only a
    assert len(ul.merge(ur, on="k", nulls_equal=True).to_pandas()) == 2


@pytest.mark.parametrize("how", ["inner", "left", "right", "outer"])
@pytest.mark.parametrize("key", ["string", "int"])
def test_nulls_equal_gives_the_pandas_rows(how, key):
    rng = np.random.default_rng(7)
    pool = ["a", "b", "c", None] if key == "string" else [1, 2, 3, None]
    lk = [pool[i] for i in rng.integers(0, 4, 60)]
    rk = [pool[i] for i in rng.integers(0, 4, 50)]
    if key == "int":
        left = pd.DataFrame({"k": pd.array(lk, dtype="Int64"), "x": np.arange(60)})
        right = pd.DataFrame({"k": pd.array(rk, dtype="Int64"), "y": np.arange(100, 150)})
    else:
        left = pd.DataFrame({"k": lk, "x": np.arange(60)})
        right = pd.DataFrame({"k": rk, "y": np.arange(100, 150)})
    want = left.merge(right, on="k", how=how)
    got = (
        DataFrame.from_pandas(left)
        .merge(DataFrame.from_pandas(right), on="k", how=how, nulls_equal=True)
        .to_pandas()
    )
    assert list(got.columns) == list(want.columns)
    assert rows(got) == rows(want)


@pytest.mark.parametrize("how", ["inner", "left"])
def test_the_default_drops_null_keys_from_the_match(how):
    left, right = proposal_frames()
    want = left.dropna(subset=["k"]).merge(right.dropna(subset=["k"]), on="k", how=how)
    got = (
        DataFrame.from_pandas(left).merge(DataFrame.from_pandas(right), on="k", how=how).to_pandas()
    )
    matched = got.dropna(subset=["y"]) if how == "left" else got
    assert len(matched) == len(want.dropna(subset=["y"]))


def test_semi_and_anti():
    left, right = proposal_frames()
    ul, ur = DataFrame.from_pandas(left), DataFrame.from_pandas(right)
    semi = ul.join(ur, on="k", how="semi", nulls_equal=True).to_pandas()
    anti = ul.join(ur, on="k", how="anti", nulls_equal=True).to_pandas()
    assert sorted(semi["x"]) == [1, 2]
    assert sorted(anti["x"]) == [3]


def test_a_null_in_one_of_two_keys():
    left = pd.DataFrame({"a": [7, 7], "b": pd.array([None, 1], dtype="Int64"), "x": [1, 2]})
    right = pd.DataFrame({"a": [7], "b": pd.array([None], dtype="Int64"), "y": [10]})
    got = (
        DataFrame.from_pandas(left)
        .merge(DataFrame.from_pandas(right), on=["a", "b"], nulls_equal=True)
        .to_pandas()
    )
    assert rows(got) == rows(left.merge(right, on=["a", "b"]))
    assert len(got) == 1


def test_a_lazy_join_gives_the_eager_rows():
    left, right = proposal_frames()
    ul, ur = DataFrame.from_pandas(left), DataFrame.from_pandas(right)
    for how in ("inner", "left", "right", "outer"):
        eager = ul.join(ur, on="k", how=how, nulls_equal=True).to_pandas()
        lazy = ul.lazy().join(ur.lazy(), on="k", how=how, nulls_equal=True).collect().to_pandas()
        assert rows(lazy) == rows(eager)


def test_the_pandas_style_merge_function():
    left, right = proposal_frames()
    ul, ur = DataFrame.from_pandas(left), DataFrame.from_pandas(right)
    assert len(pd_style_merge(ul, ur, on="k", nulls_equal=True).to_pandas()) == 2
    assert len(pd_style_merge(ul, ur, on="k").to_pandas()) == 1


@pytest.mark.parametrize("how", ["cross", "lookup", "nest"])
def test_cross_lookup_and_nest_refuse_the_option(how):
    left, right = proposal_frames()
    ul, ur = DataFrame.from_pandas(left), DataFrame.from_pandas(right)
    on = None if how == "cross" else "k"
    with pytest.raises(ValueError, match=how):
        ul.join(ur, on=on, how=how, suffix="n", nulls_equal=True)
