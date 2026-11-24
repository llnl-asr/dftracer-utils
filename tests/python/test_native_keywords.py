# SPDX-License-Identifier: MIT
"""Native Series methods that replaced Python wrappers keep the keyword names
of the wrappers' signatures.

The list is frozen from the signatures before the wrappers were deleted; it is
not computed from the running class.
"""

import pytest

from dftracer.utils import DataFrame
from dftracer.utils import dftracer_utils_ext as _ext
from dftracer.utils._series import Series

# (method, keyword names of the old Python signature, in order)
OLD_KEYWORDS = [
    ("add", ("other",)),
    ("add_scalar", ("v",)),
    ("bottom_k", ("k",)),
    ("child", ("i",)),
    ("clip", ("lower", "upper")),
    ("div_scalar", ("v",)),
    ("dot", ("other",)),
    ("dt_format", ("fmt", "unit")),
    ("dt_part", ("code", "unit")),
    ("dt_round", ("every", "mode")),
    ("ewm_mean", ("alpha",)),
    ("ewm_std", ("alpha",)),
    ("fill_null", ("value",)),
    ("filter", ("predicate",)),
    ("floordiv", ("other",)),
    ("is_between", ("lower", "upper")),
    ("list_get", ("index",)),
    ("list_join", ("sep",)),
    ("logical", ("op", "other")),
    ("mod", ("other",)),
    ("mul", ("other",)),
    ("mul_scalar", ("v",)),
    ("pow", ("other",)),
    ("prim", ("op",)),
    ("qcut", ("q",)),
    ("quantile", ("q",)),
    ("rolling", ("window", "op")),
    ("rolling_max", ("window",)),
    ("rolling_mean", ("window",)),
    ("rolling_median", ("window",)),
    ("rolling_min", ("window",)),
    ("rolling_quantile", ("window", "q")),
    ("rolling_std", ("window",)),
    ("rolling_sum", ("window",)),
    ("rolling_var", ("window",)),
    ("search_sorted", ("element",)),
    ("slice", ("offset", "length")),
    ("str_case", ("code",)),
    ("str_cat", ("other",)),
    ("str_contains", ("needle",)),
    ("str_count", ("pat",)),
    ("str_ends_with", ("suffix",)),
    ("str_eq", ("rhs",)),
    ("str_extract", ("pattern", "group")),
    ("str_find", ("needle",)),
    ("str_findall", ("pattern",)),
    ("str_is", ("code",)),
    ("str_like", ("pattern",)),
    ("str_matches", ("pattern",)),
    ("str_normalize", ("form",)),
    ("str_regex_replace", ("pattern", "to")),
    ("str_remove_prefix", ("prefix",)),
    ("str_remove_suffix", ("suffix",)),
    ("str_repeat", ("n",)),
    ("str_replace", ("pat", "repl")),
    ("str_replace_all", ("pat", "repl")),
    ("str_rfind", ("needle",)),
    ("str_search", ("pattern",)),
    ("str_slice", ("start", "length")),
    ("str_split", ("sep",)),
    ("str_starts_with", ("prefix",)),
    ("str_translate", ("from_", "to")),
    ("str_wrap", ("width", "flags")),
    ("str_zfill", ("width",)),
    ("sub", ("other",)),
    ("sub_scalar", ("v",)),
    ("top_k", ("k",)),
    ("with_timezone", ("tz",)),
]


@pytest.mark.parametrize("method,keywords", OLD_KEYWORDS, ids=[m for m, _ in OLD_KEYWORDS])
def test_native_method_accepts_the_old_keyword_names(method, keywords):
    assert hasattr(_ext.Series, method), method
    s = Series(["a", "b"])
    for count in range(1, len(keywords) + 1):
        try:
            getattr(s, method)(**{k: 1 for k in keywords[:count]})
        except TypeError as exc:
            assert "keyword" not in str(exc), (method, keywords[:count], str(exc))
        except Exception:
            pass


def test_keyword_calls_give_the_positional_results():
    s = Series([1.0, None, 3.0, 4.0], name="x")
    assert s.fill_null(value=0).to_list() == s.fill_null(0).to_list() == [1.0, 0.0, 3.0, 4.0]
    assert s.quantile(q=0.5) == s.quantile(0.5)
    assert s.is_between(lower=1, upper=3).to_list() == s.is_between(1, 3).to_list()
    assert s.clip(lower=2, upper=3).to_list() == s.clip(2, 3).to_list()
    mask = Series([True, False, True, False])
    assert s.filter(predicate=mask).to_list() == s.filter(mask).to_list() == [1.0, 3.0]
    assert s.slice(offset=1, length=2).to_list() == s.slice(1, 2).to_list()
    assert s.top_k(k=2).to_list() == s.top_k(2).to_list()
    assert s.rolling_quantile(window=2, q=0.5).to_list() == s.rolling_quantile(2, 0.5).to_list()
    words = Series(["ab", "cd"])
    assert words.str_replace(pat="a", repl="z").to_list() == ["zb", "cd"]
    assert words.str_contains(needle="c").to_list() == [False, True]
    assert words.str_slice(start=1).to_list() == ["b", "d"]
    assert s.floordiv(other=2).to_list() == s.floordiv(2).to_list()


