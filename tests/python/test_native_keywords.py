# SPDX-License-Identifier: MIT
"""Native Series methods that replaced Python wrappers keep the keyword names
of the wrappers' signatures.

The list is frozen from the signatures before the wrappers were deleted; it is
not computed from the running class.
"""

import pytest

from dftracer.utils import dftracer_utils_ext as _ext
from dftracer.utils.series import Series

# (method, keyword names of the old Python signature, in order)
OLD_KEYWORDS = [
    ("add", ("other",)),
    ("add_scalar", ("v",)),
    ("bottom_k", ("k",)),
    ("child", ("i",)),
    ("div_scalar", ("v",)),
    ("dot", ("other",)),
    ("dt_format", ("fmt", "unit")),
    ("dt_part", ("code", "unit")),
    ("dt_round", ("every", "mode")),
    ("ewm_mean", ("alpha",)),
    ("ewm_std", ("alpha",)),
    ("fillna", ("value",)),
    ("filter", ("mask",)),
    ("floordiv", ("other",)),
    ("is_between", ("lo", "hi")),
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
    ("rolling_median", ("window",)),
    ("rolling_quantile", ("window", "q")),
    ("rolling_std", ("window",)),
    ("rolling_var", ("window",)),
    ("search_sorted", ("values",)),
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
    assert s.fillna(value=0).to_list() == s.fillna(0).to_list() == [1.0, 0.0, 3.0, 4.0]
    assert s.quantile(q=0.5) == s.quantile(0.5)
    assert s.is_between(lo=1, hi=3).to_list() == s.is_between(1, 3).to_list()
    mask = Series([True, False, True, False])
    assert s.filter(mask=mask).to_list() == s.filter(mask).to_list() == [1.0, 3.0]
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
        s.is_between(lo=1, hi=2, nope=3)
    with pytest.raises(TypeError):
        s.is_between(lo=1)
