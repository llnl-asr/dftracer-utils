# SPDX-License-Identifier: MIT
"""The alias tables (python/dftracer/utils/_aliases.py): every alias resolves,
an identity alias is the canonical attribute itself, an adapter is a method of
its class, and every pandas alias keeps its pandas keywords and gives what
pandas gives on the same data. The polars aliases are compared with polars."""

import importlib.util
from pathlib import Path
from typing import Any, Callable, Dict, List, Tuple

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import Expr, GroupBy  # noqa: E402
from dftracer.utils._lazyframe import LazyGroupBy  # noqa: E402

pd = pytest.importorskip("pandas")

_ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location(
    "dftu_aliases", _ROOT / "python" / "dftracer" / "utils" / "_aliases.py"
)
assert _spec is not None and _spec.loader is not None
_aliases = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_aliases)

CLASSES = {
    "Series": dft.Series,
    "Expr": Expr,
    "DataFrame": dft.DataFrame,
    "LazyFrame": dft.LazyFrame,
    "GroupBy": GroupBy,
    "LazyGroupBy": LazyGroupBy,
}
ROWS = list(_aliases.ALIASES)
IDS = [f"{r.cls}.{r.alias}" for r in ROWS]


@pytest.mark.parametrize("row", ROWS, ids=IDS)
def test_every_alias_resolves_to_its_target(row):
    cls = CLASSES[row.cls]
    assert hasattr(cls, row.target), f"{row.cls}.{row.alias}: no target {row.target}"
    if row.adapter:
        assert hasattr(cls, row.alias), (
            f"{row.cls}.{row.alias} is an adapter, a method of the class"
        )
        assert getattr(cls, row.alias) is not getattr(cls, row.target)
    else:
        assert getattr(cls, row.alias) is getattr(cls, row.target)


def test_the_canonical_names_follow_polars():
    for cls_name, canonical in {
        "Series": {
            "fill_null",
            "is_null",
            "is_not_null",
            "cast",
            "is_in",
            "drop_nulls",
            "n_unique",
        },
        "DataFrame": {"fill_null", "cast", "drop_nulls", "group_by", "sort", "unique", "gather"},
        "LazyFrame": {"sort", "unique", "gather", "head"},
        "Expr": {"fill_null", "is_null", "is_not_null", "cast", "is_in", "cum_sum", "forward_fill"},
    }.items():
        for name in canonical:
            assert hasattr(CLASSES[cls_name], name), (cls_name, name)
            assert name not in {a.alias for a in ROWS if a.cls == cls_name and a.face == "pandas"}
    assert dft.Series.is_null.__doc__ == "Bool mask: row is null."


def test_a_removed_spelling_is_gone():
    for cls, name in (
        (dft.trace.TraceViewer, "query"),
        (dft.trace.TraceViewer, "explain_query"),
        (dft.trace.TraceViewer, "limit"),
        (dft.Series, "skewness"),
        (dft.Series, "_null_mask"),
        (dft.Series, "_valid_mask"),
    ):
        assert not hasattr(cls, name), (cls, name)
    import dftracer.utils._expr as expr

    assert not hasattr(expr, "Field") and not hasattr(expr, "ColumnExpr")


def test_series_is_null_is_a_mask_and_is_null_at_is_a_bool():
    s = dft.Series([1, None])
    assert s.is_null().to_list() == [False, True]
    assert s.is_null_at(1) is True and s.is_null_at(0) is False
    with pytest.raises(TypeError):
        s.is_null(0)


def test_series_cast_is_one_method_taking_a_dtype():
    s = dft.Series([1, 2])
    assert s.cast("float64").to_list() == s.cast(dft.DType.FLOAT64).to_list() == [1.0, 2.0]
    assert s.cast(int(dft.DType.FLOAT64)).to_list() == [1.0, 2.0]
    assert "cast" in vars(dft.Series)
    assert dft.Series.astype is dft.Series.cast


# ---------------------------------------------------------------------------
# pandas parity of every pandas alias: the same call with pandas keywords on
# the same data.


