# SPDX-License-Identifier: MIT
"""The pandas ``eval`` and ``query`` forms that rule strings use: ``.isin``,
``.str.split(sep).str[i]``, ``@name`` with ``local_dict`` / ``global_dict``, and
``query`` with the same syntax as ``eval``. Each case is checked against pandas."""

import math

import pandas as pd
import pytest

from dftracer.utils import DataFrame, Series
from dftracer.utils.errors import DFTUtilsTranspileError

DATA = {
    "cat": ["posix", "stdio", "app", "app.x", "posix", "mpi"],
    "func_name": ["open", "read", "write", "open64", "close", "open"],
    "io_cat": [3, 1, 3, 3, 2, 3],
    "proc_name": ["a#h1#1", "b#h2#2", "c#h3#3", "d", None, "f#h6#6"],
    "time_range": [1, 2, 3, 4, 5, 6],
}


def _ours():
    return DataFrame.from_dict(DATA)


def _theirs():
    return pd.DataFrame(DATA)


def _values(x):
    out = x.to_list() if hasattr(x, "to_list") else list(x)
    return [None if isinstance(v, float) and math.isnan(v) else v for v in out]


def _rows(frame):
    return frame.reset_index(drop=True).to_dict()


def _pandas_rows(frame):
    out = frame.reset_index(drop=True).to_dict(orient="list")
    return {
        k: [None if isinstance(v, float) and math.isnan(v) else v for v in vs]
        for k, vs in out.items()
    }


@pytest.mark.parametrize(
    "expr",
    [
        'func_name.isin(["open", "read"])',
        "func_name.isin(('open', 'read'))",
        'cat == "posix" & ~func_name.isin(["open"])',
        "~io_cat.isin([3])",
        'func_name.isin(["open"]) | io_cat.isin([1, 2])',
    ],
)
def test_isin_matches_pandas(expr):
    want = _theirs().eval(expr)
    assert _values(_ours().eval(expr)) == [bool(v) for v in want]


@pytest.mark.parametrize("i", [0, 1, 2, -1, 5])
def test_split_get_matches_pandas(i):
    expr = f'proc_name.str.split("#").str[{i}]'
    want = _theirs()["proc_name"].str.split("#").str[i]
    assert _values(_ours().eval(expr)) == _values(want)


def test_split_get_as_an_assignment():
    out = _ours().eval('host = proc_name.str.split("#").str[1]')
    assert list(out.columns) == [*DATA, "host"]
    assert out["host"].to_list() == ["h1", "h2", "h3", None, None, "h6"]


def test_split_get_in_a_predicate():
    got = _ours().query('proc_name.str.split("#").str[1] == "h2"')
    assert got["time_range"].to_list() == [2]


def test_local_dict_and_at_names():
    keys = [1, 3]
    want = _pandas_rows(_theirs().query("time_range in @keys", local_dict={"keys": keys}))
    assert _rows(_ours().query("time_range in @keys", local_dict={"keys": keys})) == want
    got = _ours().eval("time_range.isin(@keys)", local_dict={"keys": keys})
    assert _values(got) == [True, False, True, False, False, False]
    got = _ours().eval("io_cat == @n", global_dict={"n": 3})
    assert _values(got) == [True, False, True, True, False, True]


def test_at_names_read_the_caller():
    keys = [2, 4]  # noqa: F841 (read through @keys)
    limit = 3  # noqa: F841 (read through @limit)
    got = _ours().query("time_range in @keys or io_cat < @limit")
    want = _theirs().query("time_range in @keys or io_cat < @limit")
    assert _rows(got) == _pandas_rows(want)


def test_local_dict_wins_over_the_caller():
    keys = [1]
    got = _ours().query("time_range in @keys", local_dict={"keys": [6]})
    assert got["time_range"].to_list() == [6]
    assert keys == [1]


def test_an_unknown_at_name_raises():
    with pytest.raises(NameError, match="missing_name"):
        _ours().eval("time_range in @missing_name", local_dict={})


def test_at_needs_a_name():
    with pytest.raises(SyntaxError):
        _ours().eval("time_range @ io_cat")


@pytest.mark.parametrize(
    "expr",
    [
        'cat.str.contains("posix|stdio")',
        'io_cat == 3 and func_name.str.contains("open")',
        'cat == "app" or cat.str.startswith("app.")',
        'func_name.isin(["open", "close"]) & (io_cat > 1)',
        'not func_name.str.startswith("o")',
    ],
)
def test_query_takes_the_eval_syntax(expr):
    ours = _ours()
    got = _rows(ours.query(expr))
    assert got == _rows(ours[ours.eval(expr)])
    assert got == _pandas_rows(_theirs().query(expr))


