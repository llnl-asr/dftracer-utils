"""``DataFrame.eval``: Python expressions over the columns, lowered by the same
code as ``apply``'s source tier (openspec change dataframe-eval)."""

import math

import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame, col, lit
from dftracer.utils._transpile import TranspileError


def _frame():
    return DataFrame.from_dict({"a": [1.0, 2.0, 3.0], "b": [10.0, 20.0, 30.0], "c": [7, 8, 9]})


def _l(x):
    return x.to_list() if hasattr(x, "to_list") else x


def _close(got, want):
    assert len(got) == len(want), (got, want)
    for g, w in zip(got, want):
        if w is None:
            assert g is None, (got, want)
        else:
            assert g == pytest.approx(w), (got, want)


def test_proposal_repro_ratio_assignment():
    d = _frame()
    out = d.eval("m = a / b")
    assert list(out.columns) == ["a", "b", "c", "m"]
    _close(out["m"].to_list(), [0.1, 0.1, 0.1])
    assert list(d.columns) == ["a", "b", "c"]  # the original is unchanged
    _close(_l(d.eval("a / b")), [0.1, 0.1, 0.1])


def test_precedence_and_arithmetic():
    d = _frame()
    _close(_l(d.eval("a + b * 2 - 1")), [20.0, 41.0, 62.0])
    _close(_l(d.eval("(a + b) * 2")), [22.0, 44.0, 66.0])
    _close(_l(d.eval("a ** 2")), [1.0, 4.0, 9.0])
    _close(_l(d.eval("a ** 0.5")), [1.0, math.sqrt(2), math.sqrt(3)])


def test_integer_division_and_remainder_stay_integers():
    d = _frame()
    assert _l(d.eval("c // 2")) == [3, 4, 4]
    assert _l(d.eval("c % 4")) == [3, 0, 1]
    assert d.eval("c // 2").dtype == d["c"].dtype


def test_unary_operators():
    d = _frame()
    _close(_l(d.eval("-a")), [-1.0, -2.0, -3.0])
    _close(_l(d.eval("+a")), [1.0, 2.0, 3.0])
    assert _l(d.eval("not (a > 1)")) == [True, False, False]


def test_a_power_with_a_column_exponent_is_an_error():
    with pytest.raises(TranspileError, match="constant exponent"):
        _frame().eval("a ** b")


def test_comparisons_and_connectives():
    d = _frame()
    assert _l(d.eval("a > 1 and b < 30")) == [False, True, False]
    assert _l(d.eval("1 < a <= 2")) == [False, True, False]
    assert _l(d.eval("a < 2 or b > 20")) == [True, False, True]
    assert _l(d.eval("c == 8")) == [False, True, False]
    assert _l(d.eval("c != 8")) == [True, False, True]


def test_mask_operators_combine_and_negate_comparisons():
    d = _frame()
    assert _l(d.eval("(a > 1) & (b < 30)")) == [False, True, False]
    assert _l(d.eval("(a > 1) | (b > 25)")) == [False, True, True]
    assert _l(d.eval("~(a > 1)")) == [True, False, False]
    assert _l(d.eval("~((a > 1) & (b < 30))")) == [True, False, True]
    # & and | combine masks; two float columns are not masks
    with pytest.raises(RuntimeError):
        d.eval("a & b")


def test_and_or_bind_like_pandas_eval_not_like_python():
    # In pandas eval `&` and `|` mean `and` and `or`, so a comparison needs no parentheses;
    # in plain Python `a > 1 & b < 30` is `a > (1 & b) < 30`.
    d = _frame()
    assert _l(d.eval("a > 1 & b < 30")) == [False, True, False]
    assert _l(d.eval("a > 1 | b > 25")) == [False, True, True]
    assert _l(d.eval("a < 2 | a > 2 & b > 25")) == [True, False, True]  # & binds tighter than |
    assert _l(d.eval("c == 7 | c == 9")) == [True, False, True]
    assert _l(d.eval("~(a > 1) & b < 30")) == [True, False, False]
    # the rewrite is for the operators: a `&` inside a string stays
    s = DataFrame.from_dict({"s": ["a&b", "ab"]})
    assert _l(s.eval("s.str.contains('a&b')")) == [True, False]


