"""DataFrame.eval lowers the pandas method calls the analyzer's presets use, and
agrees with pandas eval on the preset strings."""

import math

import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame
from dftracer.utils._transpile import TranspileError

# derived_metrics and layer conditions of the analyzer's shipped presets
POSIX = [
    "io_cat == 1 or io_cat == 2",
    "io_cat == 1",
    "io_cat == 2",
    "io_cat == 3",
    'io_cat == 3 and func_name.str.contains("close") and ~func_name.str.contains("dir")',
    'io_cat == 3 and func_name.str.contains("open") and ~func_name.str.contains("dir")',
    'io_cat == 3 and func_name.str.contains("seek")',
    'io_cat == 3 and func_name.str.contains("stat")',
    "io_cat == 6",
    "io_cat == 7",
]
DLIO = [
    'func_name.str.contains("init")',
    'func_name.str.contains(".close")',
    'func_name.str.contains(".open")',
    'func_name.str.contains(".get_sample")',
]
LAYERS = [
    'cat.str.contains("posix|stdio")',
    'cat.str.contains("posix|stdio") & cat.str.contains("_reader")',
    'cat == "pipeline" & func_name.str.startswith("epoch")',
]
ADDITIONAL = [
    "a.fillna(0) / (b.fillna(0) + 1e-09)",
    "a / (b + 1e-09)",
    "(b.isna() | b == 0)",
    "(a.isna() | a == 0) & (b.isna() | b == 0)",
]


def frame(n=500, seed=0):
    rng = np.random.default_rng(seed)
    names = [
        "read",
        "write",
        "close",
        "close_dir",
        "open",
        "opendir",
        "seek",
        "stat",
        "epoch.1",
        "init",
        "x.close",
        "x.open",
        "y.get_sample",
    ]
    cats = ["posix", "stdio", "posix_reader", "stdio_checkpoint", "pipeline", "compute", "data"]
    io_cat = rng.integers(0, 8, n).astype("float64")
    io_cat[rng.random(n) < 0.1] = np.nan
    a = np.where(rng.random(n) < 0.2, np.nan, rng.normal(5, 3, n))
    b = np.where(rng.random(n) < 0.2, np.nan, rng.normal(2, 2, n))
    b[rng.random(n) < 0.1] = 0.0
    return pd.DataFrame(
        {
            "io_cat": io_cat,
            "func_name": rng.choice(names, n),
            "cat": rng.choice(cats, n),
            "a": a,
            "b": b,
        }
    )


def values(series):
    return [float("nan") if v is None else v for v in series.to_list()]


def mask(series):
    return [bool(v) if v is not None else False for v in series.to_list()]


def same_numbers(got, want, tol=1e-9):
    got, want = np.asarray(values(got), dtype="float64"), np.asarray(want, dtype="float64")
    assert got.shape == want.shape
    assert np.array_equal(np.isnan(got), np.isnan(want))
    ok = ~np.isnan(want)
    assert np.allclose(got[ok], want[ok], rtol=tol, atol=tol)


@pytest.mark.parametrize("text", POSIX + DLIO + LAYERS + ADDITIONAL[2:])
def test_preset_masks_equal_pandas_eval(text):
    pdf = frame()
    got = DataFrame.from_pandas(pdf).eval(text)
    want = pdf.eval(text, engine="python")
    assert mask(got) == [bool(v) if not pd.isna(v) else False for v in want.tolist()]


@pytest.mark.parametrize("text", ADDITIONAL[:2])
def test_additional_metric_ratios_equal_pandas_eval(text):
    pdf = frame()
    same_numbers(DataFrame.from_pandas(pdf).eval(text), pdf.eval(text, engine="python").to_numpy())


def test_fillna_in_a_ratio_has_no_nulls():
    d = DataFrame.from_dict({"a": [1.0, None, 3.0], "b": [2.0, 0.0, None]})
    got = d.eval("a.fillna(0) / (b.fillna(0) + 1e-9)").to_list()
    assert None not in got
    assert got[0] == pytest.approx(1.0 / (2.0 + 1e-9))
    assert got[1] == 0.0


def test_fillna_with_a_column():
    pdf = frame()
    same_numbers(
        DataFrame.from_pandas(pdf).eval("a.fillna(b)"),
        pdf.eval("a.fillna(b)", engine="python").to_numpy(),
    )