def test_query_takes_duql_by_keyword():
    ours = _ours()
    assert ours.query(duql="cat ~ '^st'")["func_name"].to_list() == ["read"]
    assert ours.query(duql="io_cat == 3 and cat == 'app'")["func_name"].to_list() == ["write"]
    with pytest.raises(SyntaxError):
        ours.query("cat ~ '^st'")
    with pytest.raises(TypeError, match="not both"):
        ours.query("io_cat == 3", duql="io_cat == 3")
    with pytest.raises(TypeError, match="local_dict"):
        ours.query(duql="io_cat == 3", local_dict={})


def test_query_needs_one_bool_expression():
    ours = _ours()
    with pytest.raises(TypeError, match="Bool"):
        ours.query("io_cat + 1")
    with pytest.raises(TypeError, match="assignment"):
        ours.query("x = io_cat > 1")


def test_query_runs_the_plan_on_the_rows_kept():
    got = _ours().query("io_cat == 3", select=["func_name"], order_by="func_name", limit=2)
    assert got.to_dict() == {"func_name": ["open", "open"]}


def test_query_drops_null_rows_as_the_eval_mask_does():
    ours = DataFrame.from_dict({"a": [1.0, None, 3.0], "s": ["x", None, "y"]})
    for expr in ["a != 1", "not a > 1", 's != "x"', "a == 1 or a > 2", "~(a < 2)"]:
        assert _rows(ours.query(expr)) == _rows(ours[ours.eval(expr)]), expr
    assert ours.query("a != 1")["a"].to_list() == [3.0]
    assert ours.query("s not in ['x']")["s"].to_list() == [None, "y"]


def test_two_column_compares_match_pandas():
    data = {
        "u1": pd.Series([1, 5, 7, 0, 9], dtype="uint64"),
        "u2": pd.Series([2, 5, 3, 0, 9], dtype="uint64"),
        "f": [0.5, 1.5, 2.5, float("inf"), 1.0],
        "g": [0.5, 1.0, 2.5, float("inf"), 2.0],
        "s": ["x", "y", "z", "app", "x"],
        "t": ["x", "z", "z", "ap", "y"],
    }
    theirs = pd.DataFrame(data)
    ours = DataFrame.from_pandas(theirs)
    for expr in ["u1 < u2", "u1 > u2", "u1 <= u2", "f == g", "f >= g", "s == t", "s != t"]:
        assert _rows(ours.query(expr)) == _pandas_rows(theirs.query(expr)), expr


def test_bool_constants_match_pandas():
    data = {"flag": [True, False, True], "n": [1, 2, 3]}
    ours, theirs = DataFrame.from_dict(data), pd.DataFrame(data)
    for expr in ["flag == True", "flag == False", "flag != True", "True == flag", "flag"]:
        assert _rows(ours.query(expr)) == _pandas_rows(theirs.query(expr)), expr


def test_true_and_false_against_a_number_are_one_and_zero():
    data = {"n": [0, 1, 2]}
    ours, theirs = DataFrame.from_dict(data), pd.DataFrame(data)
    for expr in ["n == True", "n != True", "n == False", "True == n", "n > False"]:
        assert _values(ours.eval(expr)) == [bool(v) for v in theirs.eval(expr)], expr


def test_a_bool_variable_tests_a_bool_column():
    ours = DataFrame.from_dict({"flag": [True, False, True]})
    assert _values(ours.eval("flag == @f", local_dict={"f": True})) == [True, False, True]
    assert _values(ours.eval("flag != @f", local_dict={"f": True})) == [False, True, False]


def test_bool_literals_stay_out_of_values():
    ours = DataFrame.from_dict({"n": [1, 2, 3]})
    with pytest.raises(DFTUtilsTranspileError, match="True"):
        ours.eval("True if n > 1 else False")


def test_apply_keeps_bool_results():
    def big(x):
        if x > 1:
            return True
        return False

    got = Series.from_list([0, 1, 2]).apply(big)
    assert got.to_list() == [False, False, True]
    assert got.dtype.name == "BOOL"


def test_at_names_cannot_hide_a_column():
    ours = DataFrame.from_dict({"n": [0, 1, 2], "__at_k": [0, 0, 0]})
    got = ours.eval("n == __at_k or n > @k", local_dict={"k": 2})
    want = pd.DataFrame({"n": [0, 1, 2], "__at_k": [0, 0, 0]}).eval(
        "n == __at_k or n > @k", local_dict={"k": 2}
    )
    assert _values(got) == [bool(v) for v in want] == [True, False, False]


def test_an_at_in_a_string_is_text():
    ours = DataFrame.from_dict({"s": ["a@b", "x"]})
    assert ours.query("s == 'a@b'")["s"].to_list() == ["a@b"]
    assert _values(ours.eval("s.str.split('@').str[1]")) == ["b", None]
    assert _values(ours.eval("s.str.contains('@')  # find @mail")) == [True, False]


