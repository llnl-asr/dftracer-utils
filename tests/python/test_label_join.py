# SPDX-License-Identifier: MIT
"""The pandas row-label join: ``DataFrame.join`` with no key and ``merge`` with
``left_index`` / ``right_index``. Every case runs on real pandas and on ours;
the labels, index names, column names, values and errors are compared."""

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame  # noqa: E402

from .common import skip_below_pandas3  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

HOWS = ["left", "right", "inner", "outer"]


def _kind(e):
    for base in (KeyError, NotImplementedError, TypeError, ValueError, IndexError):
        if isinstance(e, base):
            return base.__name__
    return type(e).__name__


def _run(f):
    try:
        return f()
    except Exception as e:  # noqa: BLE001
        return e


def _labels(frame):
    return [None if pd.isna(x) else x for x in frame.index.tolist()]


def _compare(ours, ref):
    ours = ours.to_pandas()
    assert list(ours.columns) == list(ref.columns)
    assert list(ours.index.names) == list(ref.index.names)
    assert _labels(ours) == _labels(ref)
    pd.testing.assert_frame_equal(
        ours.reset_index(drop=True),
        ref.reset_index(drop=True),
        check_dtype=False,
        check_index_type=False,
        check_column_type=False,
    )


def check(make, call):
    """Run ``call`` on the pandas objects ``make()`` builds and on ours."""
    skip_below_pandas3()
    ref = _run(lambda: call(*make()))
    ours = _run(lambda: call(*[DataFrame.from_pandas(x) for x in make()]))
    if isinstance(ref, Exception):
        assert isinstance(ours, Exception), f"pandas raised {ref!r}; ours returned {ours!r}"
        assert _kind(ours) == _kind(ref), (ours, ref)
        return ours
    assert not isinstance(ours, Exception), f"pandas returned; ours raised {ours!r}"
    _compare(ours, ref)
    return ours


def _frames(left_index, right_index, overlap=False):
    left = pd.DataFrame({"a": [1, 2, 3], "v": [1.5, 2.5, 3.5]})
    right = pd.DataFrame({"b": [10, 20], ("v" if overlap else "w"): [5.5, 6.5]})
    if left_index is not None:
        left.index = left_index
    if right_index is not None:
        right.index = right_index
    return left, right


def _idx(values, name="i"):
    return pd.Index(values, name=name)


MI_LEFT = pd.MultiIndex.from_tuples([(1, "a"), (1, "b"), (2, "a")], names=["x", "y"])
MI_RIGHT = pd.MultiIndex.from_tuples([(1, "b"), (2, "a")], names=["x", "y"])

INDEXED = {
    "none": (None, None),
    "named": (_idx([1, 2, 3]), _idx([2, 3])),
    "text": (_idx(["a", "b", "c"]), _idx(["b", "z"])),
    "repeated": (_idx([1, 1, 2]), _idx([1, 1])),
    "disjoint": (_idx([1, 2, 3]), _idx([7, 8])),
    "multi": (MI_LEFT, MI_RIGHT),
}


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("kind", INDEXED)
def test_label_join_each_how(kind, how):
    li, ri = INDEXED[kind]
    check(lambda: _frames(li, ri), lambda a, b: a.join(b, how=how))


@pytest.mark.parametrize("kind", INDEXED)
def test_label_join_defaults_to_left(kind):
    li, ri = INDEXED[kind]
    check(lambda: _frames(li, ri), lambda a, b: a.join(b))


def test_label_join_full_is_outer():
    skip_below_pandas3()
    li, ri = INDEXED["named"]
    ref = _frames(li, ri)[0].join(_frames(li, ri)[1], how="outer")
    a, b = [DataFrame.from_pandas(x) for x in _frames(li, ri)]
    _compare(a.join(b, how="full"), ref)


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("kind", ["none", "named", "repeated", "multi"])
def test_label_join_overlap_with_suffixes(kind, how):
    li, ri = INDEXED[kind]
    check(
        lambda: _frames(li, ri, overlap=True),
        lambda a, b: a.join(b, how=how, suffixes=("_l", "_r")),
    )


