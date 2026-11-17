# SPDX-License-Identifier: MIT
"""Polars parity (plan D3b, the Polars half of the gate): each Polars-named
method we expose on Series, DataFrame, LazyFrame, Expr and the group-by runs
on the same data in Polars and in ours, and the results are compared: type,
shape, dtype kind and values.

The cases come from the tables below, not one test per method. ``FINDINGS``
lists the methods whose result differs today, each with its reason; a listed
case is an expected failure (non-strict, so a fixed method shows as XPASS and
its row is deleted). A method that is not listed must match Polars exactly."""

import pytest

pa = pytest.importorskip("pyarrow")
pl = pytest.importorskip("polars")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import DataFrame, Series  # noqa: E402

from .parity_common import R, compare, load_findings, norm_polars, resolve  # noqa: E402

NAN = float("nan")

# Series inputs: ints with nulls, floats with a null and a NaN, strings with
# nulls, bools, and an empty int64.
SERIES = {
    "I": (
        "int_nulls",
        lambda: Series([1, None, 3, 2, 2, None, -4]).rename("v"),
        lambda: pl.Series("v", [1, None, 3, 2, 2, None, -4], dtype=pl.Int64),
    ),
    "F": (
        "float_null_nan",
        lambda: Series([1.5, None, NAN, -2.5, 3.0, 0.0]).rename("v"),
        lambda: pl.Series("v", [1.5, None, NAN, -2.5, 3.0, 0.0], dtype=pl.Float64),
    ),
    "S": (
        "str_nulls",
        lambda: Series(["b", "a", None, "ab", "a"]).rename("v"),
        lambda: pl.Series("v", ["b", "a", None, "ab", "a"], dtype=pl.String),
    ),
    "B": (
        "bool",
        lambda: Series([True, False, True, True]).rename("v"),
        lambda: pl.Series("v", [True, False, True, True], dtype=pl.Boolean),
    ),
    "E": (
        "empty",
        lambda: Series.from_arrow(pa.array([], pa.int64())).rename("v"),
        lambda: pl.Series("v", [], dtype=pl.Int64),
    ),
}

_MIXED = {
    "a": [1, None, 3, 2, 2],
    "b": [1.5, None, NAN, -2.5, 3.0],
    "c": ["x", None, "y", "x", "y"],
    "d": [True, False, True, True, False],
}
_NUM = {"a": _MIXED["a"], "b": _MIXED["b"]}
_GROUPED = {
    "k": ["x", "y", "x", "y", "x"],
    "a": [1, None, 3, 2, 2],
    "b": [1.5, None, 3.0, -2.5, 3.0],
}
_PL_DTYPES = {"a": pl.Int64, "b": pl.Float64, "c": pl.String, "d": pl.Boolean, "k": pl.String}


def _frames(data):
    ours = lambda: DataFrame(data)  # noqa: E731
    theirs = lambda: pl.DataFrame(  # noqa: E731
        {k: pl.Series(k, v, dtype=_PL_DTYPES[k]) for k, v in data.items()}
    )
    return ours, theirs


FRAMES = {
    "M": ("mixed", *_frames(_MIXED)),
    "N": ("num", *_frames(_NUM)),
    "G": ("grouped", *_frames(_GROUPED)),
    "E": (
        "empty",
        lambda: DataFrame.from_arrow(
            pa.table({"a": pa.array([], pa.int64()), "b": pa.array([], pa.float64())})
        ),
        lambda: pl.DataFrame(
            {"a": pl.Series("a", [], dtype=pl.Int64), "b": pl.Series("b", [], dtype=pl.Float64)}
        ),
    ),
}

SELF = R(lambda r: r)


def _col(receiver):
    return dft.col if isinstance(receiver, dft.LazyFrame) else pl.col


def _like(r):
    return type(r)(list(range(10, 10 + len(r))))


OTHER = R(_like)

