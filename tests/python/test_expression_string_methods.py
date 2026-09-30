"""The expression forms of the eager ``Series.str`` methods: each gives the
eager result (values, nulls, types) and, where pandas has the method, pandas'."""

import random

import pytest

from dftracer.utils import DataFrame, Series, col
from dftracer.utils.enums import DType

_ALPHABET = "abcxyzABCXYZ0189 \t+-_,.|ab--,,"


def _rows(seed, n_extra=300):
    """Every length 0..130 in order, then random lengths; a null about 1 row in 7."""
    rng = random.Random(seed)
    lengths = list(range(131)) + [rng.randrange(300) for _ in range(n_extra)]
    out = []
    for n in lengths:
        out.append(
            None if rng.randrange(7) == 0 else "".join(rng.choice(_ALPHABET) for _ in range(n))
        )
    out[-1] = "q" * 129  # a long string ends the buffer
    return out


def _frame(seed=1):
    s = _rows(seed)
    t = _rows(seed + 100)[: len(s)]
    t += [None] * (len(s) - len(t))
    return DataFrame.from_dict({"s": s, "t": t[: len(s)]})


def _both(df, expr):
    """(eager frame, lazy frame) results of one expression."""
    eager = df.with_columns(out=expr)["out"].to_list()
    lazy = df.lazy().with_columns(out=expr).collect()["out"].to_list()
    assert eager == lazy
    return eager


# name -> (expression, eager Series.str call)
def _cases():
    c = col("s")
    return {
        "capitalize": (c.capitalize(), lambda s: s.str.capitalize()),
        "title": (c.title(), lambda s: s.str.title()),
        "swapcase": (c.swapcase(), lambda s: s.str.swapcase()),
        "casefold": (c.casefold(), lambda s: s.str.casefold()),
        "isalnum": (c.isalnum(), lambda s: s.str.isalnum()),
        "isalpha": (c.isalpha(), lambda s: s.str.isalpha()),
        "isdecimal": (c.isdecimal(), lambda s: s.str.isdecimal()),
        "isdigit": (c.isdigit(), lambda s: s.str.isdigit()),
        "islower": (c.islower(), lambda s: s.str.islower()),
        "isnumeric": (c.isnumeric(), lambda s: s.str.isnumeric()),
        "isspace": (c.isspace(), lambda s: s.str.isspace()),
        "istitle": (c.istitle(), lambda s: s.str.istitle()),
        "isupper": (c.isupper(), lambda s: s.str.isupper()),
        "zfill": (c.zfill(9), lambda s: s.str.zfill(9)),
        "pad": (c.pad(12, "right", "."), lambda s: s.str.pad(12, "right", ".")),
        "pad_start": (c.pad_start(11, "*"), lambda s: s.str.pad_start(11, "*")),
        "pad_end": (c.pad_end(11, "*"), lambda s: s.str.pad_end(11, "*")),
        "ljust": (c.ljust(10, "-"), lambda s: s.str.ljust(10, "-")),
        "rjust": (c.rjust(10, "-"), lambda s: s.str.rjust(10, "-")),
        "center": (c.center(13, "-"), lambda s: s.str.center(13, "-")),
        "removeprefix": (c.removeprefix("ab"), lambda s: s.str.removeprefix("ab")),
        "removesuffix": (c.removesuffix("b-"), lambda s: s.str.removesuffix("b-")),
        "repeat": (c.repeat(3), lambda s: s.str.repeat(3)),
        "slice_replace": (c.slice_replace(2, 5, "##"), lambda s: s.str.slice_replace(2, 5, "##")),
        "slice_replace_open": (
            c.slice_replace(1, None, "~"),
            lambda s: s.str.slice_replace(1, None, "~"),
        ),
        "split": (c.split(","), lambda s: s.str.split(",")),
        "rsplit": (c.rsplit(","), lambda s: s.str.rsplit(",")),
        "partition": (c.partition(","), lambda s: s.str.partition(",", expand=False)),
        "rpartition": (c.rpartition("ab"), lambda s: s.str.rpartition("ab", expand=False)),
        "extract": (c.extract("([a-z])([0-9])", 2), lambda s: s.str.extract("([a-z])([0-9])", 2)),
        "findall": (c.findall("[0-9]+"), lambda s: s.str.findall("[0-9]+")),
        "match": (c.match("[a-z]+"), lambda s: s.str.match("[a-z]+")),
        "rfind": (c.rfind("a"), lambda s: s.str.rfind("a")),
        "get": (c.get(2), lambda s: s.str.get(2)),
        "cat": (c.cat(col("t"), sep="/"), None),
        "join": (c.split(",").join("+"), lambda s: s.str.split(",").str.join("+")),
    }


