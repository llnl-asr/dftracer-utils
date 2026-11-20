# SPDX-License-Identifier: MIT
"""`.str`, `.dt` and `.list` on a Series and on an Expr agree: a polars name
gives polars results, a pandas name gives pandas results, the shared names take
the same keywords, and a column the method does not fit raises one error type.
Each result is compared with real pandas, and with real polars where it is
installed."""

import datetime as dtm
from typing import Any, Callable, Dict, List, Tuple

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import col
from dftracer.utils.errors import DFTUtilsValueError

pd = pytest.importorskip("pandas")

STRINGS = ["abc", " Hello World ", None, "", "a,b,c", "ABC def", "12", "  "]
STAMPS = [
    dtm.datetime(2024, 2, 29, 13, 5, 7, 123456),
    None,
    dtm.datetime(1970, 1, 1),
    dtm.datetime(1999, 12, 31, 23, 59, 59),
    dtm.datetime(2021, 7, 4),
    dtm.datetime(2020, 12, 31, 8, 30),
    dtm.datetime(2021, 1, 1, 0, 0, 1),
]


def _str_series() -> dft.Series:
    return dft.Series(STRINGS, name="x")


def _ts_series() -> dft.Series:
    return dft.Series(STAMPS, name="x")


def _lists(values: Any) -> List[Any]:
    out = []
    for v in values:
        if v is None or (isinstance(v, float) and v != v) or v is pd.NaT:
            out.append(None)
        elif isinstance(v, pd.Timestamp):
            out.append(v.to_pydatetime())
        elif hasattr(v, "item") and not isinstance(v, (list, str)):
            out.append(v.item())
        else:
            out.append(v)
    return out


def _on_expr(series: dft.Series, build: Callable[[Any], Any]) -> List[Any]:
    return build(col("x")).apply(dft.DataFrame({"x": series})).to_list()


