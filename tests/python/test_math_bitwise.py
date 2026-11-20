# SPDX-License-Identifier: MIT
"""The Polars elementwise math and bitwise methods of Series and Expr: sin to
erfc and cbrt and log10; bitwise_count_ones to bitwise_trailing_zeros and the
bitwise_and, bitwise_or and bitwise_xor reductions. Each runs on every integer
width and on floats in Polars and here; the dtype, the nulls and the values
must agree."""

import math

import pytest

pl = pytest.importorskip("polars")
pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import DataFrame, Series, col  # noqa: E402
from dftracer.utils.enums import DType  # noqa: E402

MATH = [
    "sin", "cos", "tan", "cot", "arcsin", "arccos", "arctan", "sinh", "cosh", "tanh",
    "arcsinh", "arccosh", "arctanh", "cbrt", "log10", "erf", "erfc",
]  # fmt: skip

DTYPES = {
    "int8": (pl.Int8, DType.INT8, 8, True),
    "int16": (pl.Int16, DType.INT16, 16, True),
    "int32": (pl.Int32, DType.INT32, 32, True),
    "int64": (pl.Int64, DType.INT64, 64, True),
    "uint8": (pl.UInt8, DType.UINT8, 8, False),
    "uint16": (pl.UInt16, DType.UINT16, 16, False),
    "uint32": (pl.UInt32, DType.UINT32, 32, False),
    "uint64": (pl.UInt64, DType.UINT64, 64, False),
}

FLOATS = {"float32": (pl.Float32, DType.FLOAT32), "float64": (pl.Float64, DType.FLOAT64)}

EDGE = [
    0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 10.0, 1000.0, 1e-300, 1e300,
    float("inf"), float("-inf"), float("nan"), None,
]  # fmt: skip


def _same(got, want, rel=1e-6):
    assert len(got) == len(want)
    for g, w in zip(got, want):
        if w is None:
            assert g is None, (got, want)
        elif isinstance(w, float) and math.isnan(w):
            assert isinstance(g, float) and math.isnan(g), (got, want)
        elif isinstance(w, float) and math.isinf(w):
            assert g == w, (got, want)
        else:
            assert g == pytest.approx(w, rel=rel, abs=1e-300), (got, want)


@pytest.mark.parametrize("name", MATH)
@pytest.mark.parametrize("dtype", ["float64", "float32"])
def test_math_matches_polars_on_floats(name, dtype):
    pl_dtype, our_dtype = FLOATS[dtype]
    values = EDGE if dtype == "float64" else [v for v in EDGE if v is None or abs(v) < 1e30]
    theirs = getattr(pl.Series("v", values, dtype=pl_dtype), name)()
    ours = getattr(Series(values).astype(dtype).rename("v"), name)()
    assert ours.dtype == our_dtype
    assert theirs.dtype == pl_dtype
    _same(ours.to_list(), theirs.to_list(), rel=1e-6 if dtype == "float32" else 1e-12)


@pytest.mark.parametrize("name", MATH)
@pytest.mark.parametrize("dtype", sorted(DTYPES))
def test_math_on_every_integer_width_gives_float64(name, dtype):
    pl_dtype, _, bits, signed = DTYPES[dtype]
    values = [0, 1, 2, None, 3, 100] + ([-1, -3] if signed else [])
    theirs = getattr(pl.Series("v", values, dtype=pl_dtype), name)()
    ours = getattr(Series(values).astype(dtype).rename("v"), name)()
    assert theirs.dtype == pl.Float64
    assert ours.dtype == DType.FLOAT64
    _same(ours.to_list(), theirs.to_list(), rel=1e-12)


@pytest.mark.parametrize("name", MATH)
def test_math_domain_errors_are_nan_and_nulls_stay_null(name):
    values = [2.0, -2.0, 0.0, None, 0.5]
    ours = getattr(Series(values).rename("v"), name)().to_list()
    theirs = getattr(pl.Series("v", values, dtype=pl.Float64), name)().to_list()
    _same(ours, theirs, rel=1e-12)
    assert ours[3] is None


def test_arcsin_of_two_is_nan():
    got = Series([2.0, 1.0]).arcsin().to_list()
    assert math.isnan(got[0]) and got[1] == pytest.approx(math.pi / 2)