@pytest.mark.parametrize("name", sorted(_cases()))
def test_expression_equals_the_eager_method(name):
    expr, eager = _cases()[name]
    df = _frame()
    got = _both(df, expr)
    if name == "cat":
        t = _rows(101)
        s = df["s"].to_list()
        want = [None if a is None or b is None else a + "/" + b for a, b in zip(s, t)]
    else:
        want = eager(df["s"]).to_list()
        want = [list(v) if isinstance(v, tuple) else v for v in want]
    assert got == want


def test_cat_matches_the_eager_cat_on_the_same_columns():
    df = _frame()
    want = df["s"].str.cat(df["t"], sep="/").to_list()
    assert _both(df, col("s").cat(col("t"), sep="/")) == want


@pytest.mark.parametrize(
    "name",
    sorted(
        set(_cases())
        - {"cat", "extract", "findall", "match", "slice_replace", "slice_replace_open"}
    ),
)
def test_expression_equals_pandas(name):
    pd = pytest.importorskip("pandas")
    expr, _ = _cases()[name]
    df = _frame()
    got = _both(df, expr)
    ps = pd.Series(df["s"].to_list(), dtype=object)
    s = ps.str
    pandas = {
        "capitalize": lambda: s.capitalize(),
        "title": lambda: s.title(),
        "swapcase": lambda: s.swapcase(),
        "casefold": lambda: s.casefold(),
        "isalnum": lambda: s.isalnum(),
        "isalpha": lambda: s.isalpha(),
        "isdecimal": lambda: s.isdecimal(),
        "isdigit": lambda: s.isdigit(),
        "islower": lambda: s.islower(),
        "isnumeric": lambda: s.isnumeric(),
        "isspace": lambda: s.isspace(),
        "istitle": lambda: s.istitle(),
        "isupper": lambda: s.isupper(),
        "zfill": lambda: s.zfill(9),
        "pad": lambda: s.pad(12, "right", "."),
        "pad_start": lambda: s.pad(11, "left", "*"),
        "pad_end": lambda: s.pad(11, "right", "*"),
        "ljust": lambda: s.ljust(10, "-"),
        "rjust": lambda: s.rjust(10, "-"),
        "center": lambda: s.center(13, "-"),
        "removeprefix": lambda: s.removeprefix("ab"),
        "removesuffix": lambda: s.removesuffix("b-"),
        "repeat": lambda: s.repeat(3),
        "split": lambda: s.split(","),
        "rsplit": lambda: s.rsplit(","),
        "partition": lambda: s.partition(",", expand=False),
        "rpartition": lambda: s.rpartition("ab", expand=False),
        "rfind": lambda: s.rfind("a"),
        "get": lambda: s.get(2),
        "join": lambda: s.split(",").str.join("+"),
    }[name]()
    want = [None if v is None or (isinstance(v, float) and v != v) else v for v in pandas.tolist()]
    want = [list(v) if isinstance(v, tuple) else v for v in want]
    # pandas gives False / a number for a null row in a few methods; compare non-null rows only.
    keep = [i for i, v in enumerate(df["s"].to_list()) if v is not None]
    assert [got[i] for i in keep] == [want[i] for i in keep]
    # And a null row is null in the expression.
    assert all(got[i] is None for i in range(len(got)) if i not in set(keep))


