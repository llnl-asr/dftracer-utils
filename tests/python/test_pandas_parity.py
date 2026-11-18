# SPDX-License-Identifier: MIT
"""pandas parity (plan D3b point 2): each pandas-named method we expose on
Series, DataFrame and the group-by runs on the same data in pandas and in ours,
and the results are compared: type, shape, dtype kind, values and, where we
have no row index, that the pandas result has the default RangeIndex.

The cases come from the tables below (method -> inputs, arguments), not one
test per method. ``FINDINGS`` lists the cases (method, input) whose result
differs today, each with its reason; a listed case is a strict expected
failure, so a fixed case fails the run until its row is deleted. A case that
is not listed must match pandas exactly."""

import pytest

pa = pytest.importorskip("pyarrow")
pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402

from .parity_common import R, compare, load_findings, norm_pandas, resolve  # noqa: E402

# pandas 2 warns about behaviours it deprecates (argsort over NA, apply over
# the grouping columns); the cases run the documented call either way.
pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning")

NAN = float("nan")


def _pd_series(data, dtype):
    return pd.Series(data, dtype=dtype)


# Series inputs: ints with nulls, floats with nulls (pandas holds them as NaN),
# floats with a real NaN value (ours keeps NaN apart from null), strings with
# nulls, bools, and an empty int64.
SERIES = {
    "I": (
        "int_nulls",
        lambda: Series([1, None, 3, 2, 2, None, -4]),
        lambda: _pd_series([1, None, 3, 2, 2, None, -4], "Int64"),
    ),
    "F": (
        "float_null",
        lambda: Series([1.5, None, 3.0, -2.5, 3.0, 0.0]),
        lambda: _pd_series([1.5, None, 3.0, -2.5, 3.0, 0.0], "Float64"),
    ),
    "X": (
        "float_nan",
        lambda: Series([1.5, NAN, 3.0, -2.5, 3.0, 0.0]),
        lambda: _pd_series([1.5, NAN, 3.0, -2.5, 3.0, 0.0], "float64"),
    ),
    "W": (
        "float_whole",
        lambda: Series([1.0, None, 3.0, -2.0]),
        # numpy float64 reads its NaN as missing, which convert_dtypes turns to Int64.
        lambda: _pd_series([1.0, NAN, 3.0, -2.0], "float64"),
    ),
    "S": (
        "str_nulls",
        lambda: Series(["b", "a", None, "ab", "a"]),
        lambda: _pd_series(["b", "a", None, "ab", "a"], "string"),
    ),
    "B": (
        "bool",
        lambda: Series([True, False, True, True]),
        lambda: _pd_series([True, False, True, True], "bool"),
    ),
    "E": (
        "empty",
        lambda: Series.from_arrow(pa.array([], pa.int64())),
        lambda: _pd_series([], "Int64"),
    ),
}

_MIXED = {
    "a": [1, None, 3, 2, 2],
    "b": [1.5, None, 3.0, -2.5, 3.0],
    "c": ["x", None, "y", "x", "y"],
    "d": [True, False, True, True, False],
}
_NUM = {"a": _MIXED["a"], "b": _MIXED["b"]}
_WHOLE = {"a": [1, None, 3], "b": [1.0, None, 3.0], "c": ["x", None, "y"]}
_NANS = {"a": _MIXED["a"], "b": [1.5, NAN, 3.0, -2.5, 3.0]}
_GROUPED = {
    "k": ["x", "y", "x", "y", "x"],
    "a": [1, None, 3, 2, 2],
    "b": [1.5, None, 3.0, -2.5, 3.0],
}
_PD_DTYPES = {"a": "Int64", "b": "Float64", "c": "string", "d": "bool", "k": "string"}


def _pd_dtype(name, values):
    # A real NaN needs numpy float64: the nullable Float64 folds NaN into NA.
    if name == "b" and any(v != v for v in values if v is not None):
        return "float64"
    return _PD_DTYPES[name]


def _frames(data):
    ours = lambda: DataFrame(data)  # noqa: E731
    theirs = lambda: pd.DataFrame(  # noqa: E731
        {k: pd.Series(v, dtype=_pd_dtype(k, v)) for k, v in data.items()}
    )
    return ours, theirs