SERIES_CASES = [
    # reductions and statistics
    *[(m, "IFE", (), {}) for m in ("sum", "mean", "median", "min", "max", "std", "var", "product")],
    ("sum", "IF", (), {}),
    ("std", "IF", (), {"ddof": 0}),
    ("var", "IF", (), {"ddof": 0}),
    ("skew", "IF", (), {}),
    ("kurtosis", "IF", (), {}),
    ("count", "IFSE", (), {}),
    ("len", "IFSBE", (), {}),
    ("n_unique", "IFS", (), {}),
    ("null_count", "IFSE", (), {}),
    ("quantile", "IF", (0.5,), {}),
    ("quantile", "IF", (0.25,), {"interpolation": "lower"}),
    ("arg_max", "IF", (), {}),
    ("arg_min", "IF", (), {}),
    ("any", "B", (), {}),
    ("all", "B", (), {}),
    ("describe", "IF", (), {}),
    ("min", "S", (), {}),
    ("max", "S", (), {}),
    ("dot", "I", (OTHER,), {}),
    # elementwise
    ("abs", "IF", (), {}),
    ("round", "F", (1,), {}),
    ("round", "F", (), {"decimals": 0}),
    ("ceil", "F", (), {}),
    ("floor", "F", (), {}),
    ("sqrt", "IF", (), {}),
    ("exp", "F", (), {}),
    ("log", "F", (), {}),
    ("sign", "IF", (), {}),
    ("pow", "IF", (2,), {}),
    ("clip", "IF", (), {"lower_bound": -1, "upper_bound": 2}),
    ("clip", "IF", (0,), {}),
    ("cum_sum", "IF", (), {}),
    ("cum_sum", "IF", (), {"reverse": True}),
    ("cum_prod", "IF", (), {}),
    ("cum_max", "IF", (), {}),
    ("cum_min", "IF", (), {}),
    ("cum_count", "IF", (), {}),
    ("diff", "IF", (), {}),
    ("diff", "IF", (2,), {}),
    ("diff", "IF", (), {"n": 1, "null_behavior": "drop"}),
    ("shift", "IFS", (1,), {}),
    ("shift", "IFS", (-1,), {}),
    ("shift", "IF", (), {"n": 2, "fill_value": 0}),
    ("pct_change", "F", (), {}),
    ("rank", "IF", (), {}),
    ("rank", "IF", ("min",), {}),
    ("rank", "IF", (), {"method": "dense", "descending": True}),
    ("rank", "IF", ("ordinal",), {}),
    ("fill_null", "IFS", (0,), {}),
    ("fill_null", "I", (), {"value": 7}),
    ("fill_null", "IF", (), {"strategy": "forward"}),
    ("fill_null", "IF", (), {"strategy": "backward"}),
    ("fill_null", "IF", (), {"strategy": "mean"}),
    ("forward_fill", "IF", (), {}),
    ("forward_fill", "IF", (), {"limit": 1}),
    ("backward_fill", "IF", (), {}),
    ("interpolate", "F", (), {}),
    ("is_null", "IFS", (), {}),
    ("is_not_null", "IFS", (), {}),
    ("is_nan", "F", (), {}),
    ("is_finite", "F", (), {}),
    ("is_infinite", "F", (), {}),
    ("drop_nulls", "IFS", (), {}),
    ("is_in", "IS", ([1, 2],), {}),
    ("is_in", "S", (["a", "b"],), {}),
    ("is_between", "IF", (1, 3), {}),
    ("is_between", "IF", (1, 3), {"closed": "none"}),
    ("is_between", "IF", (1, 3), {"closed": "left"}),
    ("filter", "IF", (R(lambda r: r > 1),), {}),
    ("replace", "IF", (1, 100), {}),
    ("replace", "IF", ({1: 10, 2: 20},), {}),
    ("cast", "IF", (pl.Float64,), {}),
    ("cast", "IF", (pl.Int64,), {"strict": False}),
    ("cast", "B", (pl.Int64,), {}),
    ("cast", "I", (pl.String,), {}),
    ("is_duplicated", "IFS", (), {}),
    ("is_unique", "IFS", (), {}),
    ("unique", "IFS", (), {}),
    ("unique", "IFS", (), {"maintain_order": True}),
    ("value_counts", "IFS", (), {}),
    ("value_counts", "IFS", (), {"sort": True}),
    ("value_counts", "IFS", (), {"normalize": True, "sort": True}),
    ("mode", "IFS", (), {}),
    ("arg_sort", "IF", (), {}),
    ("arg_sort", "IF", (), {"descending": True}),
    ("sort", "IFS", (), {}),
    ("sort", "IFS", (), {"descending": True}),
    ("sort", "IFS", (), {"nulls_last": True}),
    ("top_k", "IF", (2,), {}),
    ("bottom_k", "IF", (2,), {}),
    ("head", "IFSE", (2,), {}),
    ("head", "IF", (), {}),
    ("tail", "IFSE", (2,), {}),
    ("tail", "IF", (), {"n": 3}),
    ("slice", "IFS", (1, 2), {}),
    ("slice", "IFS", (-2,), {}),
    ("gather", "IFS", ([0, 2],), {}),
    ("reverse", "IFS", (), {}),
    ("search_sorted", "F", (1.0,), {}),
    ("sample", "IF", (), {"n": 3, "seed": 1}),
    ("explode", "IFS", (), {}),
    ("item", "I", (), {}),
    ("to_list", "IFSB", (), {}),
    ("to_numpy", "F", (), {}),
    ("to_frame", "IFS", (), {}),
    ("to_frame", "IF", ("x",), {}),
    ("rename", "IF", ("z",), {}),
    ("is_sorted", "IFS", (), {}),
    ("equals", "IFS", (SELF,), {}),
    ("pipe", "IF", (R(lambda r: lambda s: s),), {}),
    ("map", "IF", (R(lambda r: lambda x: x + 1),), {}),
    ("truncate", "IF", (2,), {}),
    ("interpolate", "I", (), {}),
    ("ewm_mean", "F", (), {"alpha": 0.5}),
    ("ewm_std", "F", (), {"alpha": 0.5}),
    ("rolling_sum", "IF", (2,), {}),
    ("rolling_mean", "IF", (2,), {}),
    ("rolling_min", "IF", (2,), {}),
    ("rolling_max", "IF", (2,), {}),
    ("rolling_var", "F", (2,), {}),
    ("rolling_std", "F", (2,), {}),
    ("rolling_median", "F", (2,), {}),
    ("rolling_quantile", "F", (0.5, "nearest", 2), {}),
    ("rolling_sum", "IF", (), {"window_size": 3, "min_samples": 1}),
    ("rolling_mean", "IF", (), {"window_size": 3, "min_samples": 1}),
    ("rolling_mean", "IF", (), {"window_size": 3, "center": True}),
    *[(m, "IF", (OTHER,), {}) for m in ("eq", "ne", "ge", "gt", "le", "lt")],
    *[(m, "IF", (2,), {}) for m in ("eq", "ne", "ge", "gt", "le", "lt")],
    ("ne", "S", ("a",), {}),
    # attributes
    ("shape", "IFSBE", None, {}),
    ("dtype", "IFSBE", None, {}),
    ("name", "IFSBE", None, {}),
]  # fmt: skip