def test_the_regex_methods_equal_pandas_on_ascii():
    pd = pytest.importorskip("pandas")
    df = _frame()
    s = pd.Series(df["s"].to_list(), dtype=object).str
    keep = [i for i, v in enumerate(df["s"].to_list()) if v is not None]
    for expr, want in [
        (col("s").extract("([a-z])([0-9])", 2), s.extract("([a-z])([0-9])", expand=True)[1]),
        (col("s").findall("[0-9]+"), s.findall("[0-9]+")),
        (col("s").match("[a-z]+"), s.match("[a-z]+")),
    ]:
        got = _both(df, expr)
        w = [None if isinstance(v, float) and v != v else v for v in want.tolist()]
        assert [got[i] for i in keep] == [w[i] for i in keep]


def test_slice_replace_equals_pandas_on_ascii():
    pd = pytest.importorskip("pandas")
    df = _frame()
    s = pd.Series(df["s"].to_list(), dtype=object).str
    keep = [i for i, v in enumerate(df["s"].to_list()) if v is not None]
    for expr, want in [
        (col("s").slice_replace(2, 5, "##"), s.slice_replace(2, 5, "##")),
        (col("s").slice_replace(1, None, "~"), s.slice_replace(1, None, "~")),
    ]:
        got = _both(df, expr)
        assert [got[i] for i in keep] == [want.tolist()[i] for i in keep]


def test_non_ascii_text_equals_the_eager_method():
    df = DataFrame.from_dict({"s": ["café au lait", "日本", "abÉ", None, "x"]})
    for expr, eager in [
        (col("s").title(), lambda s: s.str.title()),
        (col("s").swapcase(), lambda s: s.str.swapcase()),
        (col("s").isalpha(), lambda s: s.str.isalpha()),
        (col("s").pad_start(12, "*"), lambda s: s.str.pad_start(12, "*")),
        (col("s").split(" "), lambda s: s.str.split(" ")),
    ]:
        assert _both(df, expr) == eager(df["s"]).to_list()


def test_the_types_are_the_eager_types():
    df = DataFrame.from_dict({"s": ["a,b", "c", None]})
    for expr, eager in [
        (col("s").isalpha(), lambda s: s.str.isalpha()),
        (col("s").rfind("a"), lambda s: s.str.rfind("a")),
        (col("s").split(","), lambda s: s.str.split(",")),
        (col("s").capitalize(), lambda s: s.str.capitalize()),
    ]:
        got = df.with_columns(out=expr)["out"]
        assert got.dtype == eager(df["s"]).dtype


def test_a_column_over_one_chunk_equals_eager():
    # More rows than one evaluator chunk (65536): the chunks join up.
    rng = random.Random(7)
    rows = ["".join(rng.choice("ab,Cd 9") for _ in range(rng.randrange(20))) for _ in range(70000)]
    rows[5] = None
    df = DataFrame.from_dict({"s": rows})
    assert df.with_columns(o=col("s").title())["o"].to_list() == df["s"].str.title().to_list()
    assert df.with_columns(o=col("s").split(","))["o"].to_list() == df["s"].str.split(",").to_list()
    assert (
        df.with_columns(o=col("s").center(21, "."))["o"].to_list()
        == df["s"].str.center(21, ".").to_list()
    )


def test_an_all_null_column_stays_null():
    s = Series.from_list([None] * 6, dtype=DType.STRING)
    df = DataFrame.from_dict({"s": s})
    for expr in (
        col("s").title(),
        col("s").isalpha(),
        col("s").split(","),
        col("s").zfill(4),
        col("s").rfind("a"),
    ):
        assert df.with_columns(o=expr)["o"].to_list() == [None] * 6


def test_a_bool_cast_to_string_then_capitalize_spells_true_and_false():
    df = DataFrame.from_dict({"b": [True, False, None, True]})
    got = df.lazy().with_columns(t=col("b").cast("string").capitalize()).collect()["t"].to_list()
    assert got == ["True", "False", None, "True"]
    assert df.with_columns(t=col("b").cast("string").capitalize())["t"].to_list() == got
    # The eager astype says the same.
    assert df["b"].astype("string").to_list() == got