@pytest.mark.parametrize("name", MATH)
def test_math_expr_matches_series_and_polars_in_select_and_lazy(name):
    values = [0.25, -0.5, None, 2.0, 0.0]
    df = DataFrame({"x": Series(values)})
    want = getattr(pl.Series("x", values, dtype=pl.Float64), name)().to_list()
    eager = df.select(getattr(col("x"), name)().alias("r"))["r"].to_list()
    lazy = df.lazy().select(getattr(col("x"), name)().alias("r")).collect()["r"].to_list()
    _same(eager, want, rel=1e-12)
    _same(lazy, want, rel=1e-12)
    _same(getattr(df["x"], name)().to_list(), want, rel=1e-12)


@pytest.mark.parametrize("name", MATH)
def test_math_refuses_text_and_bool_with_a_clear_message(name):
    for bad in (Series(["a", "b"]), Series([True, False])):
        with pytest.raises(dft.DFTUtilsValueError, match=f"{name}: .*float or an integer"):
            getattr(bad, name)()


def test_log10_is_exact_for_powers_of_ten():
    s = Series([1.0, 10.0, 1000.0, 1e15])
    assert s.log10().to_list() == [0.0, 1.0, 3.0, 15.0]
    got = DataFrame({"x": s}).select(col("x").log10().alias("r"))["r"].to_list()
    assert got == [0.0, 1.0, 3.0, 15.0]


BIT_COUNT = [
    "bitwise_count_ones", "bitwise_count_zeros", "bitwise_leading_ones",
    "bitwise_leading_zeros", "bitwise_trailing_ones", "bitwise_trailing_zeros",
]  # fmt: skip

REDUCE = ["bitwise_and", "bitwise_or", "bitwise_xor"]


def _int_values(bits, signed):
    if signed:
        top = (1 << (bits - 1)) - 1
        return [0, 1, -1, top, -top - 1, 5, None, -8, 6, 1 << (bits - 2)]
    return [0, 1, (1 << bits) - 1, (1 << (bits - 1)), 5, None, 6, 8, (1 << (bits - 1)) - 1]


def _same_ints(ours, theirs):
    assert ours.to_list() == theirs.to_list()


@pytest.mark.parametrize("name", BIT_COUNT)
@pytest.mark.parametrize("dtype", sorted(DTYPES))
def test_bit_counts_match_polars_on_every_width(name, dtype):
    pl_dtype, _, bits, signed = DTYPES[dtype]
    values = _int_values(bits, signed)
    theirs = getattr(pl.Series("v", values, dtype=pl_dtype), name)()
    ours = getattr(Series(values).astype(dtype).rename("v"), name)()
    assert theirs.dtype == pl.UInt32
    assert ours.dtype == DType.UINT32
    _same_ints(ours, theirs)


@pytest.mark.parametrize("name", BIT_COUNT)
def test_bit_counts_on_bool(name):
    values = [True, False, None, True]
    theirs = getattr(pl.Series("v", values, dtype=pl.Boolean), name)()
    ours = getattr(Series(values).rename("v"), name)()
    assert ours.dtype == DType.UINT32
    _same_ints(ours, theirs)


@pytest.mark.parametrize("name", BIT_COUNT)
def test_bit_counts_refuse_floats_and_text(name):
    for bad in (Series([1.5, 2.0]), Series(["a"])):
        with pytest.raises(dft.DFTUtilsValueError, match=f"{name}: .*integer or a bool"):
            getattr(bad, name)()


NULL_COLUMNS = {
    "string": lambda: Series([None, None, None]),
    "float64": lambda: Series([None, None, None]).astype("float64"),
}


@pytest.mark.parametrize("name", REDUCE)
@pytest.mark.parametrize("kind", sorted(NULL_COLUMNS))
@pytest.mark.parametrize("lazy", [False, True])
def test_bit_reductions_in_group_by_refuse_an_all_null_unsupported_column(name, kind, lazy):
    df = DataFrame({"k": Series([1, 1, 2]), "x": NULL_COLUMNS[kind]()})
    frame = df.lazy() if lazy else df
    with pytest.raises(Exception, match="integer or a bool"):
        plan = frame.group_by("k").agg(a=getattr(col("x"), name)())
        if lazy:
            plan.collect()


@pytest.mark.parametrize("name", REDUCE)
def test_bit_reductions_refuse_floats_and_text(name):
    for bad in (Series([1.5, 2.0]), Series(["a"])):
        with pytest.raises(TypeError, match="integer or a bool"):
            getattr(bad, name)()


@pytest.mark.parametrize("name", BIT_COUNT)
def test_bit_counts_expr_in_select_and_lazy(name):
    values = [5, -1, None, 0, 255]
    df = DataFrame({"x": Series(values).astype("int16")})
    want = getattr(pl.Series("x", values, dtype=pl.Int16), name)().to_list()
    eager = df.select(getattr(col("x"), name)().alias("r"))["r"].to_list()
    lazy = df.lazy().select(getattr(col("x"), name)().alias("r")).collect()["r"].to_list()
    assert eager == want and lazy == want