FRAME_CASES = [
    *[(m, "NE", (), {}) for m in ("sum", "mean", "median", "min", "max", "std", "var", "product", "null_count", "n_unique")],
    ("sum", "N", (), {}),
    ("std", "N", (), {"ddof": 0}),
    ("quantile", "N", (0.5,), {}),
    ("count", "NM", (), {}),
    ("describe", "N", (), {}),
    ("corr", "N", (), {}),
    ("max_horizontal", "N", (), {}),
    ("min_horizontal", "N", (), {}),
    ("sum_horizontal", "N", (), {}),
    ("mean_horizontal", "N", (), {}),
    ("fill_null", "M", (0,), {}),
    ("fill_null", "N", (), {"value": 7}),
    ("fill_null", "N", (), {"strategy": "forward"}),
    ("fill_nan", "N", (0,), {}),
    ("drop_nulls", "M", (), {}),
    ("drop_nulls", "M", ("a",), {}),
    ("drop_nulls", "M", (), {"subset": ["a", "b"]}),
    ("drop_nans", "N", (), {}),
    ("shift", "MN", (1,), {}),
    ("shift", "N", (-1,), {}),
    ("shift", "N", (), {"n": 2, "fill_value": 0}),
    ("interpolate", "N", (), {}),
    ("filter", "M", (R(lambda r: r["a"] > 1),), {}),
    ("filter", "M", (), {"predicate": R(lambda r: r["a"] > 1)}),
    ("select", "M", ("a", "c"), {}),
    ("select", "M", ([R(lambda r: r["a"] * 2)],), {}),
    ("with_columns", "N", (), {"z": R(lambda r: r["a"] * 2)}),
    ("with_columns", "N", (R(lambda r: (r["a"] + 1).alias("z")),), {}),
    ("with_row_index", "M", (), {}),
    ("with_row_index", "M", ("n",), {"offset": 1}),
    ("drop", "M", ("a",), {}),
    ("drop", "M", (["a", "b"],), {}),
    ("rename", "M", ({"a": "A"},), {}),
    ("cast", "N", (pl.Float64,), {}),
    ("cast", "N", ({"a": pl.Float64},), {}),
    ("unique", "M", (), {"maintain_order": True}),
    ("unique", "M", (), {"subset": ["c"], "keep": "last", "maintain_order": True}),
    ("is_duplicated", "M", (), {}),
    ("is_unique", "M", (), {}),
    ("is_empty", "MNE", (), {}),
    ("sort", "M", ("a",), {}),
    ("sort", "M", (), {"by": ["c", "a"], "descending": [False, True]}),
    ("sort", "M", ("a",), {"nulls_last": True}),
    ("top_k", "N", (2,), {"by": "a"}),
    ("bottom_k", "N", (2,), {"by": "a"}),
    ("head", "MNE", (2,), {}),
    ("head", "M", (), {}),
    ("tail", "MNE", (2,), {}),
    ("slice", "M", (1, 2), {}),
    ("limit", "M", (2,), {}),
    ("gather_every", "M", (2,), {}),
    ("reverse", "M", (), {}),
    ("gather", "M", ([0, 2],), {}),
    ("sample", "N", (), {"n": 2, "seed": 1}),
    ("explode", "M", ("c",), {}),
    ("unpivot", "N", (), {"index": "a"}),
    ("unpivot", "N", (), {"on": ["b"], "index": ["a"], "variable_name": "var", "value_name": "val"}),
    ("pivot", "G", (), {"on": "k", "index": None, "values": "a", "aggregate_function": "sum"}),
    ("transpose", "N", (), {}),
    ("transpose", "N", (), {"include_header": True}),
    ("to_dummies", "M", ("c",), {}),
    ("to_series", "M", (), {}),
    ("to_series", "M", (1,), {}),
    ("to_dict", "N", (), {}),
    ("to_dict", "N", (), {"as_series": False}),
    ("to_dicts", "N", (), {}),
    ("to_numpy", "N", (), {}),
    ("get_column", "M", ("a",), {}),
    ("get_columns", "M", (), {}),
    ("get_column_index", "M", ("c",), {}),
    ("row", "M", (1,), {}),
    ("rows", "N", (), {}),
    ("item", "M", (1, "a"), {}),
    ("iter_rows", "N", (), {}),
    ("equals", "MNE", (SELF,), {}),
    ("hstack", "N", (R(lambda r: [r["a"].rename("z")]),), {}),
    ("vstack", "N", (SELF,), {}),
    ("update", "N", (SELF,), {}),
    ("partition_by", "M", ("c",), {}),
    ("pipe", "M", (R(lambda r: lambda d: d),), {}),
    ("join", "G", (R(lambda r: r),), {"on": "k", "how": "inner"}),
    ("join", "G", (R(lambda r: r),), {"on": "k", "how": "left", "suffix": "_r"}),
    ("join", "G", (R(lambda r: r),), {"on": "k", "how": "semi"}),
    ("join", "G", (R(lambda r: r),), {"on": "k", "how": "anti"}),
    ("join", "G", (R(lambda r: r),), {"left_on": "k", "right_on": "k", "how": "full", "coalesce": True}),
    ("rolling", "N", (), {"index_column": "a", "period": "2i"}),
    ("lazy", "M", (), {}),
    ("shape", "MNE", None, {}),
    ("height", "MNE", None, {}),
    ("width", "MNE", None, {}),
    ("columns", "MNE", None, {}),
    ("dtypes", "MNE", None, {}),
    ("schema", "MNE", None, {}),
]  # fmt: skip