def test_index_and_rindex_run_when_every_row_has_the_substring():
    df = DataFrame.from_dict({"s": ["foo", "bo", None]})
    assert df.with_columns(o=col("s").index("o"))["o"].to_list() == [1, 1, None]
    assert df.with_columns(o=col("s").rindex("o"))["o"].to_list() == [2, 1, None]


def test_index_and_rindex_fail_the_plan_run_on_a_miss():
    df = DataFrame.from_dict({"s": ["foo", "bar"]})
    with pytest.raises(Exception, match="not found in every row"):
        df.with_columns(o=col("s").index("o"))
    with pytest.raises(Exception, match="not found in every row"):
        df.lazy().with_columns(o=col("s").rindex("o")).collect()
    # The same for a column over one chunk.
    big = DataFrame.from_dict({"s": ["foo"] * 70000 + ["bar"]})
    with pytest.raises(Exception, match="not found in every row"):
        big.with_columns(o=col("s").index("o"))


def test_get_dummies_has_no_expression_form():
    with pytest.raises(NotImplementedError, match=r"Series\.str\.get_dummies"):
        col("s").get_dummies()
    # The eager form is still there.
    assert sorted(DataFrame.from_dict({"s": ["a|b", "b"]})["s"].str.get_dummies().columns) == [
        "a",
        "b",
    ]


def test_cat_without_another_column_is_the_aggregate():
    with pytest.raises(NotImplementedError, match=r"Series\.str\.cat"):
        col("s").cat()
    with pytest.raises(TypeError):
        col("s").cat("x")


def test_get_on_a_string_needs_a_non_negative_position():
    df = DataFrame.from_dict({"s": ["abc"]})
    with pytest.raises(Exception, match="negative"):
        df.with_columns(o=col("s").get(-1))
    # On a list it counts from the end.
    assert df.with_columns(o=col("s").split("b").get(-1))["o"].to_list() == ["c"]


def test_a_fill_character_is_one_ascii_character():
    with pytest.raises(ValueError):
        col("s").pad_start(5, "ab")
    with pytest.raises(ValueError):
        col("s").center(5, "")
    with pytest.raises(ValueError):
        col("s").pad(5, "middle")


def test_a_string_method_needs_a_string_column():
    df = DataFrame.from_dict({"n": [1, 2]})
    with pytest.raises(Exception, match="String"):
        df.with_columns(o=col("n").title())


def test_join_needs_a_list_of_strings():
    df = DataFrame.from_dict({"s": ["a,b"]})
    with pytest.raises(Exception, match="List"):
        df.with_columns(o=col("s").join("-"))


def test_chained_methods_compose():
    df = DataFrame.from_dict({"s": ["  hello-world ", "foo", None]})
    expr = col("s").strip().title().removeprefix("Hello").pad_start(8, "_")
    assert _both(df, expr) == ["__-World", "_____Foo", None]


def test_rsplit_is_split_as_the_eager_one_is():
    # Both scan from the left, so a separator that overlaps itself splits as
    # split() does; Python's rsplit scans from the right and would give
    # ["a-", "b"] here. For any other separator the parts are the same.
    df = DataFrame.from_dict({"s": ["a---b"]})
    assert df["s"].str.rsplit("--").to_list() == [["a", "-b"]]
    assert df.with_columns(o=col("s").rsplit("--"))["o"].to_list() == [["a", "-b"]]


_REGEX_REPLACE_CASES = [
    ("(?<op>[a-z]+)64_(\\d+)", "${op}#$2", "open64_17", "open#17"),
    ("/+", "/", "/a//b///c", "/a/b/c"),
    ("x*", "-", "ab", "-a-b-"),
    ("(a)|(b)", "[$1|$2]", "ab", "[a|]" + "[|b]"),
    ("a", "$$", "banana", "b$n$n$"),
    ("z", "-", "banana", "banana"),
]


@pytest.mark.parametrize("pat,to,src,want", _REGEX_REPLACE_CASES)
def test_regex_replace_series_and_expression_agree(pat, to, src, want):
    df = DataFrame.from_dict({"s": [src, None]})
    assert df["s"].str_regex_replace(pat, to).to_list() == [want, None]
    assert df["s"].str.regex_replace(pat, to).to_list() == [want, None]
    assert _both(df, col("s").regex_replace(pat, to)) == [want, None]
    assert _both(df, col("s").str.regex_replace(pat, to)) == [want, None]