@pytest.mark.parametrize("kind", ["none", "named", "multi"])
def test_label_join_overlap_without_suffix_raises(kind):
    li, ri = INDEXED[kind]
    ours = check(lambda: _frames(li, ri, overlap=True), lambda a, b: a.join(b))
    assert "columns overlap but no suffix specified" in str(ours)
    check(lambda: _frames(li, ri, overlap=True), lambda a, b: a.join(b, suffixes=(None, None)))
    check(lambda: _frames(li, ri, overlap=True), lambda a, b: a.join(b, suffixes=("", "")))


@pytest.mark.parametrize("suffixes", [(None, "_r"), ("_l", None), ("_l", ""), ("", "_r")])
def test_label_join_one_side_keeps_its_name(suffixes):
    li, ri = INDEXED["named"]
    check(lambda: _frames(li, ri, overlap=True), lambda a, b: a.join(b, suffixes=suffixes))


def test_label_join_no_overlap_ignores_suffixes():
    li, ri = INDEXED["named"]
    check(lambda: _frames(li, ri), lambda a, b: a.join(b, suffixes=("_l", "_r")))
    check(lambda: _frames(li, ri), lambda a, b: a.join(b, suffixes=(None, None)))


def test_label_join_suffix_applies_to_overlap_only():
    skip_below_pandas3()
    left = pd.DataFrame({"a": [1, 2], "v": [1, 2], "u": [3, 4]})
    right = pd.DataFrame({"v": [5, 6], "w": [7, 8]})
    check(lambda: (left, right), lambda a, b: a.join(b, suffixes=("_l", "_r")))


def test_label_join_keeps_index_names_of_the_left():
    li, ri = _idx([1, 2, 3], "left"), _idx([2, 3], "right")
    check(lambda: _frames(li, ri), lambda a, b: a.join(b))
    check(lambda: _frames(li, ri), lambda a, b: a.join(b, how="outer"))


def test_label_join_many_to_many():
    left = pd.DataFrame({"a": [1, 2, 3, 4]}, index=_idx([1, 1, 2, 2]))
    right = pd.DataFrame({"b": [10, 20, 30]}, index=_idx([1, 1, 2]))
    for how in HOWS:
        check(lambda: (left, right), lambda a, b, how=how: a.join(b, how=how))


def test_label_join_wrong_arguments_raise_type_error():
    skip_below_pandas3()
    li, ri = INDEXED["named"]
    a, b = [DataFrame.from_pandas(x) for x in _frames(li, ri)]
    with pytest.raises(TypeError, match="suffix"):
        a.join(b, suffix="_x")
    with pytest.raises(TypeError, match="suffixes"):
        a.join(b, on="a", suffixes=("_l", "_r"))
    with pytest.raises(TypeError, match="suffixes"):
        a.join(b, left_on="a", right_on="b", suffixes=("_l", "_r"))


def test_label_join_suffixes_must_be_a_pair():
    skip_below_pandas3()
    li, ri = INDEXED["named"]
    a, b = [DataFrame.from_pandas(x) for x in _frames(li, ri, overlap=True)]
    with pytest.raises(ValueError):
        a.join(b, suffixes=("_l",))  # type: ignore[arg-type]


def test_keyless_cross_is_still_cross():
    skip_below_pandas3()
    a = DataFrame({"a": [1, 2]})
    b = DataFrame({"b": [3, 4, 5]})
    out = a.join(b, how="cross").to_pandas()
    assert len(out) == 6
    assert list(out.columns) == ["a", "b"]


def test_keyless_other_hows_keep_their_error():
    skip_below_pandas3()
    a = DataFrame({"a": [1, 2]})
    b = DataFrame({"b": [3, 4]})
    for how in ("semi", "anti"):
        with pytest.raises((TypeError, ValueError)):
            a.join(b, how=how)