@pytest.mark.parametrize("name", REDUCE)
@pytest.mark.parametrize("dtype", sorted(DTYPES))
def test_bit_reductions_match_polars_on_every_width(name, dtype):
    pl_dtype, _, bits, signed = DTYPES[dtype]
    values = _int_values(bits, signed)
    want = getattr(pl.Series("v", values, dtype=pl_dtype), name)()
    got = getattr(Series(values).astype(dtype).rename("v"), name)()
    assert got == want and type(got) is type(want)
    df = DataFrame({"x": Series(values).astype(dtype)})
    sel = df.select(getattr(col("x"), name)().alias("r"))
    assert sel["r"].dtype == DTYPES[dtype][1]
    assert sel["r"].to_list() == [want]
    lazy = df.lazy().select(getattr(col("x"), name)().alias("r")).collect()
    assert lazy["r"].to_list() == [want]


@pytest.mark.parametrize("name", REDUCE)
def test_bit_reductions_on_bool_empty_and_all_null(name):
    values = [True, False, None, True]
    want = getattr(pl.Series("v", values, dtype=pl.Boolean), name)()
    got = getattr(Series(values).rename("v"), name)()
    assert got is want
    df = DataFrame({"x": Series(values)})
    assert df.select(getattr(col("x"), name)().alias("r"))["r"].to_list() == [want]
    assert getattr(Series([None, None]).astype("int32"), name)() is None
    assert getattr(Series([1]).astype("int32").head(0), name)() is None


@pytest.mark.parametrize("name", REDUCE)
@pytest.mark.parametrize("dtype", ["int8", "uint16", "int64", "uint64"])
def test_bit_reductions_per_group(name, dtype):
    pl_dtype = DTYPES[dtype][0]
    keys = [1, 1, 2, 2, 3, 3, 1]
    signed = DTYPES[dtype][3]
    vals = [6, 3, 5, None, None, None, 12 if not signed else -5]
    pdf = pl.DataFrame({"k": keys, "x": pl.Series(vals, dtype=pl_dtype)})
    want = pdf.group_by("k", maintain_order=True).agg(getattr(pl.col("x"), name)().alias("r"))
    df = DataFrame({"k": Series(keys), "x": Series(vals).astype(dtype)})
    got = df.group_by("k").agg(getattr(col("x"), name)().alias("r")).sort_by("k")
    assert got["k"].to_list() == [1, 2, 3]
    assert got["r"].dtype == DTYPES[dtype][1]
    assert got["r"].to_list() == want.sort("k")["r"].to_list()
    lazy = df.lazy().group_by("k").agg(getattr(col("x"), name)().alias("r")).collect().sort_by("k")
    assert lazy["r"].to_list() == want.sort("k")["r"].to_list()


def test_bit_reductions_per_group_on_bool():
    keys = [1, 1, 2, 2, 3]
    vals = [True, False, True, True, None]
    pdf = pl.DataFrame({"k": keys, "x": pl.Series(vals, dtype=pl.Boolean)})
    df = DataFrame({"k": Series(keys), "x": Series(vals)})
    for name in REDUCE:
        want = pdf.group_by("k", maintain_order=True).agg(getattr(pl.col("x"), name)().alias("r"))
        got = df.group_by("k").agg(getattr(col("x"), name)().alias("r")).sort_by("k")
        assert got["r"].dtype == DType.BOOL
        assert got["r"].to_list()[:2] == want.sort("k")["r"].to_list()[:2], name
        # A group with no present value is null here, as in a select and for an
        # integer column; polars gives the identity (True, False, False) only
        # for a Boolean group.
        assert got["r"].to_list()[2] is None


def test_expr_reductions_in_the_same_agg_share_one_pass():
    df = DataFrame({"k": Series([1, 1, 2]), "x": Series([6, 3, 5]).astype("int32")})
    got = (
        df.group_by("k")
        .agg(
            a=col("x").bitwise_and(),
            o=col("x").bitwise_or(),
            x=col("x").bitwise_xor(),
            s=col("x").sum(),
        )
        .sort_by("k")
    )
    assert got["a"].to_list() == [2, 5]
    assert got["o"].to_list() == [7, 5]
    assert got["x"].to_list() == [5, 5]
    assert got["s"].to_list() == [9, 5]


def test_dft_namespace_is_imported():
    assert dft.col is col