FRAMES = {
    "M": ("mixed", *_frames(_MIXED)),
    "N": ("num", *_frames(_NUM)),
    "X": ("nan_value", *_frames(_NANS)),
    "W": (
        "whole",
        lambda: DataFrame(_WHOLE),
        lambda: pd.DataFrame(
            {
                "a": pd.Series([1, None, 3], dtype="Int64"),
                "b": pd.Series([1.0, NAN, 3.0], dtype="float64"),
                "c": pd.Series(["x", None, "y"], dtype="string"),
            }
        ),
    ),
    "G": ("grouped", *_frames(_GROUPED)),
    "E": (
        "empty",
        lambda: DataFrame.from_arrow(
            pa.table({"a": pa.array([], pa.int64()), "b": pa.array([], pa.float64())})
        ),
        lambda: pd.DataFrame(
            {"a": pd.Series([], dtype="Int64"), "b": pd.Series([], dtype="Float64")}
        ),
    ),
}


def _like(r):
    return type(r)(list(range(10, 10 + len(r))))


OTHER = R(_like)
SELF = R(lambda r: r)
POS = R(lambda r: r > 1)

# (method, inputs, args, kwargs); args None means the attribute, not a call.
_RED = "IFE"
SERIES_CASES = [
    # reductions
    *[(m, _RED, (), {}) for m in ("sum", "mean", "median", "min", "max", "std", "var", "prod")],
    *[(m, "X", (), {}) for m in ("sum", "mean", "max", "count", "nunique", "isna", "notna", "cumsum")],
    ("fillna", "X", (0,), {}),
    ("dropna", "X", (), {}),
    ("sum", "IF", (), {"skipna": False}),
    ("sum", "IF", (), {"min_count": 1}),
    ("sum", "E", (), {"min_count": 1}),
    ("mean", "IF", (), {"skipna": False}),
    ("max", "IF", (), {"skipna": False}),
    ("std", "IF", (), {"ddof": 0}),
    ("var", "IF", (), {"ddof": 0}),
    ("sem", "IF", (), {}),
    ("skew", "IF", (), {}),
    ("kurt", "IF", (), {}),
    ("kurtosis", "IF", (), {}),
    ("count", "IFSE", (), {}),
    ("nunique", "IFS", (), {}),
    ("nunique", "IFS", (), {"dropna": False}),
    ("min", "S", (), {}),
    ("max", "S", (), {}),
    ("any", "B", (), {}),
    ("all", "B", (), {}),
    ("quantile", "IF", (0.5,), {}),
    ("quantile", "IF", (), {"q": 0.25}),
    ("quantile", "IF", (0.4,), {"interpolation": "lower"}),
    ("quantile", "IF", (0.4,), {"interpolation": "higher"}),
    ("quantile", "IF", (0.4,), {"interpolation": "midpoint"}),
    ("quantile", "IF", (0.4,), {"interpolation": "linear"}),
    ("quantile", "IF", ([0.25, 0.75],), {}),
    ("median", "IF", (), {"skipna": False}),
    ("idxmax", "IF", (), {}),
    ("idxmin", "IF", (), {}),
    ("argmax", "IF", (), {}),
    ("argmin", "IF", (), {}),
    ("describe", "IF", (), {}),
    ("cov", "IF", (OTHER,), {}),
    ("corr", "IF", (OTHER,), {}),
    ("autocorr", "F", (), {}),
    ("dot", "I", (OTHER,), {}),
    # elementwise
    ("convert_dtypes", "IFSBWX", (), {}),
    ("convert_dtypes", "W", (), {"convert_integer": False}),
    ("infer_objects", "IFSBW", (), {}),
    ("abs", "IF", (), {}),
    ("round", "F", (), {}),
    ("round", "F", (1,), {}),
    ("clip", "IF", (), {"lower": -1, "upper": 2}),
    ("clip", "IF", (0,), {}),
    ("cumsum", "IF", (), {}),
    ("cumsum", "IF", (), {"skipna": False}),
    ("cumprod", "IF", (), {}),
    ("cummax", "IF", (), {}),
    ("cummin", "IF", (), {}),
    ("diff", "IF", (), {}),
    ("diff", "IF", (2,), {}),
    ("diff", "IF", (), {"periods": -1}),
    ("shift", "IFS", (1,), {}),
    ("shift", "IFS", (-1,), {}),
    ("shift", "IF", (), {"periods": 2, "fill_value": 0}),
    ("pct_change", "F", (), {}),
    ("rank", "IF", (), {}),
    ("rank", "IF", (), {"method": "min"}),
    ("rank", "IF", (), {"method": "dense", "ascending": False}),
    ("rank", "IF", (), {"na_option": "bottom"}),
    ("rank", "IF", (), {"pct": True}),
    ("fillna", "IF", (0,), {}),
    ("fillna", "IF", (), {"value": 7}),
    ("fillna", "S", ("z",), {}),
    ("ffill", "IF", (), {}),
    ("bfill", "IF", (), {}),
    ("ffill", "IF", (), {"limit": 1}),
    ("interpolate", "F", (), {}),
    ("isna", "IFS", (), {}),
    ("isnull", "IFS", (), {}),
    ("notna", "IFS", (), {}),
    ("notnull", "IFS", (), {}),
    ("dropna", "IFS", (), {}),
    ("isin", "IS", ([1, 2],), {}),
    ("isin", "S", (["a", "b"],), {}),
    ("between", "IF", (1, 3), {}),
    ("between", "IF", (1, 3), {"inclusive": "neither"}),
    ("where", "IF", (POS,), {}),
    ("where", "IF", (POS, 0), {}),
    ("mask", "IF", (POS,), {}),
    ("mask", "IF", (POS, 0), {}),
    ("replace", "IF", (1, 100), {}),
    ("replace", "IF", ({1: 10, 2: 20},), {}),
    ("replace", "S", ("a", "A"), {}),
    ("astype", "IF", ("float64",), {}),
    ("astype", "IF", ("int64",), {}),
    ("astype", "F", ("string",), {}),
    ("astype", "B", ("int64",), {}),
    ("copy", "IFSBE", (), {}),
    ("duplicated", "IFS", (), {}),
    ("duplicated", "IFS", (), {"keep": "last"}),
    ("duplicated", "IFS", (), {"keep": False}),
    ("drop_duplicates", "IFS", (), {}),
    ("drop_duplicates", "IFS", (), {"keep": "last"}),
    ("unique", "IFS", (), {}),
    ("value_counts", "IFS", (), {}),
    ("value_counts", "IFS", (), {"dropna": False}),
    ("value_counts", "IFS", (), {"normalize": True}),
    ("value_counts", "IFS", (), {"sort": False}),
    ("mode", "IFS", (), {}),
    ("mode", "I", (), {"dropna": False}),
    ("argsort", "IF", (), {}),
    ("sort_values", "IFS", (), {}),
    ("sort_values", "IFS", (), {"ascending": False}),
    ("sort_values", "IFS", (), {"na_position": "first"}),
    ("sort_index", "IFS", (), {}),
    ("nlargest", "IF", (2,), {}),
    ("nsmallest", "IF", (2,), {}),
    ("nlargest", "IF", (), {"n": 3, "keep": "all"}),
    ("head", "IFSE", (2,), {}),
    ("head", "IF", (), {}),
    ("head", "IF", (-1,), {}),
    ("tail", "IFSE", (2,), {}),
    ("tail", "IF", (-1,), {}),
    ("take", "IFS", ([0, 2],), {}),
    ("searchsorted", "F", (1.0,), {}),
    ("factorize", "IS", (), {}),
    ("to_list", "IFSB", (), {}),
    ("tolist", "IFSB", (), {}),
    ("to_dict", "IF", (), {}),
    ("to_frame", "IFS", (), {}),
    ("to_frame", "IF", ("x",), {}),
    ("to_numpy", "F", (), {}),
    ("item", "I", (), {}),
    ("explode", "IFS", (), {}),
    ("reset_index", "IF", (), {"drop": True}),
    ("rename", "IF", ("z",), {}),
    ("pipe", "IF", (R(lambda r: lambda s: s),), {}),
    ("equals", "IFS", (SELF,), {}),
    ("compare", "IF", (OTHER,), {}),
    ("combine_first", "IF", (SELF,), {}),
    ("repeat", "IFS", (2,), {}),
    ("truncate", "IF", (), {"before": 1, "after": 3}),
    ("get", "IFS", (1,), {}),
    ("apply", "IF", (R(lambda r: lambda x: x + 1),), {}),
    ("map", "IF", (R(lambda r: lambda x: x + 1),), {}),
    ("transform", "IF", ("abs",), {}),
    ("agg", "IF", ("sum",), {}),
    ("agg", "IF", (["sum", "min"],), {}),
    ("aggregate", "IF", ("mean",), {}),
    ("rolling", "F", (2,), {}),
    # arithmetic and comparison
    *[
        (m, "IF", (2,), {})
        for m in (
            "add", "sub", "mul", "div", "truediv", "floordiv", "mod", "pow",
            "radd", "rsub", "rmul", "rdiv", "rtruediv", "rfloordiv", "rmod", "rpow",
            "divide", "multiply", "subtract",
        )
    ],  # fmt: skip
    *[(m, "IF", (OTHER,), {}) for m in ("add", "sub", "mul", "div", "floordiv", "mod", "pow")],
    ("add", "IF", (2,), {"fill_value": 0}),
    ("add", "IF", (OTHER,), {"fill_value": 0}),
    ("divmod", "IF", (2,), {}),
    ("rdivmod", "IF", (2,), {}),
    *[(m, "IFS", (2,), {}) for m in ("eq", "ne")],
    *[(m, "IF", (2,), {}) for m in ("lt", "le", "gt", "ge")],
    *[(m, "IF", (OTHER,), {}) for m in ("eq", "ne", "lt", "le", "gt", "ge")],
    # attributes
    *[(m, "IFSBE", None, {}) for m in ("shape", "size", "ndim", "empty", "dtype", "name")],
    ("hasnans", "IFS", None, {}),
    ("is_unique", "IFS", None, {}),
    ("is_monotonic_increasing", "IFS", None, {}),
    ("is_monotonic_decreasing", "IFS", None, {}),
    ("values", "IFS", None, {}),
    ("nbytes", "IFSBE", None, {}),
]  # fmt: skip