LAZY_CASES = [
    *[(m, "N", (), {}) for m in ("sum", "mean", "min", "max", "std", "var", "null_count", "count")],
    ("fill_null", "M", (0,), {}),
    ("fill_null", "N", (), {"strategy": "forward"}),
    ("drop_nulls", "M", (), {}),
    ("drop_nulls", "M", (), {"subset": ["a"]}),
    ("shift", "N", (1,), {}),
    ("filter", "M", (R(lambda r: _col(r)("a") > 1),), {}),
    ("select", "M", ("a", "c"), {}),
    ("with_columns", "N", (), {"z": R(lambda r: _col(r)("a") * 2)}),
    ("with_row_index", "M", (), {}),
    ("drop", "M", ("a",), {}),
    ("rename", "M", ({"a": "A"},), {}),
    ("unique", "M", (), {"maintain_order": True}),
    ("sort", "M", ("a",), {}),
    ("sort", "M", ("a",), {"descending": True, "nulls_last": True}),
    ("head", "M", (2,), {}),
    ("head", "M", (), {}),
    ("tail", "M", (2,), {}),
    ("slice", "M", (1, 2), {}),
    ("limit", "M", (2,), {}),
    ("reverse", "M", (), {}),
    ("gather", "M", ([0, 2],), {}),
    ("explode", "M", ("c",), {}),
    ("unpivot", "N", (), {"index": "a"}),
    ("describe", "N", (), {}),
    ("join", "G", (R(lambda r: r),), {"on": "k", "how": "inner"}),
    ("collect", "M", (), {}),
    ("collect_schema", "M", (), {}),
]  # fmt: skip

