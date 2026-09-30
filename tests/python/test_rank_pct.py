import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame


def series(values):
    return DataFrame.from_dict({"x": values})["x"]


def test_percentile_rank():
    assert series([10, 30, 20]).rank(pct=True).to_list() == pytest.approx([1 / 3, 1.0, 2 / 3])


def test_nulls_are_not_counted():
    out = series([10, None, 20]).rank(pct=True).to_list()
    assert out == [0.5, None, 1.0]


def test_default_is_unchanged():
    assert series([10, 30, 20]).rank().to_list() == [1.0, 3.0, 2.0]


def test_dense_divides_by_the_distinct_count():
    out = series([10, 30, 20, 20, None, 10]).rank(method="dense", pct=True).to_list()
    assert out[:4] == pytest.approx([1 / 3, 1.0, 2 / 3, 2 / 3])
    assert out[4] is None
    assert out[5] == pytest.approx(1 / 3)


@pytest.mark.parametrize("method", ["average", "min", "max", "dense", "first"])
@pytest.mark.parametrize("ascending", [True, False])
def test_matches_pandas_for_every_method(method, ascending):
    rng = np.random.default_rng(3)
    vals = rng.integers(0, 12, 200).astype(float)
    vals[::13] = np.nan
    ours = DataFrame.from_pandas(pd.DataFrame({"x": vals}))["x"]
    got = ours.rank(method=method, ascending=ascending, pct=True).to_list()
    want = pd.Series(vals).rank(method=method, ascending=ascending, pct=True).tolist()
    for g, w in zip(got, want):
        if w != w:
            assert g is None or g != g
        else:
            assert g == pytest.approx(w)


def test_percentile_rank_is_in_zero_one():
    out = series([5, 5, 5]).rank(pct=True).to_list()
    assert all(0 < v <= 1 for v in out)