FRAME_CASES = [
    *[(m, "NE", (), {}) for m in ("sum", "mean", "median", "min", "max", "std", "var", "prod")],
    ("sum", "N", (), {"skipna": False}),
    ("sum", "N", (), {"min_count": 1}),
    ("sum", "N", (), {"axis": 1}),
    ("mean", "N", (), {"axis": 1}),
    ("max", "N", (), {"axis": 1}),
    ("sum", "M", (), {"numeric_only": True}),
    ("std", "N", (), {"ddof": 0}),
    ("var", "N", (), {"ddof": 0}),
    ("sem", "N", (), {}),
    ("skew", "N", (), {}),
    ("kurt", "N", (), {}),
    *[(m, "X", (), {}) for m in ("sum", "mean", "count", "isna", "dropna")],
    ("count", "MNE", (), {}),
    ("count", "M", (), {"axis": 1}),
    ("nunique", "M", (), {}),
    ("nunique", "M", (), {"dropna": False}),
    ("any", "M", (), {}),
    ("all", "M", (), {}),
    ("quantile", "N", (0.5,), {}),
    ("quantile", "N", ([0.25, 0.75],), {}),
    ("idxmax", "N", (), {}),
    ("idxmin", "N", (), {}),
    ("describe", "N", (), {}),
    ("cov", "N", (), {}),
    ("corr", "N", (), {}),
    ("corrwith", "N", (SELF,), {}),
    ("convert_dtypes", "MNW", (), {}),
    ("convert_dtypes", "W", (), {"convert_integer": False}),
    ("infer_objects", "MNW", (), {}),
    ("abs", "N", (), {}),
    ("round", "N", (), {}),
    ("round", "N", (1,), {}),
    ("clip", "N", (), {"lower": -1, "upper": 2}),
    ("cumsum", "N", (), {}),
    ("cumprod", "N", (), {}),
    ("cummax", "N", (), {}),
    ("cummin", "N", (), {}),
    ("diff", "N", (), {}),
    ("diff", "N", (), {"periods": 2}),
    ("shift", "MN", (1,), {}),
    ("shift", "N", (-1,), {}),
    ("shift", "N", (), {"periods": 2, "fill_value": 0}),
    ("pct_change", "N", (), {"fill_method": None}),
    ("rank", "N", (), {}),
    ("rank", "N", (), {"method": "min", "ascending": False}),
    ("fillna", "N", (0,), {}),
    ("fillna", "N", (), {"value": 7}),
    ("ffill", "N", (), {}),
    ("bfill", "N", (), {}),
    ("interpolate", "N", (), {}),
    ("isna", "M", (), {}),
    ("isnull", "M", (), {}),
    ("notna", "M", (), {}),
    ("notnull", "M", (), {}),
    ("dropna", "M", (), {}),
    ("dropna", "M", (), {"how": "all"}),
    ("dropna", "M", (), {"subset": ["a"]}),
    ("dropna", "M", (), {"axis": 1}),
    ("isin", "N", ([1, 2],), {}),
    ("where", "N", (R(lambda r: r["b"] > 0),), {}),
    ("where", "N", (R(lambda r: r["b"] > 0), -1), {}),
    ("mask", "N", (R(lambda r: r["b"] > 0), -1), {}),
    ("replace", "N", (1, 100), {}),
    ("replace", "M", ("x", "X"), {}),
    ("astype", "N", ("float64",), {}),
    ("astype", "N", ({"a": "float64"},), {}),
    ("copy", "MNE", (), {}),
    ("duplicated", "M", (), {}),
    ("duplicated", "M", (), {"subset": ["c"], "keep": "last"}),
    ("drop_duplicates", "M", (), {}),
    ("drop_duplicates", "M", (), {"subset": ["c"], "keep": "last"}),
    ("value_counts", "M", (), {}),
    ("mode", "N", (), {}),
    ("sort_values", "M", ("a",), {}),
    ("sort_values", "M", (), {"by": ["c", "a"], "ascending": [True, False]}),
    ("sort_values", "M", ("a",), {"na_position": "first"}),
    ("sort_index", "M", (), {}),
    ("nlargest", "N", (2, "a"), {}),
    ("nsmallest", "N", (2, "a"), {}),
    ("head", "MNE", (2,), {}),
    ("head", "M", (), {}),
    ("tail", "MNE", (2,), {}),
    ("take", "M", ([0, 2],), {}),
    ("drop", "M", (), {"columns": ["a"]}),
    ("drop", "M", (["a", "b"],), {"axis": 1}),
    ("drop", "M", ("a",), {"axis": 1}),
    ("rename", "M", (), {"columns": {"a": "A"}}),
    ("filter", "M", (), {"items": ["a", "c"]}),
    ("filter", "M", (), {"like": "a"}),
    ("filter", "M", (), {"regex": "^[ab]$"}),
    ("assign", "N", (), {"z": R(lambda r: r["a"] * 2)}),
    ("insert", "N", (1, "z", 5), {}),
    ("melt", "N", (), {"id_vars": ["a"]}),
    ("explode", "M", ("c",), {}),
    ("select_dtypes", "M", (), {"include": "number"}),
    ("select_dtypes", "M", (), {"exclude": "number"}),
    ("reset_index", "M", (), {"drop": True}),
    ("to_dict", "N", (), {}),
    ("to_dict", "N", (), {"orient": "list"}),
    ("to_dict", "N", (), {"orient": "records"}),
    ("to_numpy", "N", (), {}),
    ("to_records", "N", (), {"index": False}),
    ("pipe", "M", (R(lambda r: lambda d: d),), {}),
    ("equals", "MNE", (SELF,), {}),
    ("compare", "N", (R(lambda r: r.copy()),), {}),
    ("combine_first", "N", (SELF,), {}),
    ("update", "N", (SELF,), {}),
    ("transpose", "N", (), {}),
    ("squeeze", "N", (), {}),
    ("apply", "N", (R(lambda r: lambda x: x + 1),), {}),
    ("apply", "N", ("sum",), {}),
    ("map", "N", (R(lambda r: lambda x: x + 1),), {}),
    ("transform", "N", ("abs",), {}),
    ("agg", "N", ("sum",), {}),
    ("agg", "N", (["sum", "min"],), {}),
    ("agg", "N", ({"a": "sum", "b": "max"},), {}),
    ("aggregate", "N", ("mean",), {}),
    ("query", "N", ("a > 1",), {}),
    ("eval", "N", ("a + b",), {}),
    ("merge", "G", (R(lambda r: r),), {"on": "k"}),
    ("sample", "N", (), {"n": 2, "random_state": 1}),
    ("pivot_table", "G", (), {"index": "k", "values": "a", "aggfunc": "sum"}),
    ("get", "M", ("a",), {}),
    ("pop", "M", ("a",), {}),
    ("first_valid_index", "N", (), {}),
    ("last_valid_index", "N", (), {}),
    ("add_prefix", "N", ("p_",), {}),
    ("add_suffix", "N", ("_s",), {}),
    ("memory_usage", "N", (), {"index": False}),
    ("info", "N", (), {}),
    ("to_markdown", "N", (), {}),
    ("to_string", "N", (), {}),
    ("to_json", "N", (), {}),
    ("to_csv", "N", (), {"index": False}),
    ("to_html", "N", (), {}),
    ("ewm", "N", (), {"alpha": 0.5}),
    ("expanding", "N", (), {}),
    ("rolling", "N", (2,), {}),
    ("resample", "N", ("2D",), {}),
    *[
        (m, "N", (2,), {})
        for m in (
            "add", "sub", "mul", "div", "truediv", "floordiv", "mod", "pow",
            "radd", "rsub", "rmul", "rdiv", "rtruediv", "rfloordiv", "rmod", "rpow",
            "divide", "multiply", "subtract",
        )
    ],  # fmt: skip
    ("add", "N", (), {"other": SELF}),
    ("add", "N", (2,), {"fill_value": 0}),
    *[(m, "N", (2,), {}) for m in ("eq", "ne", "lt", "le", "gt", "ge")],
    *[(m, "N", (SELF,), {}) for m in ("eq", "ne", "lt", "le", "gt", "ge")],
    ("dot", "N", (R(lambda r: r.transpose()),), {}),
    *[(m, "MNE", None, {}) for m in ("shape", "size", "ndim", "empty", "columns", "dtypes")],
    ("values", "N", None, {}),
    ("T", "N", None, {}),
    ("axes", "N", None, {}),
    ("index", "N", None, {}),
]  # fmt: skip