@pytest.mark.parametrize("pat,to", [("(", "x"), ("a", "$"), ("a", "$1"), ("(a)", "${nope}")])
def test_regex_replace_bad_pattern_or_replacement_raises(pat, to):
    df = DataFrame.from_dict({"s": ["abc"]})
    with pytest.raises(ValueError):
        df["s"].str_regex_replace(pat, to)


_COL_A = ["abcabc", "abc", None, "", "xyz", "aaa", "abc", "x"]
_COL_B = ["abc", "bc", "a", "", None, "aa", "", "xy"]
_COL_C = ["Z", "Q", "-", "+", "!", None, "?", "w"]


def _col_frame():
    return DataFrame.from_dict({"a": _COL_A, "b": _COL_B, "c": _COL_C})


def _ref(fn):
    return [None if None in args else fn(*args) for args in zip(_COL_A, _COL_B, _COL_C)]


@pytest.mark.parametrize("name", ["starts_with", "ends_with", "contains"])
def test_column_needle_predicates(name):
    py = {
        "starts_with": lambda a, b, c: a.startswith(b),
        "ends_with": lambda a, b, c: a.endswith(b),
        "contains": lambda a, b, c: b in a,
    }[name]
    want = [None if x is None or y is None else py(x, y, None) for x, y in zip(_COL_A, _COL_B)]
    df = _col_frame()
    assert _both(df, getattr(col("a"), name)(col("b"))) == want
    assert _both(df, getattr(col("a").str, name)(col("b"))) == want


def test_column_needle_can_be_an_expression_and_empty_is_true():
    df = _col_frame()
    got = _both(df, col("a").contains(col("b").str.replace_all("a", "")))
    want = [
        None if x is None or y is None else y.replace("a", "") in x for x, y in zip(_COL_A, _COL_B)
    ]
    assert got == want
    assert _both(df, col("a").starts_with(col("b")))[3] is True


def test_replace_all_with_columns_and_mixed():
    df = _col_frame()
    want = [
        None if None in (a, b, c) else (a if b == "" else a.replace(b, c))
        for a, b, c in zip(_COL_A, _COL_B, _COL_C)
    ]
    assert _both(df, col("a").replace_all(col("b"), col("c"))) == want
    assert _both(df, col("a").str.replace_all(col("b"), col("c"))) == want
    mixed_to = [
        None if a is None or b is None else (a if b == "" else a.replace(b, "#"))
        for a, b in zip(_COL_A, _COL_B)
    ]
    assert _both(df, col("a").replace_all(col("b"), "#")) == mixed_to
    mixed_from = [
        None if a is None or c is None else a.replace("a", c) for a, c in zip(_COL_A, _COL_C)
    ]
    assert _both(df, col("a").replace_all("a", col("c"))) == mixed_from


def test_column_forms_serialize_to_duql():
    assert col("a").starts_with(col("b")).to_duql() == "starts_with(a, b)"
    assert col("a").str.ends_with(col("b")).to_duql() == "ends_with(a, b)"
    assert col("a").contains(col("b")).to_duql() == "contains(a, b)"
    assert col("a").replace_all(col("b"), "#").to_duql() == 'replace(a, b, "#")'
    assert col("a").replace_all("x", col("c")).to_duql() == 'replace(a, "x", c)'
    with pytest.raises(TypeError):
        col("a").starts_with(col("b").str.replace_all("a", "")).to_duql()


def test_replace_first_only_rejects_expressions():
    with pytest.raises(TypeError):
        col("a").replace(col("b"), "x")


def test_str_forms_unchanged():
    df = _col_frame()
    assert _both(df, col("a").starts_with("ab")) == [
        None if x is None else x.startswith("ab") for x in _COL_A
    ]
    assert _both(df, col("a").replace_all("a", "-")) == [
        None if x is None else x.replace("a", "-") for x in _COL_A
    ]