def test_keyed_join_defaults_are_unchanged():
    skip_below_pandas3()
    a = DataFrame({"k": [1, 2, 3], "v": [1, 2, 3]})
    b = DataFrame({"k": [2, 3, 4], "v": [5, 6, 7]})
    out = a.join(b, on="k").to_pandas()
    assert list(out.columns) == ["k", "v", "v_right"]
    assert out["k"].tolist() == [2, 3]
    out = a.join(b, on="k", suffix="_o").to_pandas()
    assert list(out.columns) == ["k", "v", "v_o"]
    out = a.join(b, on="k", how="left").to_pandas()
    assert out["k"].tolist() == [1, 2, 3]


# ---- merge ---------------------------------------------------------------------


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("kind", INDEXED)
def test_merge_both_index(kind, how):
    li, ri = INDEXED[kind]
    check(
        lambda: _frames(li, ri, overlap=True),
        lambda a, b: a.merge(b, how=how, left_index=True, right_index=True),
    )


def test_merge_both_index_suffixes():
    li, ri = INDEXED["named"]
    check(
        lambda: _frames(li, ri, overlap=True),
        lambda a, b: a.merge(b, left_index=True, right_index=True, suffixes=("_l", "_r")),
    )
    check(
        lambda: _frames(li, ri, overlap=True),
        lambda a, b: a.merge(b, left_index=True, right_index=True, suffixes=(None, "_r")),
    )
    check(
        lambda: _frames(li, ri, overlap=True),
        lambda a, b: a.merge(b, left_index=True, right_index=True, suffixes=(None, None)),
    )


def _keyed(overlap_key, left_has_index=True, right_has_index=True):
    left = pd.DataFrame(
        {"a": [1, 2, 3], "v": [1, 2, 3], "k": [1, 2, 2]},
        index=_idx([1, 2, 3], "i") if left_has_index else None,
    )
    right = pd.DataFrame(
        {"b": [1, 2], "v": [5, 6], ("k" if overlap_key else "m"): [2, 9]},
        index=_idx([7, 8], "j") if right_has_index else None,
    )
    return left, right


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("overlap_key", [True, False])
@pytest.mark.parametrize("indexed", [True, False])
def test_merge_left_index_right_on(how, overlap_key, indexed):
    key = "k" if overlap_key else "m"
    check(
        lambda: _keyed(overlap_key, indexed, indexed),
        lambda a, b: a.merge(b, how=how, left_index=True, right_on=key),
    )


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("overlap_key", [True, False])
@pytest.mark.parametrize("indexed", [True, False])
def test_merge_left_on_right_index(how, overlap_key, indexed):
    check(
        lambda: _keyed(overlap_key, indexed, indexed),
        lambda a, b: a.merge(b, how=how, left_on="k", right_index=True),
    )


def _repeated():
    left = pd.DataFrame(
        {"a": [1, 2, 3, 4], "k": [2, 1, 2, 5], "v": [1, 2, 3, 4]},
        index=_idx([1, 2, 3, 4], "i"),
    )
    right = pd.DataFrame(
        {"b": [1, 2, 3, 4], "m": [9, 2, 1, 2], "v": [5, 6, 7, 8]},
        index=_idx([9, 2, 1, 2], "j"),
    )
    return left, right


@pytest.mark.parametrize("how", HOWS)
def test_merge_mixed_row_order_with_repeated_keys(how):
    check(lambda: _repeated(), lambda a, b: a.merge(b, how=how, left_on="k", right_index=True))
    check(lambda: _repeated(), lambda a, b: a.merge(b, how=how, left_index=True, right_on="m"))
    check(
        lambda: _repeated(),
        lambda a, b: a.merge(b, how=how, left_on="k", right_index=True, suffixes=("_l", "_r")),
    )


def test_merge_mixed_row_order_with_repeated_keys_without_index():
    for how in HOWS:
        check(
            lambda: [x.reset_index(drop=True) for x in _repeated()],
            lambda a, b, how=how: a.merge(b, how=how, left_on="k", right_index=True),
        )


