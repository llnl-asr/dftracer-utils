# SPDX-License-Identifier: MIT
"""A negative ``n`` of ``groupby().head`` / ``tail`` keeps all but the last
(head) or first (tail) ``-n`` rows of each group, and ``DataFrame({"x": s})``
keeps the labels of a labelled Series (aligning several on the union of their
labels); both run on real pandas and on ours and the results are compared."""

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

DATA = {
    "k": ["b", "a", "b", "a", "a", "c", "c", None],
    "h": [1, 1, 2, 1, 2, 1, 1, 1],
    "v": [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0],
}


def _check(x, y):
    x = x.to_pandas()
    assert [None if i != i else i for i in x.index.tolist()] == y.index.tolist()
    if isinstance(y, pd.DataFrame):
        assert list(x.columns) == list(y.columns)
        assert x.to_dict("list") == y.to_dict("list")
    else:
        assert x.tolist() == y.tolist()


@pytest.mark.parametrize("n", [-1, -2, -3, -10, 0, 1])
@pytest.mark.parametrize("pick", ["head", "tail"])
@pytest.mark.parametrize("dropna", [True, False])
@pytest.mark.parametrize("sort", [True, False])
@pytest.mark.parametrize("select", [None, "v", ["v"], ["h", "v"]])
def test_a_negative_group_pick_keeps_all_but_the_edge(n, pick, dropna, sort, select):
    ours, theirs = DataFrame(DATA).groupby("k", dropna=dropna, sort=sort), None
    theirs = pd.DataFrame(DATA).groupby("k", dropna=dropna, sort=sort)
    if select is not None:
        ours, theirs = ours[select], theirs[select]
    _check(getattr(ours, pick)(n), getattr(theirs, pick)(n))


@pytest.mark.parametrize("pick", ["head", "tail"])
def test_a_negative_pick_over_two_keys_and_labelled_rows(pick):
    p = pd.DataFrame(DATA).set_index(pd.Index(list("abcdefgh"), name="r"))
    ours = DataFrame.from_pandas(p)
    _check(getattr(ours.groupby(["k", "h"]), pick)(-1), getattr(p.groupby(["k", "h"]), pick)(-1))


def _frames(ours, theirs):
    x = ours.to_pandas()
    assert x.index.tolist() == theirs.index.tolist()
    assert list(x.columns) == list(theirs.columns)
    pd.testing.assert_frame_equal(
        x, theirs, check_dtype=False, check_index_type=False, check_names=False
    )


def test_a_frame_from_a_sorted_series_keeps_its_labels():
    ps = pd.Series([3, 1, 2], name="s").sort_values()
    _frames(DataFrame({"x": Series.from_pandas(ps)}), pd.DataFrame({"x": ps}))


def test_a_frame_from_a_set_index_series_keeps_its_labels():
    p = pd.DataFrame({"a": [5, 6, 7], "b": [1, 2, 3]}).set_index("a")
    ours = DataFrame.from_pandas(p)
    _frames(DataFrame({"x": ours["b"]}), pd.DataFrame({"x": p["b"]}))


def test_series_with_equal_labels_share_them():
    a, b = pd.Series([1, 2, 3], index=[9, 8, 7]), pd.Series([4, 5, 6], index=[9, 8, 7])
    ours = DataFrame({"a": Series.from_pandas(a), "b": Series.from_pandas(b)})
    _frames(ours, pd.DataFrame({"a": a, "b": b}))


def test_series_with_different_labels_align_on_the_union():
    a, b = pd.Series([1, 2, 3], index=[1, 2, 3]), pd.Series([4.0, 5.0], index=[3, 4])
    ours = DataFrame({"a": Series.from_pandas(a), "b": Series.from_pandas(b)})
    _frames(ours, pd.DataFrame({"a": a, "b": b}).astype({"a": "float64"}))


def test_a_list_next_to_a_labelled_series_is_positional():
    a = pd.Series([1, 2, 3], index=[9, 8, 7])
    ours = DataFrame({"a": Series.from_pandas(a), "b": [4, 5, 6]})
    _frames(ours, pd.DataFrame({"a": a, "b": [4, 5, 6]}))


def test_a_dict_of_lists_stays_unlabelled():
    assert DataFrame({"a": [1, 2], "b": [3, 4]}).index_names() == []


def test_series_with_different_labels_next_to_a_plain_column_are_refused():
    a, b = pd.Series([1, 2], index=[1, 2]), pd.Series([4, 5], index=[7, 8])
    with pytest.raises(ValueError, match="different labels"):
        DataFrame({"a": Series.from_pandas(a), "b": Series.from_pandas(b), "c": [1, 2]})


def test_a_column_named_like_its_level_keeps_the_column_and_the_labels():
    p = pd.DataFrame({"a": [1, 2, 2, 3, 3, 3]})
    vc = p["a"].value_counts().sort_index()
    ours = DataFrame({"a": DataFrame.from_pandas(p)["a"].value_counts().sort_index()})
    theirs = pd.DataFrame({"a": vc})
    assert ours.columns == ["a"]
    assert ours.to_pandas()["a"].tolist() == theirs["a"].tolist()
    assert ours.to_pandas().index.tolist() == theirs.index.tolist()


def test_the_polars_group_by_refuses_a_negative_n():
    g = DataFrame({"k": ["a", "a", "a", "b"], "v": [1, 2, 3, 4]}).group_by("k")
    for pick in (g.head, g.tail):
        with pytest.raises(ValueError, match="negative"):
            pick(-1)
    assert sorted(g.head(1)["v"].to_list()) == [1, 4]


def _labelled():
    s = Series.from_list([3, 1, 2]).sort_values()
    return DataFrame({"x": s})


def test_the_polars_names_leave_out_hidden_default_labels():
    f = _labelled()
    assert f.index_names() != []
    assert f.to_dicts() == [{"x": 1}, {"x": 2}, {"x": 3}]
    assert f.rows() == [(1,), (2,), (3,)]
    assert list(f.iter_rows()) == [(1,), (2,), (3,)]
    assert f.row(0) == (1,) and f.row(0, named=True) == {"x": 1}
    assert f.width == 1 and f.shape == (3, 1)
    assert list(f.schema) == ["x"]
    assert f.columns == ["x"] and len(f.dtypes) == 1
    assert [c.name for c in f.get_columns()] == ["x"]
    assert [c.name for c in f.iter_columns()] == ["x"]
    assert f.to_series(0).name == "x"
    assert f.item(0, 0) == 1
    assert f.to_pandas().index.tolist() == [1, 2, 0]


def test_named_levels_stay_in_the_polars_names():
    f = DataFrame({"a": [1, 2], "b": [3, 4]}).set_index("a")
    assert f.width == 2 and list(f.schema) == ["a", "b"]


def test_a_duplicate_label_series_next_to_different_labels_is_refused():
    a, b = pd.Series([1, 2, 3], index=[1, 1, 2]), pd.Series([4, 5], index=[2, 3])
    with pytest.raises(ValueError, match="duplicate labels"):
        DataFrame({"a": Series.from_pandas(a), "b": Series.from_pandas(b)})