def test_a_longer_separator_is_refused():
    ours = DataFrame.from_dict({"s": ["a..b"]})
    with pytest.raises(DFTUtilsTranspileError, match="regex"):
        ours.eval('s.str.split("..").str[1]')


def test_at_names_take_numpy_and_series_values():
    np = pytest.importorskip("numpy")
    ours = DataFrame.from_dict({"n": [1, 2, 3]})
    want = [True, False, True]
    assert _values(ours.eval("n.isin(@k)", local_dict={"k": np.array([1, 3])})) == want
    assert _values(ours.eval("n in @k", local_dict={"k": ours["n"][ours["n"] != 2]})) == want
    assert _values(ours.eval("n < @k", local_dict={"k": np.int64(2)})) == [True, False, False]
    with pytest.raises(DFTUtilsTranspileError, match="@k"):
        ours.eval("n + @k", local_dict={"k": [1]})


def test_an_at_name_is_not_an_assignment_target():
    with pytest.raises(DFTUtilsTranspileError, match="@x"):
        _ours().eval("@x = io_cat + 1", local_dict={"x": 5})


def test_min_and_max_compare_without_subtraction():
    theirs = pd.DataFrame(
        {
            "u1": pd.Series([0, 1, 2**63, 2**64 - 1], dtype="uint64"),
            "u2": pd.Series([1, 0, 2**64 - 1, 2**63], dtype="uint64"),
        }
    )
    ours = DataFrame.from_pandas(theirs)
    assert ours.eval("min(u1, u2)").to_list() == [0, 0, 2**63, 2**63]
    assert ours.eval("max(u1, u2)").to_list() == [1, 1, 2**64 - 1, 2**64 - 1]


def test_query_plan_on_the_pandas_path_has_no_row_labels():
    ours = DataFrame.from_dict(
        {"dur": [50, 150, 200, 80, 300], "cat": ["io", "cpu", "io", "io", "net"]}
    )
    kw = {"group_by": "cat", "aggs": ["count", "sum:dur"], "order_by": "count", "descending": True}
    got = ours.query("dur > 60", **kw)
    want = ours.query(duql="dur > 60", **kw)
    assert got.to_dict() == want.to_dict()
    assert list(got.columns) == list(want.columns)
    assert ours.query("dur > 60", order_by="dur").column_names == ["dur", "cat"]


def test_dicts_without_expr_raise():
    with pytest.raises(TypeError, match="local_dict"):
        _ours().query(local_dict={"x": 1})


def test_no_at_leaves_the_caller_alone():
    import gc
    import weakref

    class Big:
        pass

    obj = Big()
    ref = weakref.ref(obj)
    _ours().eval("io_cat > 1")
    del obj
    gc.collect()
    assert ref() is None


def test_bool_expressions_compare_with_true_and_false():
    data = {"n": [0, 1, 2], "flag": [True, False, True], "s": ["a", "b", "ab"]}
    ours, theirs = DataFrame.from_dict(data), pd.DataFrame(data)
    for expr in [
        "(n > 0) == True",
        "(n > 0) == False",
        "~flag == True",
        "s.str.contains('a') == False",
        "n.isin([1]) == False",
        "n.isna() == False",
        "flag == @b",
    ]:
        want = theirs.eval(expr, local_dict={"b": True})
        assert _values(ours.eval(expr, local_dict={"b": True})) == [bool(v) for v in want], expr
    with pytest.raises(DFTUtilsTranspileError, match="two constants"):
        ours.eval("1 == True")


def test_apply_falls_back_on_bool_and_list_forms():
    flags = Series.from_list([True, False])
    assert flags.apply(lambda x: 1 if x == True else 0).to_list() == [1, 0]  # noqa: E712
    words = Series.from_list(["a#b", "c"])
    assert words.apply(lambda x: len(x.split("#"))).to_list() == [2, 1]
    assert words.apply(lambda x: "c" in x.split("#")).to_list() == [False, True]


def test_temporal_columns_still_compare():
    theirs = pd.DataFrame(
        {
            "a": pd.to_datetime(["2024-01-01", "2024-01-03"]),
            "b": pd.to_datetime(["2024-01-02", "2024-01-02"]),
        }
    )
    theirs["x"] = theirs["b"] - theirs["a"]
    theirs["y"] = theirs["a"] - theirs["b"]
    ours = DataFrame.from_pandas(theirs)
    for expr in ["a < b", "a >= b", "x == y", "x > y"]:
        assert _values(ours.eval(expr)) == [bool(v) for v in theirs.eval(expr)], expr