GROUPBY_CASES = [
    *[
        (m, "G", (), {})
        for m in ("sum", "mean", "median", "min", "max", "std", "var", "prod", "count", "size",
                  "first", "last", "nunique", "sem", "skew", "idxmax", "idxmin", "any", "all")
    ],  # fmt: skip
    ("sum", "G", (), {"min_count": 1}),
    ("std", "G", (), {"ddof": 0}),
    ("quantile", "G", (0.5,), {}),
    ("describe", "G", (), {}),
    ("agg", "G", ("sum",), {}),
    ("agg", "G", (["sum", "min"],), {}),
    ("agg", "G", ({"a": "sum", "b": "max"},), {}),
    ("aggregate", "G", ("mean",), {}),
    ("cumsum", "G", (), {}),
    ("cumprod", "G", (), {}),
    ("cummax", "G", (), {}),
    ("cummin", "G", (), {}),
    ("cumcount", "G", (), {}),
    ("ngroup", "G", (), {}),
    ("shift", "G", (1,), {}),
    ("diff", "G", (), {}),
    ("pct_change", "G", (), {"fill_method": None}),
    ("rank", "G", (), {}),
    ("ffill", "G", (), {}),
    ("bfill", "G", (), {}),
    ("head", "G", (1,), {}),
    ("tail", "G", (1,), {}),
    ("nth", "G", (0,), {}),
    ("transform", "G", ("sum",), {}),
    ("transform", "G", ("max",), {}),
    ("filter", "G", (R(lambda g: lambda d: len(d) > 2),), {}),
    ("apply", "G", (R(lambda g: lambda d: d),), {}),
    ("get_group", "G", ("x",), {}),
    ("value_counts", "G", (), {}),
    ("ngroups", "G", None, {}),
    ("groups", "G", None, {}),
    ("indices", "G", None, {}),
]  # fmt: skip