def _norm(v: Any) -> Any:
    if isinstance(v, float) and v != v:
        return None
    if hasattr(v, "item") and not isinstance(v, (list, str, bytes)):
        return _norm(v.item())
    return v


def _series_list(s: Any) -> List[Any]:
    values = s.to_list() if isinstance(s, dft.Series) else s.tolist()
    return [_norm(v) for v in values]


def _frame_dict(df: Any) -> Dict[str, List[Any]]:
    if isinstance(df, dft.LazyFrame):
        df = df.collect()
    if isinstance(df, dft.DataFrame):
        return {c: _series_list(df[c]) for c in df.columns}
    return {c: _series_list(df[c]) for c in df.columns}


X = [3, None, 1, 2, None, 5]
S_NUM = lambda: dft.Series(X, name="x")  # noqa: E731
P_NUM = lambda: pd.Series([float("nan") if v is None else v for v in X], name="x")  # noqa: E731
SORTED = [1, 2, 4, 8]


def _same(ours: Any, theirs: Any) -> None:
    assert _norm_any(ours) == _norm_any(theirs), (ours, theirs)


def _norm_any(v: Any) -> Any:
    if isinstance(v, (dft.Series,)):
        return _series_list(v)
    if isinstance(v, (dft.DataFrame, dft.LazyFrame)):
        return _frame_dict(v)
    if isinstance(v, pd.Series):
        return _series_list(v)
    if isinstance(v, pd.DataFrame):
        return _frame_dict(v.reset_index(drop=True))
    if isinstance(v, list):
        return [_norm_any(x) for x in v]
    return _norm(v)


# alias name -> (call on the ours Series, call on the pandas Series)
SERIES_CASES: Dict[str, Tuple[Callable, Callable]] = {
    "astype": (lambda s: s.astype("float64"), lambda s: s.astype("float64")),
    "isna": (lambda s: s.isna(), lambda s: s.isna()),
    "isnull": (lambda s: s.isnull(), lambda s: s.isnull()),
    "notna": (lambda s: s.notna(), lambda s: s.notna()),
    "notnull": (lambda s: s.notnull(), lambda s: s.notnull()),
    "dropna": (lambda s: s.dropna(), lambda s: s.dropna()),
    "fillna": (lambda s: s.fillna(value=0), lambda s: s.fillna(value=0)),
    "isin": (lambda s: s.isin([1, 2]), lambda s: s.isin([1, 2])),
    "nunique": (lambda s: s.nunique(), lambda s: s.nunique()),
    "prod": (lambda s: s.prod(), lambda s: s.prod()),
    "argmax": (lambda s: s.dropna().argmax(), lambda s: s.dropna().argmax()),
    "argmin": (lambda s: s.dropna().argmin(), lambda s: s.dropna().argmin()),
    "idxmax": (lambda s: s.dropna().idxmax(), lambda s: s.dropna().reset_index(drop=True).idxmax()),
    "idxmin": (lambda s: s.dropna().idxmin(), lambda s: s.dropna().reset_index(drop=True).idxmin()),
    "cumsum": (lambda s: s.cumsum(), lambda s: s.cumsum()),
    "cumprod": (lambda s: s.cumprod(), lambda s: s.cumprod()),
    "cummax": (lambda s: s.cummax(), lambda s: s.cummax()),
    "cummin": (lambda s: s.cummin(), lambda s: s.cummin()),
    "tolist": (lambda s: s.tolist(), lambda s: s.tolist()),
    "ffill": (lambda s: s.ffill(), lambda s: s.ffill()),
    "pad": (lambda s: s.pad(), lambda s: s.ffill()),
    "bfill": (lambda s: s.bfill(), lambda s: s.bfill()),
    "backfill": (lambda s: s.backfill(), lambda s: s.bfill()),
    "take": (lambda s: s.take([2, 0, 0]), lambda s: s.take([2, 0, 0]).reset_index(drop=True)),
    "sort_values": (
        lambda s: s.sort_values(ascending=False),
        lambda s: s.sort_values(ascending=False).reset_index(drop=True),
    ),
    "nlargest": (lambda s: s.nlargest(2), lambda s: s.nlargest(2).reset_index(drop=True)),
    "nsmallest": (lambda s: s.nsmallest(2), lambda s: s.nsmallest(2).reset_index(drop=True)),
    "aggregate": (lambda s: s.aggregate("sum"), lambda s: s.aggregate("sum")),
    "kurt": (lambda s: s.dropna().kurt(), lambda s: float(s.dropna().kurt())),
    "divide": (lambda s: s.divide(2).to_list(), lambda s: s.divide(2).tolist()),
    "truediv": (lambda s: s.truediv(2).to_list(), lambda s: s.truediv(2).tolist()),
    "multiply": (lambda s: s.multiply(2).to_list(), lambda s: s.multiply(2).tolist()),
    "subtract": (lambda s: s.subtract(2).to_list(), lambda s: s.subtract(2).tolist()),
    "rtruediv": (lambda s: s.rtruediv(2).to_list(), lambda s: s.rtruediv(2).tolist()),
    "searchsorted": (None, None),
    "iat": (lambda s: s.iat[3], lambda s: s.iat[3]),
    "loc": (lambda s: s.loc[3], lambda s: s.loc[3]),
    "at": (lambda s: s.at[3], lambda s: s.at[3]),
    "div": (lambda s: s.div(2).to_list(), lambda s: s.div(2).tolist()),
    "drop_duplicates": (
        lambda s: s.drop_duplicates().to_list(),
        lambda s: s.drop_duplicates().tolist(),
    ),
    "between": (
        lambda s: s.dropna().between(1, 3).to_list(),
        lambda s: s.dropna().between(1, 3).tolist(),
    ),
}