# The analyzer's DERIVED_POSIX_METRICS (dftracer/analyzer/config.py), copied as data: pandas-style
# strings with the `.str` accessor, `~` on a mask, and `and` / `or`.
_DERIVED_POSIX_METRICS = {
    "data": "io_cat == 1 or io_cat == 2",
    "read": "io_cat == 1",
    "write": "io_cat == 2",
    "metadata": "io_cat == 3",
    "close": 'io_cat == 3 and func_name.str.contains("close") and ~func_name.str.contains("dir")',
    "open": 'io_cat == 3 and func_name.str.contains("open") and ~func_name.str.contains("dir")',
    "seek": 'io_cat == 3 and func_name.str.contains("seek")',
    "stat": 'io_cat == 3 and func_name.str.contains("stat")',
    "other": "io_cat == 6",
    "sync": "io_cat == 7",
}


def _posix_like_frame():
    rng = np.random.default_rng(1)
    n = 300
    names = [
        "read",
        "write",
        "open",
        "close",
        "opendir",
        "closedir",
        "lseek",
        "fstat",
        "fsync",
        "pread",
    ]
    return pd.DataFrame(
        {
            "io_cat": rng.choice([1, 2, 3, 6, 7], n),
            "func_name": rng.choice(names, n),
            "x": rng.normal(size=n),
            "y": rng.normal(size=n),
        }
    )


@pytest.mark.parametrize("metric", sorted(_DERIVED_POSIX_METRICS))
def test_analyzer_derived_metrics_strings_equal_pandas_eval(metric):
    p = _posix_like_frame()
    text = _DERIVED_POSIX_METRICS[metric]
    want = p.eval(text, engine="python")
    got = DataFrame.from_pandas(p).eval(text).to_pandas()
    assert [bool(v) for v in got.tolist()] == [bool(v) for v in want.tolist()], text


@pytest.mark.parametrize(
    "text",
    [
        "x > 0 & y < 0",
        "x > 0 | y < 0",
        "(x > 0) & (y < 0)",
        "~(x > 0) & (y < 0)",
        "x > 0 & y < 0 | io_cat == 1",
        'func_name.str.contains("open|close")',
        'func_name.str.contains("posix|stdio")',
        'io_cat == 3 & func_name.str.contains("stat")',
    ],
)
def test_pandas_precedence_and_str_accessor_equal_pandas_eval(text):
    p = _posix_like_frame()
    want = p.eval(text, engine="python")
    got = DataFrame.from_pandas(p).eval(text).to_pandas()
    assert [bool(v) for v in got.tolist()] == [bool(v) for v in want.tolist()], text


def test_the_analyzer_denominator_mask_equals_pandas_with_nulls_and_zeros():
    # analyzer.py builds `(<d>.isna() | <d> == 0)` for each denominator and joins them with `&`
    rng = np.random.default_rng(2)
    n = 60
    cols = {}
    for name in ("x", "y"):
        base = np.where(rng.random(n) < 0.3, 0.0, rng.normal(size=n))
        cols[name] = pd.array(np.where(rng.random(n) < 0.3, None, base), dtype="Float64")
    p = pd.DataFrame(cols)
    d = DataFrame.from_pandas(p)
    for text in (
        "(x.isna() | x == 0)",
        "(x.isna() | x == 0) & (y.isna() | y == 0)",
        "x.isna()",
        "x.notna() & y.notna()",
        "x.isnull() | y.notnull()",
        "x == 0",
    ):
        want = [None if v is pd.NA else bool(v) for v in p.eval(text, engine="python").tolist()]
        got = [
            None if v is pd.NA else bool(v) for v in d.eval(text).to_pandas(nullable=True).tolist()
        ]
        assert got == want, text


def test_str_contains_is_a_regex_search_as_in_pandas():
    s = DataFrame.from_dict({"s": ["posix", "stdio", "mpiio", None]})
    assert _l(s.eval('s.str.contains("posix|stdio")')) == [True, True, False, None]
    assert _l(s.eval('~s.str.contains("io")')) == [True, False, False, None]
    assert _l(s.eval('s.str.contains("p.s")')) == [True, False, False, None]


def test_membership_and_conditional():
    d = _frame()
    assert _l(d.eval("c in [7, 9]")) == [True, False, True]
    assert _l(d.eval("c not in [7, 9]")) == [False, True, False]
    _close(_l(d.eval("a if a > 1 else b")), [10.0, 2.0, 3.0])