STR_CASES = [
    ("lower", "S", (), {}),
    ("upper", "S", (), {}),
    ("len", "S", (), {}),
    ("strip", "S", (), {}),
    ("contains", "S", ("a",), {}),
    ("contains", "S", ("A",), {"case": False}),
    ("contains", "S", ("a.",), {"regex": False}),
    ("startswith", "S", ("a",), {}),
    ("endswith", "S", ("b",), {}),
    ("replace", "S", ("a", "x"), {}),
    ("replace", "S", ("a", "x"), {"regex": False}),
    ("replace", "S", ("^a", "x"), {"regex": True}),
    ("split", "S", ("b",), {}),
    ("find", "S", ("a",), {}),
    ("slice", "S", (0, 1), {}),
    ("get", "S", (0,), {}),
    ("pad", "S", (4,), {}),
    ("zfill", "S", (4,), {}),
    ("count", "S", ("a",), {}),
    ("repeat", "S", (2,), {}),
    ("capitalize", "S", (), {}),
    ("title", "S", (), {}),
    ("isalpha", "S", (), {}),
    ("isdigit", "S", (), {}),
    ("center", "S", (5,), {}),
    ("ljust", "S", (5,), {}),
    ("rjust", "S", (5,), {}),
    ("removeprefix", "S", ("a",), {}),
    ("removesuffix", "S", ("b",), {}),
    ("cat", "S", (), {"sep": "-", "na_rep": "?"}),
    ("join", "S", ("-",), {}),
    ("match", "S", ("a",), {}),
    ("fullmatch", "S", ("a",), {}),
    ("extract", "S", ("(a)",), {"expand": False}),
    ("findall", "S", ("a",), {}),
    ("get_dummies", "S", (), {}),
]  # fmt: skip