def _pandas_has(cls: type, name: str) -> bool:
    return hasattr(cls, name)


@pytest.mark.parametrize(
    "alias", [r.alias for r in ROWS if r.cls == "Series" and r.face == "pandas"]
)
def test_a_series_alias_gives_what_pandas_gives_for_the_pandas_keywords(alias):
    case = SERIES_CASES.get(alias)
    assert case is not None, f"no pandas case for Series.{alias}"
    if alias == "searchsorted":
        ours = dft.Series(SORTED).searchsorted(dft.Series([2, 5, 9]))
        theirs = pd.Series(SORTED).searchsorted([2, 5, 9])
        assert _series_list(ours) == [int(v) for v in theirs]
        return
    if not _pandas_has(pd.Series, alias):
        pytest.skip(f"pandas {pd.__version__} has no Series.{alias}")
    ours = case[0](S_NUM())
    theirs = case[1](P_NUM())
    if alias == "kurt":
        assert ours == pytest.approx(theirs, rel=1e-9)
        return
    _same(ours, theirs)


F_DATA = {"k": ["a", "b", "a", "b", "c"], "a": [3, 1, 2, 5, None], "b": [1.5, 2.5, None, 4.5, 0.5]}


def _pdf() -> Any:
    return pd.DataFrame(
        {k: [float("nan") if v is None else v for v in vals] for k, vals in F_DATA.items()}
    )


def _ours() -> Any:
    return dft.DataFrame(F_DATA)


def _sorted_dict(d: Dict[str, List[Any]], key: str) -> Dict[str, List[Any]]:
    order = sorted(range(len(d[key])), key=lambda i: (d[key][i] is None, d[key][i]))
    return {c: [v[i] for i in order] for c, v in d.items()}


