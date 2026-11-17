# SPDX-License-Identifier: MIT
"""The Series arithmetic and comparison operators are the native number and
rich-compare slots: no Python method sits between `s + 1` and the kernel."""

import operator

import pytest

import dftracer.utils as dft
from dftracer.utils import dftracer_utils_ext as ext

DUNDERS = [
    "__add__",
    "__radd__",
    "__sub__",
    "__rsub__",
    "__mul__",
    "__rmul__",
    "__truediv__",
    "__floordiv__",
    "__rfloordiv__",
    "__mod__",
    "__rmod__",
    "__pow__",
    "__rpow__",
    "__eq__",
    "__ne__",
    "__lt__",
    "__le__",
    "__gt__",
    "__ge__",
]


@pytest.mark.parametrize("name", DUNDERS)
def test_the_operator_is_native(name):
    assert name in ext.Series.__dict__
    assert name not in dft.Series.__dict__


def _ints():
    s = dft.Series([1, -2, 3, None, 5])
    s.name = "a"
    return s


def test_scalar_on_either_side_runs_the_kernel_and_keeps_the_name():
    s = _ints()
    for out, want in (
        (s + 1, [2, -1, 4, None, 6]),
        (1 + s, [2, -1, 4, None, 6]),
        (s - 1, [0, -3, 2, None, 4]),
        (10 - s, [9, 12, 7, None, 5]),
        (s * 2, [2, -4, 6, None, 10]),
        (2 * s, [2, -4, 6, None, 10]),
        (s // 2, [0, -1, 1, None, 2]),
        (7 // dft.Series([2, 3]), [3, 2]),
        (s % 3, [1, 1, 0, None, 2]),
        (2 ** dft.Series([1, 2, 3]), [2, 4, 8]),
    ):
        assert type(out) is dft.Series
        assert out.to_list() == want
    assert (s + 1).name == "a" and (1 - s).name == "a"


def test_true_division_widens_integers_to_float():
    s = _ints()
    out = s / 2
    assert out.to_list() == [0.5, -1.0, 1.5, None, 2.5] and int(out.dtype) == int(dft.DType.FLOAT64)
    assert (s / dft.Series([2, 4, 2, 2, 5])).to_list() == [0.5, -0.5, 1.5, None, 1.0]
    assert (6 / dft.Series([2, 3])).to_list() == [3.0, 2.0]


@pytest.mark.parametrize("bad", ["a", True, None, [1], object()])
@pytest.mark.parametrize("op", [operator.add, operator.sub, operator.mul, operator.truediv])
def test_a_non_number_operand_is_not_implemented(op, bad):
    with pytest.raises(TypeError):
        op(_ints(), bad)


def test_comparisons_make_masks_against_scalars_strings_and_series():
    s = _ints()
    assert (s > 1).to_list() == [False, False, True, None, True]
    assert (1 < s).to_list() == [False, False, True, None, True]
    assert (s == s).to_list() == [True, True, True, None, True]
    assert (s != 3).to_list() == [True, True, False, None, True]
    words = dft.Series(["a", "b", None])
    assert (words == "a").to_list() == [True, False, None]
    assert (s <= dft.Series([1, 1, 3, 4, 4])).to_list() == [True, True, True, None, False]


def test_a_series_is_unhashable_because_equality_is_a_mask():
    with pytest.raises(TypeError):
        hash(_ints())


class _Answers:
    def __eq__(self, other):
        return "answered"

    def __radd__(self, other):
        return "radd"

    __hash__ = None


def test_an_operand_that_defines_the_operator_answers():
    s = _ints()
    assert (s == _Answers()) == "answered"
    assert (_Answers() == s) == "answered"
    assert s + _Answers() == "radd"


@pytest.mark.parametrize("op", [operator.lt, operator.le, operator.gt, operator.ge])
def test_ordering_against_an_unsupported_operand_is_a_type_error(op):
    with pytest.raises(TypeError):
        op(_ints(), object())


def test_equality_against_an_unsupported_operand_is_not_implemented():
    s = _ints()
    assert s.__eq__([1]) is NotImplemented
    assert s.__ne__(object()) is NotImplemented
    assert s.__lt__(object()) is NotImplemented


def test_comparing_with_none_matches_pandas():
    pd = pytest.importorskip("pandas")
    for values in ([1, 2, None], ["a", None], [1.5, None], [True, None]):
        mine, want = dft.Series(values), pd.Series(values)
        assert (mine == None).to_list() == (want == None).tolist()  # noqa: E711
        assert (mine != None).to_list() == (want != None).tolist()  # noqa: E711
        assert (None == mine).to_list() == (None == want).tolist()  # noqa: E711
    named = dft.Series([1, None])
    named.name = "n"
    assert (named == None).name == "n"  # noqa: E711


def test_a_series_can_be_searched_for_in_a_list_holding_none():
    s = _ints()
    assert s in [None, s]