@pytest.mark.parametrize(
    "text,want",
    [
        ("a.clip(lower=0)", [0.0, None, 3.0]),
        ("a.clip(upper=0)", [-2.0, None, 0.0]),
        ("a.clip(lower=-1, upper=1)", [-1.0, None, 1.0]),
        ("a.clip(-1, 1)", [-1.0, None, 1.0]),
    ],
)
def test_clip_keeps_nulls_with_one_or_both_bounds(text, want):
    d = DataFrame.from_dict({"a": [-2.0, None, 3.0]})
    assert d.eval(text).to_list() == want


def test_where_without_other_is_null_where_false():
    d = DataFrame.from_dict({"a": [1.0, 2.0, None]})
    assert d.eval("a.where(a > 1)").to_list() == [None, 2.0, None]


def test_where_and_mask_with_other():
    pdf = frame()
    d = DataFrame.from_pandas(pdf)
    same_numbers(
        d.eval("a.where(a > 5, 0)"), pdf.eval("a.where(a > 5, 0)", engine="python").to_numpy()
    )
    same_numbers(
        d.eval("a.mask(a > 5, 0)"), pdf.eval("a.mask(a > 5, 0)", engine="python").to_numpy()
    )
    same_numbers(
        d.eval("a.where(a > 5, other=-1)"),
        pdf.eval("a.where(a > 5, other=-1)", engine="python").to_numpy(),
    )


def test_mask_without_other_is_null_where_true():
    d = DataFrame.from_dict({"a": [1.0, 2.0, None]})
    assert d.eval("a.mask(a > 1)").to_list() == [1.0, None, None]


def test_where_in_an_assignment_leaves_no_helper_column():
    d = DataFrame.from_dict({"a": [1.0, 2.0, None]})
    out = d.eval("m = a.where(a > 1)")
    assert out.columns == ["a", "m"]
    assert out["m"].to_list() == [None, 2.0, None]


def test_abs_and_round_equal_pandas():
    pdf = frame()
    d = DataFrame.from_pandas(pdf)
    # pandas before 3 cannot call a method on a parenthesised expression in
    # eval, so the oracle is numpy on the same columns.
    a, b = pdf["a"].to_numpy(), pdf["b"].to_numpy()
    for text, want in (
        ("(a - b).abs()", np.abs(a - b)),
        ("(a - b).abs().round(2)", np.round(np.abs(a - b), 2)),
        ("a.round(1)", np.round(a, 1)),
        ("a.round()", np.round(a)),
        ("a.round(decimals=3)", np.round(a, 3)),
    ):
        same_numbers(d.eval(text), want, tol=1e-9)


def test_isna_and_notna():
    pdf = frame()
    d = DataFrame.from_pandas(pdf)
    for text in ("a.isna()", "a.notna()", "a.isnull()", "a.notnull()"):
        assert mask(d.eval(text)) == pdf.eval(text, engine="python").tolist()


def test_a_literal_substring_match():
    d = DataFrame.from_dict({"s": ["a.b", "ab", "A.B"]})
    assert d.eval('s.str.contains(".", regex=False)').to_list() == [True, False, True]
    assert d.eval('s.str.contains("a.b", case=False)').to_list() == [
        True,
        False,
        True,
    ]  # "." is a regex dot
    assert d.eval('s.str.contains("a.b")').to_list() == [True, False, False]


def test_an_unknown_keyword_is_named():
    d = DataFrame.from_dict({"a": [1.0]})
    with pytest.raises(TranspileError, match="side"):
        d.eval("a.clip(lower=0, side=1)")
    with pytest.raises(TranspileError, match="lower"):
        d.eval("a.clip(0, lower=1)")
    with pytest.raises(TranspileError, match="bound"):
        d.eval("a.clip()")


def test_a_method_without_an_engine_form_is_named():
    d = DataFrame.from_dict({"proc_name": ["app#host#1#2"]})
    with pytest.raises(TranspileError, match="split"):
        d.eval('proc_name.str.split("#").str[1]')


def test_round_matches_numpy_on_halves():
    vals = [0.125, 0.375, 2.5, 3.5, -0.125, 1.005, 2.675]
    d = DataFrame.from_dict({"a": vals})
    got = d.eval("a.round(2)").to_list()
    want = np.round(np.array(vals), 2).tolist()
    assert all(math.isclose(g, w, rel_tol=1e-12, abs_tol=1e-12) for g, w in zip(got, want))
