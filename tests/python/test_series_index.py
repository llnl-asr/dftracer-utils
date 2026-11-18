# SPDX-License-Identifier: MIT
"""The row labels of a Series: each case runs on real pandas and on ours with
the labels from ``Series.from_pandas`` and compares the values, the name, the
labels and the level names of the result."""

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning")

NAN = float("nan")


def _s(values, index, name, level_names=None):
    if isinstance(index, pd.MultiIndex):
        return pd.Series(values, index=index, name=name)
    return pd.Series(values, index=pd.Index(index, name=level_names), name=name)


PI = _s([3.0, 1.0, NAN, 2.0, 2.0], [30, 10, 20, 40, 50], "v", "k")
PI2 = _s([1.0, 2.0, 3.0, 4.0], [10, 20, 30, 60], "w", "k")
PS = _s([5.0, 6.0, 7.0], ["b", "a", "c"], "x", "s")
PM = _s(
    [1.0, 2.0, 3.0, 4.0],
    pd.MultiIndex.from_tuples([("a", 1), ("a", 2), ("b", 1), ("b", 2)], names=["g", "n"]),
    "m",
)
PM2 = _s(
    [10.0, 20.0, 30.0],
    pd.MultiIndex.from_tuples([("a", 2), ("b", 1), ("c", 1)], names=["g", "n"]),
    "m2",
)
PDUP = _s([1.0, 2.0, 3.0], [1, 1, 2], "d1", "d")
PDUP2 = _s([10.0, 20.0, 30.0], [1, 2, 2], "d2", "d")
PINT = _s([1, 2, 3, 4], [10, 20, 30, 40], "n", "k")
PINT2 = _s([5, 6, 7], [20, 30, 50], "n2", "k")
PPOS = pd.Series([1.0, 2.0, 3.0], name="p")
PSTR = _s(["b", "a", "b", "c", "a"], [1, 2, 3, 4, 5], "t", "k")


def ours(p):
    return Series.from_pandas(p)


def _result(r):
    r = r.to_pandas() if hasattr(r, "to_pandas") else r
    return r


def _run(fn, *inputs):
    results = []
    for build in (ours, lambda p: p):
        try:
            results.append(_result(fn(*[build(i) for i in inputs])))
        except Exception as e:  # noqa: BLE001 - the type is compared
            results.append(e)
    return results


# Which name the aligned labels take when the two sides' names differ changed in
# pandas 3 (the left side's; pandas 2 gives None). The engine follows pandas 3.
PANDAS3 = int(pd.__version__.split(".")[0]) >= 3
LEFT_NAME_CASES = {"labelled and positional"}


def _same(x, y, names=True):
    if isinstance(y, Exception):
        assert isinstance(x, Exception), f"ours returned, pandas raised {y!r}"
        assert isinstance(x, type(y)) or type(y).__name__ in ("ValueError", "KeyError")
        return
    assert not isinstance(x, Exception), f"ours raised {x!r}"
    if isinstance(y, pd.DataFrame):
        assert list(x.index.names) == list(y.index.names)
        assert x.index.tolist() == y.index.tolist()
        assert list(x.columns) == list(y.columns)
        pd.testing.assert_frame_equal(
            x, y, check_dtype=False, check_index_type=False, check_names=False
        )
    elif isinstance(y, pd.Series):
        if names:
            assert list(x.index.names) == list(y.index.names)
        assert x.index.tolist() == y.index.tolist()
        pd.testing.assert_series_equal(
            x, y, check_dtype=False, check_index_type=False, check_names=False
        )
        assert x.name == y.name
    else:
        assert x == y or (x != x and y != y)


def _mask(s):
    return s[s > 1.5] if isinstance(s, pd.Series) else s.filter(s > 1.5)


def _axes(s):
    index = s.axes[0]
    return index.to_series(index=range(len(s))) if isinstance(s, pd.Series) else index


def _unique(s):
    values = s.unique() if isinstance(s, pd.Series) else s.unique().to_list()
    return pd.Series(sorted(values, key=lambda v: (v is None or v != v, v)))


def _iloc_reverse(s):
    return s.iloc[::-1] if isinstance(s, pd.Series) else s.reverse()