def test_join_column_named_like_an_index_level():
    skip_below_pandas3()
    left = pd.DataFrame({"a": [1, 2]}, index=_idx([1, 2], "i"))
    right = pd.DataFrame({"i": [5, 6]}, index=_idx([1, 2], "j"))
    ref = left.join(right)
    assert list(ref.columns) == ["a", "i"]
    a, b = DataFrame.from_pandas(left), DataFrame.from_pandas(right)
    with pytest.raises(ValueError, match="index level"):
        a.join(b)


def test_merge_index_level_named_like_a_column():
    skip_below_pandas3()
    left = pd.DataFrame({"a": [1, 2], "k": [1, 2]}, index=_idx([1, 2], "i"))
    right = pd.DataFrame({"i": [5, 6]}, index=_idx([1, 2], "j"))
    left.merge(right, left_on="k", right_index=True)
    a, b = DataFrame.from_pandas(left), DataFrame.from_pandas(right)
    with pytest.raises(ValueError, match="index level"):
        a.merge(b, left_on="k", right_index=True)


def test_merge_mixed_suffixes():
    for suffixes in [("_l", "_r"), (None, "_r"), ("_l", None), ("", "_r")]:
        check(
            lambda: _keyed(True),
            lambda a, b, s=suffixes: a.merge(b, left_index=True, right_on="k", suffixes=s),
        )
        check(
            lambda: _keyed(True),
            lambda a, b, s=suffixes: a.merge(b, left_on="k", right_index=True, suffixes=s),
        )


def test_merge_mixed_several_levels():
    left = pd.DataFrame({"a": [1, 2, 3]}, index=MI_LEFT)
    right = pd.DataFrame({"x": [1, 2], "y": ["b", "a"], "b": [5, 6]})
    check(lambda: (left, right), lambda a, b: a.merge(b, left_index=True, right_on=["x", "y"]))
    check(lambda: (right, left), lambda a, b: a.merge(b, left_on=["x", "y"], right_index=True))


def test_merge_mixed_level_count_mismatch_raises():
    left = pd.DataFrame({"a": [1, 2, 3]}, index=MI_LEFT)
    right = pd.DataFrame({"x": [1, 2], "b": [5, 6]})
    check(lambda: (left, right), lambda a, b: a.merge(b, left_index=True, right_on="x"))


@pytest.mark.parametrize(
    "kwargs",
    [
        {"left_index": True, "left_on": "k", "right_index": True},
        {"left_index": True},
        {"right_index": True},
        {"left_index": True, "right_index": True, "on": "k"},
        {"left_index": True, "right_on": "k", "on": "k"},
        {"left_index": True, "right_index": True, "how": "cross"},
        {"left_index": True, "right_on": "k", "how": "cross"},
    ],
)
def test_merge_invalid_index_combinations_raise(kwargs):
    check(lambda: _keyed(True), lambda a, b: a.merge(b, **kwargs))


def test_merge_keyed_is_unchanged():
    skip_below_pandas3()
    a = DataFrame({"k": [1, 2, 3], "v": [1, 2, 3]})
    b = DataFrame({"k": [2, 3, 4], "v": [5, 6, 7]})
    out = a.merge(b, on="k").to_pandas()
    assert list(out.columns) == ["k", "v_x", "v_y"]
    assert out["k"].tolist() == [2, 3]


def test_pandas_module_merge_forwards_index_arguments():
    skip_below_pandas3()
    from dftracer.utils import pandas as dpd

    li, ri = INDEXED["named"]
    left, right = _frames(li, ri)
    ours = dpd.merge(
        DataFrame.from_pandas(left),
        DataFrame.from_pandas(right),
        how="left",
        left_index=True,
        right_index=True,
    )
    _compare(ours, left.merge(right, how="left", left_index=True, right_index=True))


# ---- index names, MultiIndex levels by name, several keys, nulls_equal -----------

NAN = float("nan")


def _named(labels, name, col):
    return pd.DataFrame({col: list(range(len(labels)))}, index=pd.Index(labels, name=name))