FINDINGS = load_findings("parity_findings_pandas.txt", pd.__version__)


def _build_params():
    params = []
    count = {}
    for kind, cases in (
        ("series", SERIES_CASES),
        ("frame", FRAME_CASES),
        ("groupby", GROUPBY_CASES),
        ("str", STR_CASES),
    ):
        for method, letters, args, kwargs in cases:
            key = f"{kind}.{method}"
            count[key] = count.get(key, 0) + 1
            for letter in letters:
                name = f"{key}#{count[key]}[{letter}]"
                marks = []
                if name in FINDINGS:
                    marks.append(pytest.mark.xfail(reason=FINDINGS[name], strict=True))
                params.append(
                    pytest.param(kind, method, letter, args, kwargs, marks=marks, id=name)
                )
    return params


PARAMS = _build_params()


def _receiver(kind, letter, side):
    table = SERIES if kind in ("series", "str") else FRAMES
    return table[letter][1 if side == "ours" else 2]()


def _run(kind, method, letter, args, kwargs, side):
    recv = _receiver(kind, letter, side)
    kwargs = dict(kwargs)
    if kind == "groupby":
        recv = recv.groupby("k")
    target = recv.str if kind == "str" else recv
    # An argument built from the receiver sees the frame or series itself.
    base = _receiver(kind, letter, side) if kind == "groupby" else recv
    if args is None:
        return getattr(target, method)
    a = [resolve(x, base) for x in args]
    kw = {k: resolve(v, base) for k, v in kwargs.items()}
    return getattr(target, method)(*a, **kw)


