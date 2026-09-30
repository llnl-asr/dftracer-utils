"""Reductions over a bool or string column, and the error for a reduction the
engine does not define (openspec change reduce-refuses-unsupported-types)."""

import pytest

from dftracer.utils import DataFrame, col
from dftracer.utils import dftracer_utils_ext as _ext


def _frame():
    return DataFrame.from_dict(
        {
            "flag": [True, False, True, True],
            "name": ["a", "b", "c", "d"],
            "x": [1, 2, 3, 4],
        }
    )


def test_proposal_repro_gives_counts_not_zero():
    d = _frame()
    assert d["flag"].sum() == 3
    assert d["flag"].mean() == pytest.approx(0.75)
    assert (d["x"] > 2).sum() == 2
    assert d["name"].min() == "a"
    with pytest.raises(TypeError, match="sum.*string"):
        d["name"].sum()


def test_bool_reductions_count_true():
    flag = _frame()["flag"]
    assert flag.sum() == 3
    assert flag.min() == 0
    assert flag.max() == 1
    assert flag.mean() == pytest.approx(0.75)


def test_bool_agrees_with_group_by_sum():
    d = _frame()
    grouped = d.group_by("name").agg(s=col("flag").sum()).to_pandas()
    assert grouped["s"].tolist() == [1, 0, 1, 1]
    # one group over the whole column equals the Series reduction
    whole = d.with_columns(k=col("x") * 0).group_by("k").agg(s=col("flag").sum()).to_pandas()
    assert int(whole["s"].iloc[0]) == d["flag"].sum()


def test_comparison_mask_counts_matches():
    d = _frame()
    assert (d["x"] > 2).sum() == 2
    assert (d["x"] >= 1).sum() == 4


def test_string_min_and_max_are_bytewise_extremes():
    s = DataFrame.from_dict({"s": ["b", "a", "d", "c"]})["s"]
    assert s.min() == "a"
    assert s.max() == "d"


def test_string_sum_and_mean_are_refused():
    s = DataFrame.from_dict({"s": ["a", "b"]})["s"]
    with pytest.raises(TypeError, match="sum.*string"):
        s.sum()
    with pytest.raises(TypeError, match="string"):
        s.mean()


def test_refused_reduction_names_the_operation_and_type():
    s = DataFrame.from_dict({"s": ["a"]})["s"]
    with pytest.raises(TypeError) as err:
        s.sum()
    assert "sum" in str(err.value) and "string" in str(err.value)


def test_error_reaches_the_generic_op_runner():
    # ops.run goes through the C ABI runner, which hands back the refusal as an
    # error scalar; it must raise, never return a number.
    s = DataFrame.from_dict({"s": ["a", "b"]})["s"]
    with pytest.raises(TypeError, match="sum.*string"):
        _ext.op_run("dftu.series.reduce", s._native, 1)


def test_numeric_reductions_are_unchanged():
    x = DataFrame.from_dict({"x": [3, 1, 4]})["x"]
    assert x.sum() == 8
    assert x.min() == 1
    assert x.max() == 4
    assert x.mean() == pytest.approx(8 / 3)
    empty = DataFrame.from_dict({"x": [3, 1, 4]}).filter(col("x") > 100)["x"]
    assert empty.sum() == 0
    assert empty.mean() == 0.0