NAME_CASES = {
    "equal": ([1, 2], [1, 2]),
    "cast": ([1, 2], [1.0, 2.0]),
    "float_left": ([1.0, 2.0], [1, 2]),
    "nulls": ([1.0, NAN], [1.0, NAN]),
    "strings_with_nulls": (["a", None], ["a", None]),
    "disjoint": ([1, 2, 3], [2, 3, 4]),
}


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("case", NAME_CASES)
def test_label_join_index_names_follow_pandas(case, how):
    left, right = NAME_CASES[case]
    check(
        lambda: (_named(left, "i", "a"), _named(right, "j", "b")),
        lambda a, b: a.join(b, how=how),
    )
    check(
        lambda: (_named(left, "i", "a"), _named(right, None, "b")),
        lambda a, b: a.join(b, how=how),
    )
    check(
        lambda: (_named(left, None, "a"), _named(right, "j", "b")),
        lambda a, b: a.join(b, how=how),
    )


@pytest.mark.parametrize("how", HOWS)
def test_merge_both_index_names_follow_pandas(how):
    check(
        lambda: (_named([1, 2], "i", "a"), _named([1, 2], "j", "b")),
        lambda a, b: a.merge(b, how=how, left_index=True, right_index=True),
    )


def _mi(tuples, names):
    return pd.MultiIndex.from_tuples(tuples, names=names)


def _multi_left():
    return pd.DataFrame(
        {"a": [1, 2, 3, 4]}, index=_mi([(1, "a"), (1, "b"), (2, "a"), (2, "b")], ["x", "y"])
    )


MULTI_RIGHTS = {
    "same_names_reversed": lambda: pd.DataFrame(
        {"b": [10, 20, 30]}, index=_mi([("a", 1), ("b", 2), ("c", 3)], ["y", "x"])
    ),
    "partly_shared_x": lambda: pd.DataFrame(
        {"b": [10, 20, 30]}, index=_mi([(1, "p"), (2, "q"), (2, "r")], ["x", "z"])
    ),
    "partly_shared_y": lambda: pd.DataFrame(
        {"b": [10, 20, 30]}, index=_mi([("a", "p"), ("a", "q"), ("b", "r")], ["y", "z"])
    ),
    "single_x": lambda: pd.DataFrame({"b": [10, 20]}, index=pd.Index([1, 2], name="x")),
    "single_y": lambda: pd.DataFrame({"b": [10, 20]}, index=pd.Index(["b", "a"], name="y")),
    "no_shared": lambda: pd.DataFrame({"b": [10, 20]}, index=pd.Index([1, 2], name="q")),
    "unnamed": lambda: pd.DataFrame({"b": [10, 20]}, index=_mi([(1, "a"), (2, "b")], [None, None])),
    "unindexed": lambda: pd.DataFrame({"b": [10, 20]}),
}


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("kind", MULTI_RIGHTS)
def test_label_join_matches_multiindex_levels_by_name(kind, how):
    make = MULTI_RIGHTS[kind]
    check(lambda: (_multi_left(), make()), lambda a, b: a.join(b, how=how))
    check(lambda: (make(), _multi_left()), lambda a, b: a.join(b, how=how))


def test_label_join_no_shared_level_names_raises():
    ours = check(lambda: (_multi_left(), MULTI_RIGHTS["no_shared"]()), lambda a, b: a.join(b))
    assert "overlapping index names" in str(ours)


@pytest.mark.parametrize("how", HOWS)
def test_label_join_by_level_name_with_suffixes(how):
    right = lambda: pd.DataFrame(  # noqa: E731
        {"a": [10, 20, 30]}, index=_mi([("a", 1), ("b", 2), ("c", 3)], ["y", "x"])
    )
    check(
        lambda: (_multi_left(), right()),
        lambda a, b: a.join(b, how=how, suffixes=("_l", "_r")),
    )