def test_split_separators_follow_pandas():
    data = {"s": ["e##f", "aaa", "p.q"]}
    ours, theirs = DataFrame.from_dict(data), pd.DataFrame(data)
    for expr in [
        's.str.split("##").str[1]',
        's.str.rsplit("#f").str[0]',
        's.str.rsplit(".").str[0]',
    ]:
        assert _values(ours.eval(expr)) == _values(theirs.eval(expr)), expr
    for expr in [
        's.str.split("..").str[1]',
        's.str.rsplit("aa").str[0]',
        's.str.rsplit("##").str[1]',
    ]:
        with pytest.raises(DFTUtilsTranspileError):
            ours.eval(expr)
    with pytest.raises(DFTUtilsTranspileError):
        ours.eval('s.str.split("#").__str_part__()')


def test_a_folded_name_cannot_hide_a_column():
    ours = DataFrame.from_dict({"n": [0, 1, 2], "__at_k": [0, 0, 0]})
    got = ours.query("n == __\uff41\uff54_k or n > @k", local_dict={"k": 2})
    assert got["n"].to_list() == [0]


def test_a_generator_variable_is_read_once():
    got = _ours().eval("io_cat.isin(@g) | io_cat.isin(@g)", local_dict={"g": (x for x in [1, 2])})
    assert _values(got) == [False, True, False, False, True, False]


def test_numpy_scalars_in_a_list():
    np = pytest.importorskip("numpy")
    got = _ours().eval("io_cat.isin(@k)", local_dict={"k": [np.int64(1), np.int64(2)]})
    assert _values(got) == [False, True, False, False, True, False]


def test_an_at_in_a_string_leaves_the_caller_alone():
    import gc
    import weakref

    class Big:
        pass

    obj = Big()
    ref = weakref.ref(obj)
    _ours().eval("cat == 'a@b'")
    del obj
    gc.collect()
    assert ref() is None


def test_errors_name_the_at_variable():
    with pytest.raises(DFTUtilsTranspileError, match="@fn"):
        _ours().eval("io_cat > @fn(1)", local_dict={"fn": 5})


def test_apply_keeps_numeric_truth_tests():
    def neg(x):
        if (not x) == True:  # noqa: E712
            return 1
        return 0

    assert Series.from_list([0, 1, -1]).apply(neg).to_list() == [1, 0, 0]


def test_float16_columns_still_compare():
    np = pytest.importorskip("numpy")
    theirs = pd.DataFrame(
        {"x": np.array([1, 2], dtype=np.float16), "y": np.array([1, 4], dtype=np.float16)}
    )
    ours = DataFrame.from_pandas(theirs)
    assert _values(ours.eval("x == y")) == [True, False]
    assert ours.eval("min(x, y)").to_list() == [1.0, 2.0]


def test_derived_values_compare_directly():
    theirs = pd.DataFrame(
        {
            "s": ["a-b", "C-d"],
            "t": ["a", "c"],
            "u1": pd.Series([0, 2**64 - 1], dtype="uint64"),
            "u2": pd.Series([1, 2**63], dtype="uint64"),
        }
    )
    ours = DataFrame.from_pandas(theirs)
    assert _values(ours.eval("s.str.split('-').str[0] == t")) == [True, False]
    assert _values(ours.eval("s.str.lower().str.split('-').str[0] == t")) == [True, True]
    assert _values(ours.eval("abs(u1) < u2")) == [True, False]
    assert _values(ours.eval("u1.fillna(0) < u2")) == [True, False]


def test_a_dict_closure_stays_in_the_engine():
    import warnings

    keys = {1: "a", 3: "b"}
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        got = Series.from_list([0, 1, 2, 3]).apply(lambda x: x in keys)
    assert got.to_list() == [False, True, False, True]


def test_an_at_function_is_not_inlined():
    def helper(x):
        return x * 10

    with pytest.raises(DFTUtilsTranspileError, match="@fn"):
        _ours().eval("@fn(io_cat)", local_dict={"fn": helper})


def test_a_folded_at_name_is_its_variable():
    ours = DataFrame.from_dict({"n": [0, 1, 2]})
    assert _values(ours.eval("n > @ｋ", local_dict={"k": 0})) == [False, True, True]
    with pytest.raises(DFTUtilsTranspileError, match="@k"):
        ours.eval("@ｋ = n + 1", local_dict={"k": 0})


def test_contains_flags_read_bool_variables():
    ours = DataFrame.from_dict({"s": ["A.C", "abc"]})
    got = ours.eval(
        "s.str.contains(@pat, regex=@r, case=@c)", local_dict={"pat": "a.c", "r": False, "c": False}
    )
    assert _values(got) == [True, False]


def test_query_names_an_assignment_first():
    with pytest.raises(TypeError, match="assignment"):
        _ours().query("cat = 'io'")