GROUPBY_CASES = [
    *[(m, "G", (), {}) for m in ("sum", "mean", "median", "min", "max", "first", "last", "len", "n_unique")],
    ("quantile", "G", (0.5,), {}),
    ("head", "G", (1,), {}),
    ("tail", "G", (1,), {}),
    ("all", "G", (), {}),
]  # fmt: skip

# Expression cases: (method, input, args, kwargs) run as
# df.select(col("v").method(...).alias("r")) on a one-column frame named v, in
# both libraries.
EXPR_CASES = [
    ("abs", "IF", (), {}),
    ("sqrt", "IF", (), {}),
    ("exp", "F", (), {}),
    ("log", "F", (), {}),
    ("log10", "F", (), {}),
    ("log1p", "F", (), {}),
    ("ceil", "F", (), {}),
    ("floor", "F", (), {}),
    ("round", "F", (1,), {}),
    ("sign", "IF", (), {}),
    ("pow", "IF", (2,), {}),
    ("clip", "IF", (), {"lower_bound": -1, "upper_bound": 2}),
    ("sum", "IF", (), {}),
    ("mean", "IF", (), {}),
    ("median", "IF", (), {}),
    ("min", "IF", (), {}),
    ("max", "IF", (), {}),
    ("std", "IF", (), {}),
    ("var", "IF", (), {"ddof": 0}),
    ("count", "IFS", (), {}),
    ("len", "IFS", (), {}),
    ("n_unique", "IFS", (), {}),
    ("null_count", "IFS", (), {}),
    ("first", "IFS", (), {}),
    ("last", "IFS", (), {}),
    ("skew", "IF", (), {}),
    ("quantile", "IF", (0.5,), {}),
    ("arg_max", "IF", (), {}),
    ("arg_min", "IF", (), {}),
    ("cum_sum", "IF", (), {}),
    ("cum_prod", "IF", (), {}),
    ("cum_max", "IF", (), {}),
    ("cum_min", "IF", (), {}),
    ("cum_count", "IF", (), {}),
    ("diff", "IF", (), {}),
    ("shift", "IFS", (1,), {}),
    ("pct_change", "F", (), {}),
    ("rank", "IF", (), {}),
    ("rank", "IF", ("dense",), {}),
    ("fill_null", "IFS", (0,), {}),
    ("fill_null", "IF", (), {"strategy": "forward"}),
    ("forward_fill", "IF", (), {}),
    ("backward_fill", "IF", (), {}),
    ("interpolate", "F", (), {}),
    ("is_null", "IFS", (), {}),
    ("is_not_null", "IFS", (), {}),
    ("is_nan", "F", (), {}),
    ("is_finite", "F", (), {}),
    ("is_infinite", "F", (), {}),
    ("is_in", "IS", ([1, 2],), {}),
    ("is_between", "IF", (1, 3), {}),
    ("is_duplicated", "IFS", (), {}),
    ("is_unique", "IFS", (), {}),
    ("cast", "IF", (pl.Float64,), {}),
    ("alias", "IFS", ("z",), {}),
    ("sort", "IFS", (), {}),
    ("sort", "IFS", (), {"descending": True, "nulls_last": True}),
    ("arg_sort", "IF", (), {}),
    ("reverse", "IFS", (), {}),
    ("replace", "IF", (1, 100), {}),
    ("hash", "IS", (), {}),
    ("slice", "IFS", (1, 2), {}),
    ("get", "IFS", (1,), {}),
    ("ewm_mean", "F", (), {"alpha": 0.5}),
    ("ewm_std", "F", (), {"alpha": 0.5}),
    ("rolling_sum", "IF", (2,), {}),
    ("rolling_mean", "IF", (2,), {}),
    ("rolling_min", "IF", (2,), {}),
    ("rolling_max", "IF", (2,), {}),
    ("rolling_var", "F", (2,), {}),
    ("rolling_std", "F", (2,), {}),
    ("rolling_median", "F", (2,), {}),
    ("rolling_quantile", "F", (0.5, "nearest", 2), {}),
    ("sample", "IF", (), {"n": 3, "seed": 1}),
    ("eq", "IF", (2,), {}),
    ("ne", "IF", (2,), {}),
    ("ne", "S", ("a",), {}),
    ("hist", "F", (), {}),
]  # fmt: skip