def _several_keys(indexed):
    index = pd.Index([5, 6, 7, 8], name="j") if indexed else None
    left = pd.DataFrame({"a": [1, 2, 3]}, index=_mi([(1, "a"), (1, "b"), (2, "a")], ["x", "y"]))
    right = pd.DataFrame(
        {"p": [1, 2, 1, 3], "q": ["a", "b", "a", "z"], "b": [10, 20, 30, 40]}, index=index
    )
    return left, right


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("indexed", [True, False])
def test_merge_several_keys_left_index_right_on(how, indexed):
    check(
        lambda: _several_keys(indexed),
        lambda a, b: a.merge(b, how=how, left_index=True, right_on=["p", "q"]),
    )


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("indexed", [True, False])
def test_merge_several_keys_left_on_right_index(how, indexed):
    check(
        lambda: _several_keys(indexed)[::-1],
        lambda a, b: a.merge(b, how=how, left_on=["p", "q"], right_index=True),
    )


@pytest.mark.parametrize("how", HOWS)
def test_merge_single_key_against_a_multiindex(how):
    left = pd.DataFrame({"a": [1, 2, 3]}, index=_mi([(1, "a"), (1, "b"), (2, "a")], ["x", "y"]))
    right = pd.DataFrame({"p": [1, 2, 1, 3], "b": [10, 20, 30, 40]}, index=_idx([5, 6, 7, 8], "j"))
    check(lambda: (left, right), lambda a, b: a.merge(b, how=how, left_index=True, right_on="p"))


@pytest.mark.parametrize(
    "kwargs",
    [
        {"left_index": True, "right_index": True},
        {"left_index": True, "right_on": "k"},
        {"left_on": "k", "right_index": True},
    ],
)
def test_merge_index_forms_refuse_nulls_equal(kwargs):
    skip_below_pandas3()
    a, b = [DataFrame.from_pandas(x) for x in _keyed(True)]
    with pytest.raises(TypeError, match="nulls_equal"):
        a.merge(b, nulls_equal=True, **kwargs)
    a.merge(b, nulls_equal=False, **kwargs)


@pytest.mark.parametrize("how", HOWS)
def test_label_join_one_level_against_several_drops_unmatched_one_level_rows(how):
    multi = pd.DataFrame(
        {"a": [1, 2, 3, 4, 5]},
        index=_mi([(2, "a"), (1, "b"), (2, "c"), (9, "d"), (1, "e")], ["x", "y"]),
    )
    single = pd.DataFrame({"b": [10, 20, 30, 40]}, index=pd.Index([2, 1, 7, 3], name="x"))
    check(lambda: (multi, single), lambda a, b: a.join(b, how=how))
    check(lambda: (single, multi), lambda a, b: a.join(b, how=how))


# ---- round 2: suffix collisions, equal labels, empty key frames, None names ------


def _collide():
    left = pd.DataFrame({"a": [1, 2], "a_l": [3, 4], "k": [1, 2]}, index=_idx([1, 2], "i"))
    right = pd.DataFrame({"a": [5, 6], "m": [1, 2]}, index=_idx([1, 2], "j"))
    return left, right


def test_suffixes_that_collide_with_a_column_raise():
    suffixes = ("_l", "_r")
    ours = check(lambda: _collide(), lambda a, b: a.join(b, suffixes=suffixes))
    assert "duplicate" in str(ours)
    check(
        lambda: _collide(),
        lambda a, b: a.merge(b, left_index=True, right_index=True, suffixes=suffixes),
    )
    check(
        lambda: _collide(),
        lambda a, b: a.merge(b, left_index=True, right_on="m", suffixes=suffixes),
    )
    check(
        lambda: _collide(),
        lambda a, b: a.merge(b, left_on="k", right_index=True, suffixes=suffixes),
    )
    check(lambda: _collide()[::-1], lambda a, b: a.join(b, suffixes=("_l", "_r")))