def test_the_new_series_dt_methods_match_pandas_and_the_expr_methods():
    s = _ts_series()
    p = pd.Series([pd.Timestamp(v) if v is not None else pd.NaT for v in STAMPS])
    iso = p.dt.isocalendar()
    assert _lists(s.dt.ordinal_day().to_list()) == _lists(p.dt.dayofyear.astype("float"))
    assert s.dt.week().to_list() == _lists(iso["week"].astype("float"))
    assert s.dt.iso_year().to_list() == _lists(iso["year"].astype("float"))
    assert s.dt.epoch("s").to_list() == [
        None if v is None else int(pd.Timestamp(v).timestamp() // 1) for v in STAMPS
    ]
    assert s.dt.truncate("1h").to_list() == _lists(p.dt.floor("1h"))
    for name in ("ordinal_day", "week", "iso_year"):
        assert getattr(s.dt, name)().to_list() == _on_expr(s, lambda e, n=name: getattr(e.dt, n)())
    assert s.dt.epoch("ms").to_list() == _on_expr(s, lambda e: e.dt.epoch("ms"))
    assert s.dt.truncate("5min").to_list() == _on_expr(s, lambda e: e.dt.truncate("5min"))


def test_the_series_dt_pandas_names_are_properties_and_the_polars_names_methods():
    s = _ts_series()
    p = pd.Series([pd.Timestamp(v) if v is not None else pd.NaT for v in STAMPS])
    for name in ("year", "month", "day", "hour", "minute", "second", "weekday", "dayofweek"):
        got = getattr(s.dt, name)
        assert isinstance(got, dft.Series), name
        assert got.to_list() == _lists(getattr(p.dt, name).astype("float")), name
    for name in ("epoch", "iso_year", "ordinal_day", "truncate", "week"):
        assert callable(getattr(s.dt, name)) and not isinstance(getattr(s.dt, name), dft.Series)


def test_weekday_is_zero_based_on_a_series_and_one_based_on_an_expr():
    s = _ts_series()
    p = pd.Series([pd.Timestamp(v) if v is not None else pd.NaT for v in STAMPS])
    zero = _lists(p.dt.weekday.astype("float"))
    assert s.dt.weekday.to_list() == zero
    assert s.dt.dayofweek.to_list() == s.dt.day_of_week.to_list() == zero
    assert _on_expr(s, lambda e: e.dt.dayofweek()) == zero
    assert _on_expr(s, lambda e: e.dt.day_of_week()) == zero
    assert _on_expr(s, lambda e: e.dt.weekday()) == [None if v is None else v + 1 for v in zero]


def test_the_dt_methods_expr_gained_match_the_series_properties_and_pandas():
    s = _ts_series()
    p = pd.Series([pd.Timestamp(v) if v is not None else pd.NaT for v in STAMPS])
    for name in (
        "is_month_start",
        "is_month_end",
        "is_year_start",
        "is_year_end",
        "is_quarter_start",
        "is_quarter_end",
        "days_in_month",
        "daysinmonth",
        "dayofyear",
        "day_of_year",
    ):
        want = getattr(s.dt, name).to_list()
        assert _on_expr(s, lambda e, n=name: getattr(e.dt, n)()) == want, name
    present = [i for i, v in enumerate(STAMPS) if v is not None]
    for name in (
        "is_month_start",
        "is_month_end",
        "is_year_start",
        "is_year_end",
        "is_quarter_start",
        "is_quarter_end",
    ):
        ours = getattr(s.dt, name).to_list()
        theirs = getattr(p.dt, name).to_list()
        assert [ours[i] for i in present] == [theirs[i] for i in present], name
    for name in ("floor", "ceil", "round"):
        want = getattr(s.dt, name)("1h").to_list()
        assert _on_expr(s, lambda e, n=name: getattr(e.dt, n)("1h")) == want
        assert want == _lists(getattr(p.dt, name)("1h")), name
    assert _on_expr(s, lambda e: e.dt.normalize()) == s.dt.normalize().to_list()
    assert _lists(s.dt.normalize().to_list()) == _lists(p.dt.normalize())


def test_total_seconds_agrees_on_a_duration_and_an_int_column():
    durations = [dtm.timedelta(seconds=90, microseconds=5), None, dtm.timedelta(days=2)]
    s = dft.Series(durations, name="x")
    assert s.dt.total_seconds().to_list() == [90.000005, None, 172800.0]
    got = _on_expr(s, lambda e: e.dt.total_seconds())
    assert got[1] is None
    assert [got[0], got[2]] == pytest.approx([90.000005, 172800.0])
    ints = dft.Series([1500, None], name="x")
    assert ints.dt("ms").total_seconds().to_list() == [1.5, None]
    assert _on_expr(ints, lambda e: e.dt("ms").total_seconds()) == [1.5, None]


def test_tz_localize_and_tz_convert_agree_across_series_and_expr():
    s = _ts_series()
    utc_series = s.dt.tz_localize("UTC")
    frame = dft.DataFrame({"x": s})
    utc_expr = col("x").dt.tz_localize("UTC").apply(frame)
    assert utc_series.timezone == utc_expr.timezone == "UTC"
    assert utc_series.to_list() == utc_expr.to_list()
    back = col("x").dt.tz_localize("UTC").dt.tz_convert(None).apply(frame)
    assert back.timezone == "" and back.to_list() == s.to_list()
    paris_series = s.dt.tz_localize("Europe/Paris")
    paris_expr = col("x").dt.tz_localize("Europe/Paris").apply(frame)
    assert paris_series.timezone == paris_expr.timezone == "Europe/Paris"
    assert paris_series.to_list() == paris_expr.to_list()
    assert paris_series.astype("int64").to_list() != utc_series.astype("int64").to_list()
    with pytest.raises(KeyError):
        s.dt.tz_localize("Nowhere/Land")
    with pytest.raises(KeyError):
        col("x").dt.tz_localize("Nowhere/Land")


def test_the_new_series_str_methods_match_pandas():
    s = _str_series()
    p = pd.Series(STRINGS)
    assert s.str.count_matches("a").to_list() == _lists(p.str.count("a").astype("float"))
    assert s.str.strip_chars_start().to_list() == _lists(p.str.lstrip())
    assert s.str.strip_chars_end().to_list() == _lists(p.str.rstrip())
    assert s.str.strip_prefix("a").to_list() == _lists(p.str.removeprefix("a"))
    assert s.str.strip_suffix("c").to_list() == _lists(p.str.removesuffix("c"))
    assert s.str.to_titlecase().to_list() == _lists(p.str.title())
    for name, args in (
        ("count_matches", ("a",)),
        ("strip_chars_start", ()),
        ("strip_chars_end", ()),
        ("strip_prefix", ("a",)),
        ("strip_suffix", ("c",)),
        ("to_titlecase", ()),
    ):
        assert getattr(s.str, name)(*args).to_list() == _on_expr(
            s, lambda e, n=name, a=args: getattr(e.str, n)(*a)
        ), name


def test_to_titlecase_on_an_expr_is_the_title_case_not_the_swapped_case():
    s = dft.Series(["aBc dEf", "x"], name="x")
    assert _on_expr(s, lambda e: e.str.to_titlecase()) == ["Abc Def", "X"]


def test_the_new_methods_match_polars():
    pl = pytest.importorskip("polars")
    s = _str_series()
    p = pl.Series("x", STRINGS)
    assert s.str.count_matches("a").to_list() == p.str.count_matches("a", literal=True).to_list()
    assert s.str.strip_chars_start().to_list() == p.str.strip_chars_start().to_list()
    assert s.str.strip_chars_end().to_list() == p.str.strip_chars_end().to_list()
    assert s.str.strip_prefix("a").to_list() == p.str.strip_prefix("a").to_list()
    assert s.str.strip_suffix("c").to_list() == p.str.strip_suffix("c").to_list()
    assert s.str.to_titlecase().to_list() == p.str.to_titlecase().to_list()
    t = _ts_series()
    q = pl.Series("x", STAMPS)
    assert t.dt.ordinal_day().to_list() == q.dt.ordinal_day().to_list()
    assert t.dt.week().to_list() == q.dt.week().to_list()
    assert t.dt.iso_year().to_list() == q.dt.iso_year().to_list()
    assert t.dt.epoch("s").to_list() == q.dt.epoch("s").to_list()
    assert t.dt.truncate("1h").to_list() == q.dt.truncate("1h").to_list()
    assert _on_expr(t, lambda e: e.dt.weekday()) == q.dt.weekday().to_list()
    assert _on_expr(t, lambda e: e.dt.epoch("ms")) == q.dt.epoch("ms").to_list()
    frame = pl.DataFrame({"x": STAMPS})
    for name in ("year", "month", "day", "hour", "ordinal_day", "iso_year", "week", "quarter"):
        got = _on_expr(t, lambda e, n=name: getattr(e.dt, n)())
        want = frame.select(getattr(pl.col("x").dt, name)())["x"].to_list()
        assert got == want, name


# (method, keywords valid for both sides). The keywords are those of
# Series.str; an Expr takes the same ones.
SHARED_KEYWORDS: List[Tuple[str, Dict[str, Any]]] = [
    ("contains", {"pat": "a"}),
    ("contains", {"pat": "A", "case": False}),
    ("contains", {"pat": "a.", "regex": True}),
    ("startswith", {"pat": "a"}),
    ("endswith", {"pat": "c"}),
    ("starts_with", {"prefix": "a"}),
    ("ends_with", {"suffix": "c"}),
    ("find", {"sub": "b"}),
    ("rfind", {"sub": "b"}),
    ("index", {"sub": ""}),
    ("count", {"pat": "a"}),
    ("replace", {"pat": "a", "repl": "z"}),
    ("replace", {"pat": "a", "repl": "z", "n": 1}),
    ("replace", {"pat": "a", "repl": "z", "n": -1}),
    ("replace_all", {"pat": "a", "repl": "z"}),
    ("regex_replace", {"pat": "a(.)", "to": "$1"}),
    ("slice", {"start": 1, "stop": 3}),
    ("slice", {"start": 1}),
    ("pad", {"width": 6, "side": "right", "fillchar": "*"}),
    ("pad_start", {"width": 6, "fill": "*"}),
    ("pad_end", {"width": 6, "fill": "*"}),
    ("zfill", {"width": 6}),
    ("ljust", {"width": 6, "fillchar": "*"}),
    ("rjust", {"width": 6, "fillchar": "*"}),
    ("center", {"width": 7, "fillchar": "*"}),
    ("repeat", {"repeats": 2}),
    ("extract", {"pat": "(a)", "group": 1}),
    ("fullmatch", {"pat": "[a-z]+"}),
    ("match", {"pat": "a"}),
    ("findall", {"pat": "[a-z]"}),
    ("removeprefix", {"prefix": "a"}),
    ("removesuffix", {"suffix": "c"}),
    ("encode", {"encoding": "utf-8", "errors": "strict"}),
    ("slice_replace", {"start": 1, "stop": 2, "repl": "q"}),
    ("get", {"i": 0}),
    ("normalize", {"form": "NFC"}),
    ("wrap", {"width": 3, "expand_tabs": False}),
    ("split", {"pat": ",", "expand": False}),
    ("rsplit", {"pat": ",", "expand": False}),
    ("partition", {"sep": ",", "expand": False}),
    ("rpartition", {"sep": ",", "expand": False}),
]

_NON_INDEX = {"index"}


@pytest.mark.parametrize(
    "method,kwargs", SHARED_KEYWORDS, ids=[f"{m}-{'-'.join(k)}" for m, k in SHARED_KEYWORDS]
)
def test_an_expr_str_method_takes_the_keywords_of_the_series_method(method, kwargs):
    s = dft.Series([v for v in STRINGS if v != ""] if method == "index" else STRINGS, name="x")
    if method == "index":
        kwargs = {"sub": "a"}
        s = dft.Series(["abc", "a", "bca"], name="x")
    want = getattr(s.str, method)(**kwargs)
    got = _on_expr(s, lambda e: getattr(e.str, method)(**kwargs))
    expected = want.to_list()
    if method == "find":
        # Expr is Polars-shaped: null for no match where the Series gives -1.
        expected = [None if v == -1 else v for v in expected]
    assert got == expected


def test_decode_and_its_errors_keyword_agree():
    s = dft.Series([b"abc", None, b"\xff\xfe"], name="x")
    assert s.str.decode("utf-8", errors="null").to_list() == ["abc", None, None]
    assert _on_expr(s, lambda e: e.str.decode("utf-8", errors="null")) == ["abc", None, None]
    with pytest.raises(ValueError, match="not valid utf-8"):
        s.str.decode("utf-8", errors="strict")
    with pytest.raises(NotImplementedError, match="no lazy form"):
        col("x").str.decode("utf-8", errors="strict")
    with pytest.raises(ValueError, match="errors must be"):
        col("x").str.decode("utf-8", errors="nope")


def test_dt_round_floor_and_ceil_take_freq_on_both_sides():
    s = _ts_series()
    for name in ("round", "floor", "ceil"):
        want = getattr(s.dt, name)(freq="1h").to_list()
        assert _on_expr(s, lambda e, n=name: getattr(e.dt, n)(freq="1h")) == want


def test_partition_shape_is_a_frame_on_a_series_and_a_list_column_on_an_expr():
    s = dft.Series(["a,b,c", "x", None], name="x")
    frame = s.str.partition(",")
    assert isinstance(frame, dft.DataFrame) and frame.columns == ["0", "1", "2"]
    assert frame["0"].to_list() == ["a", "x", None]
    flat = s.str.partition(",", expand=False)
    assert isinstance(flat, dft.Series)
    assert _on_expr(s, lambda e: e.str.partition(",")) == flat.to_list()
    p = pd.Series(["a,b,c", "x", None]).str.partition(",")
    for i, name in enumerate(("0", "1", "2")):
        assert frame[name].to_list() == _lists(p[i])
    for name in ("partition", "rpartition", "split", "rsplit"):
        args = {"sep": ","} if "partition" in name else {"pat": ","}
        with pytest.raises(NotImplementedError, match="expand=True"):
            getattr(col("x").str, name)(expand=True, **args)


def test_the_expr_methods_that_have_no_expression_form_say_so():
    with pytest.raises(NotImplementedError, match="get_dummies"):
        col("x").str.get_dummies()
    with pytest.raises(NotImplementedError, match="cat"):
        col("x").str.cat()
    s = dft.Series(["a", "b"], name="x")
    assert _on_expr(s, lambda e: e.str.cat("!", sep="-")) == s.str.cat("!", sep="-").to_list()


# Methods that need a string column, on a column of integers.
_NOT_STRING = [
    ("upper", {}),
    ("len_bytes", {}),
    ("contains", {"pat": "a"}),
    ("starts_with", {"prefix": "a"}),
    ("count_matches", {"pat": "a"}),
    ("strip_prefix", {"prefix": "a"}),
    ("find", {"sub": "a"}),
    ("pad_start", {"width": 4}),
    ("capitalize", {}),
    ("isalpha", {}),
    ("regex_replace", {"pat": "a", "to": "b"}),
    ("repeat", {"repeats": 2}),
]


@pytest.mark.parametrize("method,kwargs", _NOT_STRING, ids=[m for m, _ in _NOT_STRING])
def test_a_string_method_on_an_int_column_raises_one_error_type_on_both_sides(method, kwargs):
    ints = dft.Series([1, 2, None], name="x")
    with pytest.raises(DFTUtilsValueError):
        getattr(ints.str, method)(**kwargs)
    with pytest.raises(DFTUtilsValueError):
        _on_expr(ints, lambda e: getattr(e.str, method)(**kwargs))


@pytest.mark.parametrize("name", ["year", "month", "hour", "is_leap_year", "quarter"])
def test_a_dt_method_on_a_string_column_raises_one_error_type_on_both_sides(name):
    words = dft.Series(["a", "b"], name="x")
    with pytest.raises(DFTUtilsValueError):
        getattr(words.dt, name).to_list()
    with pytest.raises(DFTUtilsValueError):
        _on_expr(words, lambda e: getattr(e.dt, name)())


def test_a_list_method_on_a_non_list_column_raises_that_error_type_on_a_series():
    with pytest.raises(DFTUtilsValueError, match="not a List column"):
        dft.Series([1, 2]).list.len()