CASES = [
    # the round trip through pandas keeps the labels and their names
    ("roundtrip single", lambda s: s, (PI,)),
    ("roundtrip multi", lambda s: s, (PM,)),
    ("roundtrip str", lambda s: s, (PS,)),
    ("roundtrip unindexed", lambda s: s, (PPOS,)),
    # the label accessors
    ("reset_index", lambda s: s.reset_index(), (PI,)),
    ("reset_index multi", lambda s: s.reset_index(), (PM,)),
    ("reset_index drop", lambda s: s.reset_index(drop=True), (PI,)),
    ("reset_index name", lambda s: s.reset_index(name="z"), (PI,)),
    ("reset_index level", lambda s: s.reset_index(level="g"), (PM,)),
    ("reset_index unindexed", lambda s: s.reset_index(), (PPOS,)),
    ("to_frame", lambda s: s.to_frame(), (PI,)),
    ("to_frame multi", lambda s: s.to_frame("q"), (PM,)),
    ("set_axis", lambda s: s.set_axis(["p", "q", "r", "s", "t"]), (PI,)),
    ("set_axis unindexed", lambda s: s.set_axis([7, 8, 9]), (PPOS,)),
    ("set_axis length", lambda s: s.set_axis([1, 2]), (PPOS,)),
    ("rename_axis", lambda s: s.rename_axis("z"), (PI,)),
    ("rename_axis multi", lambda s: s.rename_axis(["a", "b"]), (PM,)),
    ("rename_axis none", lambda s: s.rename_axis(None), (PI,)),
    ("rename_axis index dict", lambda s: s.rename_axis(index={"k": "z"}), (PI,)),
    ("axes", _axes, (PI,)),
    # ops that keep or move the rows keep the labels with them
    ("sort_values", lambda s: s.sort_values(), (PI,)),
    ("sort_values desc", lambda s: s.sort_values(ascending=False), (PI,)),
    ("sort_values multi", lambda s: s.sort_values(), (PM,)),
    ("head", lambda s: s.head(2), (PI,)),
    ("tail", lambda s: s.tail(2), (PI,)),
    ("take", lambda s: s.take([3, 0]), (PI,)),
    ("dropna", lambda s: s.dropna(), (PI,)),
    ("fillna", lambda s: s.fillna(0.0), (PI,)),
    ("cumsum", lambda s: s.cumsum(), (PI,)),
    ("shift", lambda s: s.shift(1), (PI,)),
    ("diff", lambda s: s.diff(), (PI,)),
    ("rank", lambda s: s.rank(), (PI,)),
    ("astype", lambda s: s.astype("float32"), (PI,)),
    ("mul scalar", lambda s: s * 2, (PI,)),
    ("add scalar", lambda s: s + 1.5, (PI,)),
    ("abs", lambda s: s.abs(), (PI,)),
    ("drop_duplicates", lambda s: s.drop_duplicates(), (PI,)),
    ("nlargest", lambda s: s.nlargest(2), (PI,)),
    ("nsmallest", lambda s: s.nsmallest(2), (PI,)),
    ("filter by mask", _mask, (PI,)),
    ("str upper", lambda s: s.str.upper(), (PSTR,)),
    ("isna", lambda s: s.isna(), (PI,)),
    ("clip", lambda s: s.clip(1.0, 2.5), (PI,)),
    ("round", lambda s: s.round(), (PI,)),
    ("repeat", lambda s: s.repeat(2), (PI,)),
    ("truncate by label", lambda s: s.truncate(before=20, after=40), (PINT,)),
    ("truncate unsorted raises", lambda s: s.truncate(before=1, after=3), (PI,)),
    ("reverse", _iloc_reverse, (PI,)),
    ("unique has no labels", _unique, (PI,)),
    # reducers give no labels
    ("sum", lambda s: s.sum(), (PI,)),
    ("mean", lambda s: s.mean(), (PI,)),
    # arithmetic aligns on the labels
    ("add equal labels", lambda a, b: a + b, (PI, PI.rename("zz"))),
    ("add differ", lambda a, b: a + b, (PI, PI2)),
    ("sub differ", lambda a, b: a - b, (PI, PI2)),
    ("mul differ", lambda a, b: a * b, (PI, PI2)),
    ("div differ", lambda a, b: a / b, (PI, PI2)),
    ("floordiv differ", lambda a, b: a // b, (PI, PI2)),
    ("pow differ", lambda a, b: a**b, (PI, PI2)),
    ("add method", lambda a, b: a.add(b), (PI, PI2)),
    ("integer add differ", lambda a, b: a + b, (PINT, PINT2)),
    ("multi level align", lambda a, b: a + b, (PM, PM2)),
    ("duplicate labels", lambda a, b: a + b, (PDUP, PDUP2)),
    ("positional pair", lambda a, b: a + b, (PPOS, PPOS)),
    ("labelled and positional", lambda a, b: a + b, (PINT, pd.Series([1, 2, 3, 4], name="r"))),
    ("positional and labelled", lambda a, b: a + b, (pd.Series([1, 2, 3, 4], name="r"), PINT)),
    ("compare equal labels", lambda a, b: a == b, (PINT, PINT.rename("zz"))),
    ("compare other labels raises", lambda a, b: a == b, (PINT, PINT2)),
    ("order other labels raises", lambda a, b: a < b, (PINT, PINT2)),
    ("and equal labels", lambda a, b: (a > 1) & (b > 1), (PINT, PINT.rename("zz"))),
]


@pytest.mark.parametrize("label,fn,inputs", CASES, ids=[c[0] for c in CASES])
def test_matches_pandas(label, fn, inputs):
    mine, theirs = _run(fn, *inputs)
    _same(mine, theirs, names=PANDAS3 or label not in LEFT_NAME_CASES)


def test_frame_column_carries_the_index():
    pdf = pd.DataFrame(
        {"a": [1.0, 2.0, 3.0], "b": [4.0, 5.0, 6.0]}, index=pd.Index([7, 8, 9], name="k")
    )
    df = DataFrame.from_pandas(pdf)
    _same(df["a"].to_pandas(), pdf["a"])
    _same(df["b"].sort_values(ascending=False).to_pandas(), pdf["b"].sort_values(ascending=False))
    _same((df["a"] + df["b"]).to_pandas(), pdf["a"] + pdf["b"])
    multi = pdf.set_index("a", append=True)
    _same(DataFrame.from_pandas(multi)["b"].to_pandas(), multi["b"])
    _same(df.set_index("b")["a"].to_pandas(), pdf.set_index("b")["a"])


def test_frame_column_of_a_frame_with_no_index_has_no_labels():
    labelled = Series.from_pandas(PI).head(2)
    frame = DataFrame.from_dict({"x": labelled})
    assert frame["x"].index_names() == []


def test_labels_follow_the_native_ops_too():
    s = Series.from_pandas(PI)
    assert s.index_names() == ["k"]
    assert s.sort().index.to_list() == [20, 10, 40, 50, 30]  # nulls first
    assert s.head(2).index.to_list() == [30, 10]
    assert s.filter(s > 1.5).index.to_list() == [30, 40, 50]
    assert s.sum() == 8.0
    assert s.unique().index_names() == []
    assert s.rename("z").index_names() == ["k"]
    assert s.share().index_names() == ["k"]
    assert s[1:3].index.to_list() == [10, 20]


def test_pickling_keeps_the_values_and_the_labels():
    import pickle

    s = Series.from_pandas(PI)
    back = pickle.loads(pickle.dumps(s))
    assert back.to_list() == s.to_list()
    assert back.index_names() == ["k"]
    assert back.index.to_list() == s.index.to_list()
    assert pickle.loads(pickle.dumps(Series.from_pandas(PM))).index_names() == ["g", "n"]
    assert pickle.loads(pickle.dumps(Series([1.0]))).index_names() == []


def test_positional_methods_label_by_position_like_pandas():
    s = Series([3.0, 1.0, 2.0])
    assert s.index_names() == []
    assert s.index.to_list() == [0, 1, 2]
    out = s.sort_values()
    assert out.index.to_list() == [1, 2, 0]
    assert s.tail(2).index.to_list() == [1, 2]
    assert s.take([2, 2, 0]).index.to_list() == [2, 2, 0]
    assert s.sort().index_names() == []


def test_labels_that_cannot_align_raise():
    a = Series.from_pandas(PI)
    b = Series.from_pandas(PS)
    with pytest.raises(ValueError, match="cannot align"):
        a + b
    with pytest.raises(ValueError, match="identically-labeled"):
        assert Series.from_pandas(PINT) == Series.from_pandas(PINT2)


def _values(s):
    return [None if v != v else v for v in s.to_pandas().tolist()]


def test_null_labels_align_with_each_other():
    p1 = pd.Series([1.0, 2.0], index=[1.0, NAN], name="v")
    p2 = pd.Series([10.0, 20.0], index=[NAN, 2.0], name="v")
    want = p1 + p2
    got = (ours(p1) + ours(p2)).to_pandas()
    assert len(got) == len(want) == 3
    assert got.index.isna().tolist() == want.index.isna().tolist()
    assert got.index.tolist()[:2] == want.index.tolist()[:2]
    assert got.isna().tolist() == want.isna().tolist()
    assert got.iloc[2] == want.iloc[2] == 12.0


def test_null_string_labels_align_with_each_other():
    p1 = pd.Series([1.0, 2.0], index=["a", None], name="v")
    p2 = pd.Series([10.0, 20.0], index=[None, "b"], name="v")
    want = p1 + p2
    got = (ours(p1) + ours(p2)).to_pandas()
    assert len(got) == len(want) == 3
    assert got.index.isna().tolist() == want.index.isna().tolist()
    assert got.isna().tolist() == want.isna().tolist()
    assert got.iloc[2] == want.iloc[2] == 12.0


@pytest.mark.parametrize("dtype", ["int32", "uint8", "float64", "int64"])
def test_numeric_labels_of_other_types_align(dtype):
    p = pd.Series([1, 2, 3], index=pd.Index([2, 1, 0], dtype=dtype), name="v")
    q = pd.Series([10, 20, 30], name="v")
    mine, theirs = _run(lambda a, b: a + b, p, q)
    _same(mine, theirs, names=False)
    both = pd.Series([4, 5], index=pd.Index([1, 2], dtype="uint16"), name="v")
    want = p + both
    got = (ours(p) + ours(both)).to_pandas()
    assert got.index.tolist() == want.index.tolist()
    assert _values(ours(p) + ours(both)) == [None if v != v else v for v in want.tolist()]


def test_numeric_labels_against_float_labels_align():
    p = pd.Series([1, 2], index=[1, 2], name="v")
    q = pd.Series([3, 4], index=[1.0, 3.0], name="v")
    want = p + q
    got = (ours(p) + ours(q)).to_pandas()
    assert got.index.tolist() == want.index.tolist()
    assert got.isna().tolist() == want.isna().tolist()


def test_string_labels_do_not_meet_number_labels():
    a = Series.from_pandas(pd.Series([1, 2], index=["a", "b"], name="v"))
    b = Series.from_pandas(pd.Series([3, 4], index=[1, 2], name="v"))
    with pytest.raises(ValueError, match="cannot align"):
        a + b


@pytest.mark.parametrize("op", [lambda a, b: a & b, lambda a, b: a | b], ids=["and", "or"])
def test_bool_across_labels_stays_bool(op):
    p = pd.Series([True, False, True], index=[1, 2, 3], name="v")
    q = pd.Series([True, True], index=[2, 3], name="v")
    want = op(p, q)
    got = op(ours(p), ours(q))
    assert got.dtype.name.lower() in ("bool", "boolean")
    assert got.to_pandas().tolist() == want.tolist()
    assert got.to_pandas().index.tolist() == want.index.tolist()


@pytest.mark.parametrize("ascending", [True, False])
@pytest.mark.parametrize("na_position", ["last", "first"])
def test_sort_index_orders_by_the_labels(ascending, na_position):
    p = pd.Series([1.0, 2.0, 3.0, 4.0, 5.0], index=[30.0, 10.0, NAN, 20.0, 40.0], name="v")
    want = p.sort_index(ascending=ascending, na_position=na_position)
    got = ours(p).sort_index(ascending=ascending, na_position=na_position).to_pandas()
    assert got.tolist() == want.tolist()
    assert got.index.isna().tolist() == want.index.isna().tolist()


def test_sort_index_strings_and_multiindex():
    want = PS.sort_index()
    got = ours(PS).sort_index().to_pandas()
    assert got.tolist() == want.tolist()
    assert got.index.tolist() == want.index.tolist()
    mi = pd.MultiIndex.from_tuples([("b", 2), ("a", 2), ("b", 1), ("a", 1)], names=["g", "n"])
    p = pd.Series([1.0, 2.0, 3.0, 4.0], index=mi, name="m")
    for ascending in (True, False):
        want = p.sort_index(ascending=ascending)
        got = ours(p).sort_index(ascending=ascending).to_pandas()
        assert got.tolist() == want.tolist()
        assert got.index.tolist() == want.index.tolist()


def test_sort_index_of_a_positional_series_is_unchanged():
    s = Series([1.0, 2.0, 3.0])
    assert s.sort_index().to_list() == [1.0, 2.0, 3.0]
    assert s.sort_index(ascending=False).to_list() == [3.0, 2.0, 1.0]


def test_positional_lookup_without_labels_keeps_working():
    s = Series([3.0, 1.0, 2.0])
    assert s.loc[1] == 1.0
    assert s.at[2] == 2.0
    assert s[0] == 3.0
    assert s.loc[0:1].to_list() == [3.0, 1.0]


def test_where_and_mask_need_the_same_labels():
    a = ours(pd.Series([1.0, 2.0, 3.0], index=[1, 2, 3], name="v"))
    other = ours(pd.Series([0.0, 0.0, 0.0], index=[4, 5, 6], name="v"))
    with pytest.raises(ValueError, match="row labels"):
        a.where(a > 1.5, other)
    kept = a.where(a > 1.5, 0.0)
    assert kept.to_pandas().index.tolist() == [1, 2, 3]
    assert a.mask(a > 1.5).to_pandas().index.tolist() == [1, 2, 3]
    assert a.where(a > 1.5, a * 0.0).to_list() == [0.0, 2.0, 3.0]


def test_str_cat_needs_the_same_labels():
    a = ours(pd.Series(["a", "b"], index=[1, 2], name="s"))
    b = ours(pd.Series(["c", "d"], index=[2, 3], name="s"))
    with pytest.raises(ValueError, match="row labels"):
        a.str.cat(b)
    joined = a.str.cat(a, sep="-")
    assert joined.to_list() == ["a-a", "b-b"]
    assert joined.to_pandas().index.tolist() == [1, 2]
    assert a.str.cat("x").to_pandas().index.tolist() == [1, 2]


@pytest.mark.parametrize("labelled", [False, True])
def test_a_mask_with_nulls_keeps_only_the_true_rows(labelled):
    p = pd.Series([3.0, NAN, 2.0, 0.5], name="v")
    if labelled:
        p.index = pd.Index([30, 10, 20, 40], name="k")
    want = p[p > 1]
    s = ours(p)
    got = s[s > 1]
    assert got.to_list() == want.tolist()
    if labelled:
        assert got.to_pandas().index.tolist() == want.index.tolist()
    assert s.filter(s > 1).to_list() == want.tolist()


def test_a_mask_with_nulls_filters_a_frame():
    df = DataFrame({"a": [3.0, None, 2.0], "b": [1, 2, 3]})
    assert df[df["a"] > 1]["b"].to_list() == [1, 3]
    assert df.filter(df["a"] > 1)["b"].to_list() == [1, 3]
    with pytest.raises(TypeError, match="Bool"):
        ours(PI)[ours(PI)]


def test_bool_alignment_fills_only_the_added_rows():
    a = ours(pd.Series([True, None, False], index=[1, 2, 3], dtype="boolean", name="v"))
    b = ours(pd.Series([True, True, True], index=[2, 3, 4], name="v"))
    both = (a & b).to_pandas()
    assert both.index.tolist() == [1, 2, 3, 4]
    # an added row is false; the null the side already had stays null
    assert both.isna().tolist() == [False, True, False, False]
    assert both.dropna().tolist() == [False, False, False]
    either = (a | b).to_pandas()
    assert either.isna().tolist() == [False, False, False, False]
    assert either.tolist() == [True, True, True, True]
