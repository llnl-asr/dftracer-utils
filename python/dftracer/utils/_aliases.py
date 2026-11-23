# SPDX-License-Identifier: MIT
"""The alias tables: every other spelling of a method, from one place.

The canonical names follow Polars. Each class is built with its canonical
methods only; :func:`install` then gives it the pandas and Polars spellings
listed here, so no class holds a hand-written wrapper that only renames.

An alias is the canonical attribute under another name, the same object, unless
it is flagged ``adapter``: then its keywords or its result differ from the
canonical method (a pandas ``sort_values(ascending=...)`` over
``sort(descending=...)``), and the class body defines the small function for it
under the alias name. The adapters are the only hand-written alias code.

``scripts/gen_pyi.py`` writes the same identity aliases into each class body
under ``if TYPE_CHECKING:``, so a type checker sees them.
"""

from typing import Dict, Iterator, Mapping, NamedTuple, Tuple


class Alias(NamedTuple):
    cls: str
    alias: str
    target: str
    face: str
    adapter: bool = False


def _pandas(cls: str, pairs: Mapping[str, str], adapters: Tuple[str, ...] = ()) -> Iterator[Alias]:
    for alias, target in pairs.items():
        yield Alias(cls, alias, target, "pandas", alias in adapters)


def _polars(cls: str, pairs: Mapping[str, str], adapters: Tuple[str, ...] = ()) -> Iterator[Alias]:
    for alias, target in pairs.items():
        yield Alias(cls, alias, target, "polars", alias in adapters)


_SERIES = {
    "aggregate": "agg",
    "astype": "cast",
    "isnull": "isna",
    "notnull": "notna",
    "dropna": "drop_nulls",
    "fillna": "fill_null",
    "isin": "is_in",
    "nunique": "n_unique",
    "prod": "product",
    "argmax": "arg_max",
    "idxmax": "arg_max",
    "argmin": "arg_min",
    "idxmin": "arg_min",
    "cumsum": "cum_sum",
    "cumprod": "cum_prod",
    "cummax": "cum_max",
    "cummin": "cum_min",
    "searchsorted": "search_sorted",
    "tolist": "to_list",
    "ffill": "forward_fill",
    "pad": "forward_fill",
    "bfill": "backward_fill",
    "backfill": "backward_fill",
    "kurt": "kurtosis",
    "take": "gather",
    "iat": "iloc",
    "at": "iloc",
    "loc": "iloc",
    "rtruediv": "rdiv",
    "divide": "truediv",
    "div": "truediv",
    "multiply": "mul",
    "subtract": "sub",
    "sort_values": "sort",
    "nlargest": "top_k",
    "nsmallest": "bottom_k",
    "drop_duplicates": "unique",
    "between": "is_between",
}

_EXPR = {
    "isnull": "isna",
    "notnull": "notna",
    "isin": "is_in",
    "astype": "cast",
    "fillna": "fill_null",
    "cumsum": "cum_sum",
    "cumprod": "cum_prod",
    "cummax": "cum_max",
    "cummin": "cum_min",
    "ffill": "forward_fill",
    "bfill": "backward_fill",
    "between": "is_between",
    "startswith": "starts_with",
    "endswith": "ends_with",
}

_DATAFRAME = {
    "aggregate": "agg",
    "astype": "cast",
    "dropna": "drop_nulls",
    "fillna": "fill_null",
    "groupby": "group_by",
    "sort_values": "sort",
    "drop_duplicates": "unique",
    "take": "gather",
    "nlargest": "top_k",
    "nsmallest": "top_k",
    "divide": "div",
    "truediv": "div",
    "multiply": "mul",
    "subtract": "sub",
    "product": "prod",
    "rtruediv": "rdiv",
    "map": "applymap",
    "isnull": "isna",
    "notnull": "notna",
    "backfill": "bfill",
    "pad": "ffill",
}

_LAZYFRAME = {
    "aggregate": "agg",
    "sort_values": "sort",
    "drop_duplicates": "unique",
    "take": "gather",
}

_GROUPBY = {"aggregate": "agg", "product": "prod"}

ADAPTERS = {
    "Series": (
        "sort_values",
        "nlargest",
        "nsmallest",
        "kurt",
        "drop_duplicates",
        "dropna",
        "fillna",
        "take",
        "between",
        "loc",
        "at",
    ),
    "Expr": ("between", "fillna"),
    "DataFrame": (
        "groupby",
        "sort_values",
        "nlargest",
        "nsmallest",
        "dropna",
        "fillna",
        "drop_duplicates",
        "take",
    ),
    "LazyFrame": ("sort_values",),
}

PANDAS: Tuple[Alias, ...] = (
    *_pandas("Series", _SERIES, ADAPTERS["Series"]),
    *_pandas("Expr", _EXPR, ADAPTERS["Expr"]),
    *_pandas("DataFrame", _DATAFRAME, ADAPTERS["DataFrame"]),
    *_pandas("LazyFrame", _LAZYFRAME, ADAPTERS["LazyFrame"]),
    *_pandas("GroupBy", _GROUPBY),
    *_pandas("LazyGroupBy", _GROUPBY),
)

POLARS: Tuple[Alias, ...] = (
    *_polars(
        "Series",
        {"alias": "rename", "clone": "copy", "limit": "head", "not_": "__invert__"},
    ),
    *_polars(
        "DataFrame",
        {
            "limit": "head",
            "clone": "copy",
            "select_seq": "select",
            "with_columns_seq": "with_columns",
        },
    ),
    *_polars(
        "LazyFrame",
        {"limit": "head", "select_seq": "select", "with_columns_seq": "with_columns"},
    ),
)

ALIASES: Tuple[Alias, ...] = PANDAS + POLARS


def install(name: str, cls: type, faces: Tuple[str, ...] = ("pandas", "polars")) -> None:
    """Give class ``cls`` (called ``name`` in the tables) its aliases of the
    given ``faces``: each identity alias becomes the canonical attribute under
    the alias name; an adapter must already be defined in the class body (a
    class that borrows another's table, as TraceViewer borrows LazyFrame's,
    needs the adapter as well)."""
    for row in ALIASES:
        if row.cls != name or row.face not in faces:
            continue
        if row.adapter:
            if row.alias not in dir(cls):
                raise TypeError(f"{name}.{row.alias} is an adapter alias; define it in the class")
        else:
            setattr(cls, row.alias, getattr(cls, row.target))


def aliases_of(name: str) -> Dict[str, Alias]:
    return {row.alias: row for row in ALIASES if row.cls == name}