FRAME_CASES: Dict[str, Tuple[Callable, Callable]] = {
    "astype": (lambda f: f.astype({"a": "float64"}), lambda f: f.astype({"a": "float64"})),
    "dropna": (lambda f: f.dropna(), lambda f: f.dropna()),
    "fillna": (lambda f: f.select("a", "b").fillna(0), lambda f: f[["a", "b"]].fillna(0)),
    "sort_values": (
        lambda f: f.sort_values("a", ascending=False),
        lambda f: f.sort_values("a", ascending=False),
    ),
    "drop_duplicates": (
        lambda f: f.drop_duplicates(subset=["k"]),
        lambda f: f.drop_duplicates(subset=["k"]),
    ),
    "take": (lambda f: f.take([4, 0]), lambda f: f.take([4, 0])),
    "nlargest": (lambda f: f.nlargest(2, "a"), lambda f: f.nlargest(2, "a")),
    "nsmallest": (lambda f: f.nsmallest(2, "a"), lambda f: f.nsmallest(2, "a")),
    "divide": (lambda f: f.select("a", "b").divide(2), lambda f: f[["a", "b"]].divide(2)),
    "truediv": (lambda f: f.select("a", "b").truediv(2), lambda f: f[["a", "b"]].truediv(2)),
    "multiply": (lambda f: f.select("a", "b").multiply(2), lambda f: f[["a", "b"]].multiply(2)),
    "subtract": (lambda f: f.select("a", "b").subtract(2), lambda f: f[["a", "b"]].subtract(2)),
    "rtruediv": (lambda f: f.select("a", "b").rtruediv(2), lambda f: f[["a", "b"]].rtruediv(2)),
    "isnull": (lambda f: f.select("a", "b").isnull(), lambda f: f[["a", "b"]].isnull()),
    "notnull": (lambda f: f.select("a", "b").notnull(), lambda f: f[["a", "b"]].notnull()),
    "product": (
        lambda f: f.select("a", "b").product(),
        lambda f: f[["a", "b"]].prod().to_frame().T,
    ),
    "aggregate": (
        lambda f: f.select("a", "b").aggregate("sum"),
        lambda f: f[["a", "b"]].aggregate("sum").to_frame().T,
    ),
    "groupby": (
        lambda f: f.select("k", "a").groupby("k").sum().reset_index(),
        lambda f: f[["k", "a"]].groupby("k").sum().reset_index(),
    ),
    "backfill": (lambda f: f.select("a", "b").backfill(), lambda f: f[["a", "b"]].bfill()),
    "pad": (lambda f: f.select("a", "b").pad(), lambda f: f[["a", "b"]].ffill()),
    "map": (lambda f: f.select("a", "b").map(lambda v: v), None),
}


@pytest.mark.parametrize(
    "alias", [r.alias for r in ROWS if r.cls == "DataFrame" and r.face == "pandas"]
)
def test_a_dataframe_alias_gives_what_pandas_gives_for_the_pandas_keywords(alias):
    case = FRAME_CASES.get(alias)
    assert case is not None, f"no pandas case for DataFrame.{alias}"
    if alias == "map":
        out = _ours().select("a", "b").map(lambda v: v)
        assert _frame_dict(out) == _frame_dict(_ours().select("a", "b"))
        return
    if not hasattr(pd.DataFrame, alias):
        pytest.skip(f"pandas {pd.__version__} has no DataFrame.{alias}")
    ours = case[0](_ours())
    theirs = case[1](_pdf())
    _same(ours, theirs)


LAZY_CASES: Dict[str, Tuple[Callable, Callable]] = {
    "sort_values": (
        lambda f: f.sort_values("a", ascending=False),
        lambda f: f.sort_values("a", ascending=False),
    ),
    "drop_duplicates": (
        lambda f: f.drop_duplicates(subset=["k"]),
        lambda f: f.drop_duplicates(subset=["k"]),
    ),
    "take": (lambda f: f.take([4, 0]), lambda f: f.take([4, 0])),
    "aggregate": (
        lambda f: f.select("a", "b").aggregate("sum"),
        lambda f: f[["a", "b"]].aggregate("sum").to_frame().T,
    ),
}


@pytest.mark.parametrize(
    "alias", [r.alias for r in ROWS if r.cls == "LazyFrame" and r.face == "pandas"]
)
def test_a_lazyframe_alias_gives_what_pandas_gives_for_the_pandas_keywords(alias):
    ours = LAZY_CASES[alias][0](_ours().lazy())
    theirs = LAZY_CASES[alias][1](_pdf())
    _same(ours, theirs)