def test_a_wrong_keyword_is_still_an_error():
    s = Series([1, 2])
    with pytest.raises(TypeError, match="keyword"):
        s.is_between(lower=1, upper=2, nope=3)
    with pytest.raises(TypeError):
        s.is_between(lower=1)


# (accessor, method, the old keyword names with valid values, in order). Frozen
# from the signatures of the Python accessor methods the rows replaced.
ACCESSOR_KEYWORDS = [
    ("str", "count", {"pat": "a"}),
    ("str", "count_matches", {"pat": "a"}),
    ("str", "endswith", {"pat": "c"}),
    ("str", "ends_with", {"suffix": "c"}),
    ("str", "find", {"sub": "b"}),
    ("str", "fullmatch", {"pat": "abc"}),
    ("str", "join", {"sep": "-"}),
    ("str", "removeprefix", {"prefix": "a"}),
    ("str", "removesuffix", {"suffix": "c"}),
    ("str", "rfind", {"sub": "b"}),
    ("str", "startswith", {"pat": "a"}),
    ("str", "strip_prefix", {"prefix": "a"}),
    ("str", "strip_suffix", {"suffix": "c"}),
    ("str", "starts_with", {"prefix": "a"}),
    ("str", "zfill", {"width": 5}),
    ("list", "get", {"index": 1}),
]


@pytest.mark.parametrize(
    "accessor,method,kwargs",
    ACCESSOR_KEYWORDS,
    ids=[f"{a}.{m}" for a, m, _ in ACCESSOR_KEYWORDS],
)
def test_accessor_method_accepts_the_old_keyword_names(accessor, method, kwargs):
    words = Series(["abc", "ba", None, "cab"], name="x")
    column = words.str.split("b") if accessor == "list" or method == "join" else words
    acc = getattr(column, accessor)
    by_name = getattr(acc, method)(**kwargs)
    by_position = getattr(acc, method)(*kwargs.values())
    assert by_name.to_list() == by_position.to_list()
    assert by_name.name == "x"
    with pytest.raises(TypeError, match="missing required argument"):
        getattr(acc, method)(**dict(list(kwargs.items())[1:]))


def test_an_unknown_keyword_is_named_like_python_does():
    words = Series(["abc"])
    with pytest.raises(
        TypeError, match=r"startswith\(\) got an unexpected keyword argument 'nope'"
    ):
        words.str.startswith(nope="a")
    with pytest.raises(TypeError, match=r"get\(\) got an unexpected keyword argument 'i'"):
        words.str.split("b").list.get(i=0)
    with pytest.raises(TypeError, match=r"clip\(\) got an unexpected keyword argument 'extra'"):
        Series([1.0]).clip(lower=0, upper=1, extra=2)
    with pytest.raises(
        TypeError, match=r"_memory_budget_advice\(\) got an unexpected keyword argument 'nope'"
    ):
        _ext._memory_budget_advice(required_bytes=1, nope=2)


def test_a_table_method_names_itself_in_an_unknown_keyword_error():
    s = Series(["a"])
    with pytest.raises(
        TypeError, match=r"^str_contains\(\) got an unexpected keyword argument 'x'"
    ):
        s.str_contains(x="a")
    with pytest.raises(
        TypeError, match=r"^str_regex_replace\(\) got an unexpected keyword argument 'x'"
    ):
        s.str_regex_replace(pattern="a", to="b", x=1)
    with pytest.raises(TypeError, match=r"^dt_format\(\) got an unexpected keyword argument 'x'"):
        s.dt_format(fmt="%Y", x=1)
    frame = DataFrame({"a": [1]})
    with pytest.raises(
        TypeError, match=r"^column_index\(\) got an unexpected keyword argument 'nope'"
    ):
        frame.column_index("a", nope=1)
    with pytest.raises(
        TypeError, match=r"^partition_id\(\) got an unexpected keyword argument 'extra'"
    ):
        frame.partition_id(keys="a", n_parts=2, extra=1)