@pytest.mark.parametrize("kind,method,letter,args,kwargs", PARAMS)
def test_matches_pandas(kind, method, letter, args, kwargs):
    theirs_err = ours_err = None
    try:
        theirs = _run(kind, method, letter, args, kwargs, "pandas")
    except Exception as e:  # noqa: BLE001
        theirs_err = e
    try:
        ours = _run(kind, method, letter, args, kwargs, "ours")
    except Exception as e:  # noqa: BLE001
        ours_err = e
    if theirs_err is not None:
        if ours_err is None:
            pytest.fail(f"[raises] pandas raised {type(theirs_err).__name__}, ours returned")
        if not isinstance(ours_err, type(theirs_err)):
            pytest.fail(
                f"[raises] pandas raised {type(theirs_err).__name__}, "
                f"ours {type(ours_err).__name__}: {ours_err}"
            )
        return
    if ours_err is not None:
        pytest.fail(f"[error] ours raised {type(ours_err).__name__}: {ours_err}")
    if kind == "groupby" and isinstance(theirs, (pd.Series, pd.DataFrame)):
        # Ours has no row index: its group keys are columns, which is what
        # pandas gives with as_index=False.
        if not isinstance(theirs.index, pd.RangeIndex):
            theirs = theirs.reset_index()
    diff = compare(ours, theirs, norm_pandas(pd, theirs))
    assert diff is None, diff


def test_every_finding_names_a_case():
    cases = {p.id for p in PARAMS}
    assert not (set(FINDINGS) - cases), sorted(set(FINDINGS) - cases)