EXPR_CASES: Dict[str, Callable] = {
    "isna": lambda e: e.isna(),
    "isnull": lambda e: e.isnull(),
    "notna": lambda e: e.notna(),
    "notnull": lambda e: e.notnull(),
    "isin": lambda e: e.isin([1, 2]),
    "astype": lambda e: e.astype("float64"),
    "fillna": lambda e: e.fillna(0),
    "cumsum": lambda e: e.cumsum(),
    "cumprod": lambda e: e.cumprod(),
    "cummax": lambda e: e.cummax(),
    "cummin": lambda e: e.cummin(),
    "ffill": lambda e: e.ffill(),
    "bfill": lambda e: e.bfill(),
    "between": lambda e: e.between(1, 3),
    "startswith": None,
    "endswith": None,
}
PANDAS_OF_EXPR: Dict[str, Callable] = {
    "isna": lambda s: s.isna(),
    "isnull": lambda s: s.isnull(),
    "notna": lambda s: s.notna(),
    "notnull": lambda s: s.notnull(),
    "isin": lambda s: s.isin([1, 2]),
    "astype": lambda s: s.astype("float64"),
    "fillna": lambda s: s.fillna(0),
    "cumsum": lambda s: s.cumsum(),
    "cumprod": lambda s: s.cumprod(),
    "cummax": lambda s: s.cummax(),
    "cummin": lambda s: s.cummin(),
    "ffill": lambda s: s.ffill(),
    "bfill": lambda s: s.bfill(),
    "between": lambda s: s.between(1, 3),
}


@pytest.mark.parametrize("alias", [r.alias for r in ROWS if r.cls == "Expr"])
def test_an_expr_alias_gives_what_pandas_gives_for_the_pandas_keywords(alias):
    frame = dft.DataFrame({"x": X, "s": ["ab", "b", None, "abc", "a", "ba"]})
    if alias in ("startswith", "endswith"):
        words = ["ab", "b", None, "abc", "a", "ba"]
        ours = getattr(dft.col("s"), alias)("a").apply(frame).to_list()
        theirs = getattr(pd.Series(words).str, alias)("a").tolist()
        present = [i for i, w in enumerate(words) if w is not None]
        assert [ours[i] for i in present] == [bool(theirs[i]) for i in present]
        assert ours[2] is None
        return
    ours = EXPR_CASES[alias](dft.col("x")).apply(frame)
    theirs = PANDAS_OF_EXPR[alias](P_NUM())
    if alias == "between":
        # pandas compares a NaN as false; a null stays null here.
        present = [i for i, v in enumerate(X) if v is not None]
        got = ours.to_list()
        assert [got[i] for i in present] == [bool(theirs.tolist()[i]) for i in present]
        assert [got[i] for i in range(len(X)) if i not in present] == [None, None]
        return
    _same(ours, theirs)


@pytest.mark.parametrize("cls", ["GroupBy"])
def test_the_groupby_aliases_give_what_pandas_gives(cls):
    ours = _ours().groupby("k")
    theirs = _pdf().groupby("k")
    got = _frame_dict(ours.aggregate("sum").reset_index())
    want = _frame_dict(theirs.aggregate("sum").reset_index())
    assert _sorted_dict(got, "k") == _sorted_dict(want, "k")
    got = _frame_dict(ours.product().reset_index())
    want = _frame_dict(theirs.prod().reset_index())
    assert _sorted_dict(got, "k") == _sorted_dict(want, "k")


def test_the_polars_aliases_give_what_polars_gives():
    pl = pytest.importorskip("polars")
    ours = _ours()
    theirs = pl.DataFrame(F_DATA)
    assert _frame_dict(ours.limit(2)) == {c: theirs.limit(2)[c].to_list() for c in theirs.columns}
    assert _frame_dict(ours.lazy().limit(2).collect()) == {
        c: theirs.lazy().limit(2).collect()[c].to_list() for c in theirs.columns
    }
    assert dft.DataFrame.limit is dft.DataFrame.head
    assert dft.LazyFrame.limit is dft.LazyFrame.head


def test_the_docs_page_lists_every_alias():
    page = (_ROOT / "docs" / "source" / "guides" / "data" / "pandas-polars.rst").read_text(
        encoding="utf-8"
    )
    for row in ROWS:
        assert f"``{row.alias}``" in page, f"{row.cls}.{row.alias} is not in pandas-polars.rst"
        assert f"``{row.target}``" in page, f"{row.cls}.{row.target} is not in pandas-polars.rst"
