# SPDX-License-Identifier: MIT
"""The reflected arithmetic methods compute `other <op> self`, as pandas does,
for a Series operand as for a number."""

import pytest

import dftracer.utils as dft


@pytest.mark.parametrize("name", ["rfloordiv", "rmod", "rpow", "rdiv", "rtruediv"])
def test_reflected_methods_compute_other_op_self_like_pandas(name):
    pd = pytest.importorskip("pandas")
    left, right = [7, 9, 11], [2, 4, 3]
    mine = getattr(dft.Series(left), name)(dft.Series(right))
    want = getattr(pd.Series(left), name)(pd.Series(right))
    assert [float(v) for v in mine.to_list()] == [float(v) for v in want.tolist()]
    scalar = getattr(dft.Series(left), name)(5)
    want_scalar = getattr(pd.Series(left), name)(5)
    assert [float(v) for v in scalar.to_list()] == [float(v) for v in want_scalar.tolist()]