STR_CASES = [
    ("to_uppercase", "S", (), {}),
    ("to_lowercase", "S", (), {}),
    ("to_titlecase", "S", (), {}),
    ("len_chars", "S", (), {}),
    ("len_bytes", "S", (), {}),
    ("contains", "S", ("a",), {}),
    ("contains", "S", ("a.",), {"literal": True}),
    ("contains", "S", ("^a",), {}),
    ("starts_with", "S", ("a",), {}),
    ("ends_with", "S", ("b",), {}),
    ("strip_chars", "S", (), {}),
    ("strip_chars", "S", ("a",), {}),
    ("strip_chars_start", "S", ("a",), {}),
    ("strip_chars_end", "S", ("a",), {}),
    ("strip_prefix", "S", ("a",), {}),
    ("strip_suffix", "S", ("b",), {}),
    ("replace", "S", ("a", "x"), {}),
    ("replace", "S", ("a", "x"), {"literal": True}),
    ("replace_all", "S", ("a", "x"), {}),
    ("replace_all", "S", ("a", "x"), {"literal": True}),
    ("split", "S", ("b",), {}),
    ("find", "S", ("a",), {}),
    ("slice", "S", (0, 1), {}),
    ("slice", "S", (1,), {}),
    ("pad_start", "S", (4,), {}),
    ("pad_end", "S", (4, "*"), {}),
    ("zfill", "S", (4,), {}),
    ("count_matches", "S", ("a",), {}),
    ("extract", "S", ("(a)", 1), {}),
    ("join", "S", ("-",), {}),
    ("encode", "S", ("hex",), {}),
]  # fmt: skip