def test_nulls_propagate_and_can_be_tested():
    d = DataFrame.from_dict({"a": [1.0, None, 3.0]})
    assert _l(d.eval("a * 2")) == [2.0, None, 6.0]
    assert _l(d.eval("a > 1")) == [False, None, True]
    assert _l(d.eval("a is None")) == [False, True, False]
    assert _l(d.eval("a is not None")) == [True, False, True]


def test_functions_of_columns():
    d = _frame()
    _close(_l(d.eval("abs(a - 2)")), [1.0, 0.0, 1.0])
    _close(_l(d.eval("math.sqrt(a * 4)")), [2.0, math.sqrt(8), math.sqrt(12)])
    _close(_l(d.eval("math.log(a)")), [0.0, math.log(2), math.log(3)])
    _close(_l(d.eval("math.exp(a)")), [math.e, math.e**2, math.e**3])
    _close(_l(d.eval("math.floor(a / 2)")), [0.0, 1.0, 1.0])
    _close(_l(d.eval("math.ceil(a / 2)")), [1.0, 1.0, 2.0])
    _close(_l(d.eval("min(a, 2.5)")), [1.0, 2.0, 2.5])
    _close(_l(d.eval("max(a, 2.5)")), [2.5, 2.5, 3.0])
    _close(_l(d.eval("round(a / 2)")), [0.0, 1.0, 2.0])
    assert _l(d.eval("int(a * 1.5)")) == [1, 3, 4]
    _close(_l(d.eval("float(c)")), [7.0, 8.0, 9.0])


def test_string_methods():
    s = DataFrame.from_dict({"s": ["x", "Yy", None]})
    assert _l(s.eval("s.upper()")) == ["X", "YY", None]
    assert _l(s.eval("s.lower()")) == ["x", "yy", None]
    assert _l(s.eval("s == 'x'")) == [True, False, None]
    assert _l(s.eval("len(s)")) == [1, 2, None]
    assert _l(s.eval("s.startswith('Y')")) == [False, True, None]
    assert _l(s.eval("s.replace('y', 'z')")) == ["x", "Yz", None]


def test_a_call_that_is_not_supported_is_an_error():
    d = _frame()
    # `a.abs()` is a supported method now (change eval-method-calls), so it is no longer in this list
    for bad in ("open(a)", "sqrt(a)", "isna(a)", "__import__('os')", "print(a)"):
        with pytest.raises(TranspileError):
            d.eval(bad)


def test_single_assignment_forms():
    d = _frame()
    out = d.eval("a = a * 10")
    assert list(out.columns) == ["a", "b", "c"] and out["a"].to_list() == [10.0, 20.0, 30.0]
    out = d.eval("k = 5")
    assert out["k"].to_list() == [5, 5, 5]
    assert d["a"].to_list() == [1.0, 2.0, 3.0]


def test_several_lines_see_the_lines_before_them():
    d = _frame()
    _close(_l(d.eval("m = a / b\nn = m * 100\nn + c")), [17.0, 18.0, 19.0])
    out = d.eval("m = a / b\nn = m * 100")
    assert list(out.columns) == ["a", "b", "c", "m", "n"]
    _close(out["n"].to_list(), [10.0, 10.0, 10.0])
    # a column made on an earlier line keeps its integer type for // and %
    assert _l(d.eval("k = c * 2\nk // 3")) == [4, 5, 6]
    assert _l(d.eval("k = c * 2; k % 5")) == [4, 1, 3]
    assert list(d.columns) == ["a", "b", "c"]


def test_an_expression_before_the_last_line_is_an_error():
    with pytest.raises(TranspileError, match="only the last line"):
        _frame().eval("a + 1\nm = a")


def test_unsupported_text_names_the_cause():
    d = _frame()
    with pytest.raises(KeyError, match="nope"):
        d.eval("a + nope")
    with pytest.raises(TranspileError, match="@name"):
        d.eval("a + @k")
    with pytest.raises(TranspileError, match="Attribute|attribute"):
        d.eval("a.dtype")
    with pytest.raises(TranspileError, match="indexed"):
        d.eval("a[0]")
    with pytest.raises(TranspileError, match="constants"):
        d.eval("2 * 3")
    with pytest.raises(TranspileError):
        d.eval("a == None")
    with pytest.raises(TranspileError, match="eval needs"):
        d.eval("  ")
    with pytest.raises(SyntaxError):
        d.eval("a +")