EQUAL_LABELS = {
    "unsorted": [2, 1, 3],
    "repeated_first": [2, 1, 1],
    "repeated_sorted": [1, 1, 2],
    "strings": ["b", "a", "c"],
    "nulls": [2.0, NAN, 1.0],
}


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("case", EQUAL_LABELS)
def test_label_join_with_equal_labels(case, how):
    labels = EQUAL_LABELS[case]
    check(
        lambda: (_named(labels, "i", "a"), _named(labels, "i", "b")),
        lambda a, b: a.join(b, how=how),
    )
    check(
        lambda: (_named(labels, "i", "a"), _named(labels, "i", "b")),
        lambda a, b: a.merge(b, how=how, left_index=True, right_index=True),
    )


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("repeated", [False, True])
def test_label_join_with_equal_multiindex(repeated, how):
    tuples = [(2, "a"), (1, "b"), (1, "a")] + ([(1, "a")] if repeated else [])
    n = len(tuples)
    check(
        lambda: (
            pd.DataFrame({"a": range(n)}, index=_mi(tuples, ["x", "y"])),
            pd.DataFrame({"b": range(n)}, index=_mi(tuples, ["x", "y"])),
        ),
        lambda a, b: a.join(b, how=how),
    )


def _empty(indexed):
    frame = pd.DataFrame({"a": pd.Series([], dtype="int64"), "k": pd.Series([], dtype="int64")})
    if indexed:
        frame.index = pd.Index([], name="i", dtype="int64")
    return frame


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("indexed", [True, False])
@pytest.mark.parametrize("other_indexed", [True, False])
def test_merge_with_an_empty_key_frame(how, indexed, other_indexed):
    def make():
        other = pd.DataFrame(
            {"b": [1, 2], "m": [5, 6]},
            index=_idx([7, 8], "j") if other_indexed else None,
        )
        return _empty(indexed), other

    check(make, lambda a, b: a.merge(b, how=how, left_index=True, right_on="m"))
    check(
        lambda: make()[::-1],
        lambda a, b: a.merge(b.rename(columns={"k": "k2"}), how=how, left_on="m", right_index=True),
    )


def test_index_levels_of_an_empty_unindexed_frame_are_int64():
    skip_below_pandas3()
    frame = DataFrame.from_pandas(_empty(False))
    reference = DataFrame.from_pandas(pd.DataFrame({"a": [1]})).index_levels()[0]
    assert frame.index_levels()[0].dtype == reference.dtype
    assert frame.index.dtype == reference.dtype


def test_unnamed_levels_on_both_sides_are_ambiguous():
    skip_below_pandas3()
    left = pd.DataFrame({"a": [1, 2]}, index=_mi([(1, "a"), (2, "b")], ["x", None]))
    right = pd.DataFrame({"b": [1, 2]}, index=_mi([(1, "p"), (2, "q")], [None, "z"]))
    a, b = DataFrame.from_pandas(left), DataFrame.from_pandas(right)
    with pytest.raises(ValueError, match="unnamed"):
        a.join(b)
    single = pd.DataFrame({"b": [1, 2]})
    with pytest.raises(ValueError, match="unnamed"):
        a.join(DataFrame.from_pandas(single))


def _index_kind(frame):
    return frame.index.dtype.kind


@pytest.mark.parametrize("how", HOWS)
@pytest.mark.parametrize("labels", [[2, 1, 1], [2, 1, 3], [1, 1, 2]])
def test_label_join_with_equal_labels_of_different_types(labels, how):
    skip_below_pandas3()
    floats = [float(x) for x in labels]
    for left, right in ((labels, floats), (floats, labels)):
        pa, pb = _named(left, "i", "a"), _named(right, "i", "b")
        calls = (
            lambda a, b: a.join(b, how=how),
            lambda a, b: a.merge(b, how=how, left_index=True, right_index=True),
        )
        for call in calls:
            ours = call(DataFrame.from_pandas(pa), DataFrame.from_pandas(pb))
            ref = call(pa, pb)
            _compare(ours, ref)
            assert _index_kind(ours.to_pandas()) == _index_kind(ref), (how, left, right)
    check(
        lambda: (_named(labels, "i", "a"), _named(floats, "j", "b")),
        lambda a, b: a.merge(b, how=how, left_index=True, right_index=True),
    )