FINDINGS = load_findings("parity_findings_polars.txt")


def _build_params():
    params = []
    count = {}
    for kind, cases in (
        ("series", SERIES_CASES),
        ("frame", FRAME_CASES),
        ("lazy", LAZY_CASES),
        ("groupby", GROUPBY_CASES),
        ("expr", EXPR_CASES),
        ("str", STR_CASES),
    ):
        for method, letters, args, kwargs in cases:
            key = f"{kind}.{method}"
            count[key] = count.get(key, 0) + 1
            for letter in letters:
                marks = []
                if key in FINDINGS:
                    marks.append(pytest.mark.xfail(reason=FINDINGS[key], strict=False))
                name = f"{key}#{count[key]}[{letter}]"
                params.append(
                    pytest.param(kind, method, letter, args, kwargs, marks=marks, id=name)
                )
    return params


PARAMS = _build_params()


def _receiver(kind, letter, side):
    table = SERIES if kind in ("series", "str", "expr") else FRAMES
    make = table[letter][1 if side == "ours" else 2]
    recv = make()
    if kind == "expr":
        return recv.to_frame("v")
    if kind == "lazy":
        return recv.lazy()
    return recv


def _run(kind, method, letter, args, kwargs, side):
    recv = _receiver(kind, letter, side)
    if kind == "groupby":
        recv = recv.group_by("k", maintain_order=True)
    base = recv
    a = [resolve(x, base) for x in (args or ())]
    kw = {k: resolve(v, base) for k, v in kwargs.items()}
    if kind == "expr":
        expr = getattr((dft.col if side == "ours" else pl.col)("v"), method)(*a, **kw)
        # Ours names a computed column explicitly; Polars keeps the root name.
        return recv.select(expr.alias("r"))
    if kind == "str":
        return getattr(recv.str, method)(*a, **kw)
    if args is None:
        return getattr(recv, method)
    return getattr(recv, method)(*a, **kw)


@pytest.mark.parametrize("kind,method,letter,args,kwargs", PARAMS)
def test_matches_polars(kind, method, letter, args, kwargs):
    theirs_err = ours_err = None
    try:
        theirs = _run(kind, method, letter, args, kwargs, "polars")
    except Exception as e:  # noqa: BLE001
        theirs_err = e
    try:
        ours = _run(kind, method, letter, args, kwargs, "ours")
    except Exception as e:  # noqa: BLE001
        ours_err = e
    if theirs_err is not None:
        if ours_err is None:
            pytest.fail(f"[raises] polars raised {type(theirs_err).__name__}, ours returned")
        return
    if ours_err is not None:
        pytest.fail(f"[error] ours raised {type(ours_err).__name__}: {ours_err}")
    diff = compare(ours, theirs, norm_polars(pl, theirs))
    assert diff is None, diff


def test_every_finding_names_a_case():
    cases = {p.id.split("#")[0] for p in PARAMS}
    assert not (set(FINDINGS) - cases), sorted(set(FINDINGS) - cases)