def test_safe_no_code_or_names_outside_the_columns_are_reachable(tmp_path):
    d = _frame()
    marker = tmp_path / "pwned"
    for evil in (
        f"__import__('os').system('touch {marker}')",
        f"open('{marker}', 'w')",
        "a.__class__",
        "(lambda: 1)()",
        "().__class__.__mro__",
    ):
        with pytest.raises((TranspileError, KeyError)):
            d.eval(evil)
    assert not marker.exists()
    for name in ("os", "len", "__builtins__"):
        with pytest.raises(KeyError):
            d.eval(name)


def test_a_number_on_the_left_of_minus_workaround_works_today():
    d = _frame()
    _close(_l(d.eval("-a + 2")), [1.0, 0.0, -1.0])


def test_a_number_on_the_left_of_minus():
    d = _frame()
    _close(_l(d.eval("2 - a")), [1.0, 0.0, -1.0])
    assert _l(d.eval("100 - c")) == [93, 92, 91]
    _close(_l((lit(100) - col("a")).apply(d)), [99.0, 98.0, 97.0])


def test_a_number_on_the_left_of_division_floor_division_and_remainder():
    d = _frame()
    _close(_l(d.eval("100 / b")), [10.0, 5.0, 100 / 30])
    assert _l(d.eval("100 // c")) == [14, 12, 11]
    assert _l(d.eval("100 % c")) == [2, 4, 1]
    _close(_l((lit(1) / col("b")).apply(d)), [0.1, 0.05, 1 / 30])


def test_scalar_first_with_nulls_and_zero_follows_the_column_kernels():
    d = DataFrame.from_dict({"x": [2.0, None, 0.0]})
    got = _l(d.eval("1 / x"))
    assert got[0] == 0.5 and got[1] is None and math.isinf(got[2])
    assert _l(d.eval("5 - x")) == [3.0, None, 5.0]


def test_pandas_parity_on_random_frames_with_nulls():
    if tuple(int(x) for x in pd.__version__.split(".")[:2]) < (2, 1):
        pytest.skip("pandas eval reads nullable Float64 columns from 2.1")
    rng = np.random.default_rng(11)
    n = 200
    p = pd.DataFrame(
        {
            "a": pd.array(np.where(rng.random(n) < 0.1, None, rng.normal(size=n)), dtype="Float64"),
            "b": pd.array(rng.normal(5, 1, n), dtype="Float64"),
            "c": pd.array(
                np.where(rng.random(n) < 0.1, None, rng.integers(1, 9, n)), dtype="Int64"
            ),
        }
    )
    d = DataFrame.from_pandas(p)
    # (our text, pandas text): the same formula in each engine's spelling
    for ours, theirs in (
        ("a + b", "a + b"),
        ("a * b - c", "a * b - c"),
        ("a / b", "a / b"),
        ("c ** 2", "c ** 2"),
        ("-a + b", "-a + b"),
        ("+a * 2", "a * 2"),
        ("(a + b) * (c + 1)", "(a + b) * (c + 1)"),
        ("a > 0", "a > 0"),
        ("b <= 5 and c > 3", "(b <= 5) & (c > 3)"),
        ("a > 0 or c < 3", "(a > 0) | (c < 3)"),
        ("(a > 0) & (c < 6)", "(a > 0) & (c < 6)"),
        ("(a > 0) | (c < 3)", "(a > 0) | (c < 3)"),
        ("~(a > 0)", "~(a > 0)"),
        ("c in [1, 2, 3]", "c in [1, 2, 3]"),
        ("c not in [1, 2, 3]", "c not in [1, 2, 3]"),
        ("abs(a) + math.sqrt(b)", "abs(a) + sqrt(b)"),
        ("1 < b < 6", "1 < b < 6"),
    ):
        want = p.eval(theirs)
        got = d.eval(ours).to_pandas(nullable=True)
        assert len(got) == len(want), ours
        for g, w in zip(got.tolist(), want.tolist()):
            if w is pd.NA or (isinstance(w, float) and math.isnan(w)):
                assert g is pd.NA or (isinstance(g, float) and math.isnan(g)), (ours, g, w)
            elif isinstance(w, (bool, np.bool_)):
                assert bool(g) == bool(w), (ours, g, w)
            else:
                assert float(g) == pytest.approx(float(w), rel=1e-9), (ours, g, w)
