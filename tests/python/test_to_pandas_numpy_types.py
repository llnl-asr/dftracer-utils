# SPDX-License-Identifier: MIT
import numpy as np
import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame  # noqa: E402


@pytest.mark.parametrize("dtype", [np.int64, np.uint64])
def test_64_bit_integers_are_numpys_own_types(dtype):
    values = np.array([1, 1, 2], dtype=dtype)
    out = DataFrame.from_dict({"k": values, "v": [1.0, 2.0, 3.0]}).to_pandas()
    assert out["k"].to_numpy().dtype.char == np.dtype(dtype).char
    # pandas has no factorizer for longlong or ulonglong, so a merge on
    # repeated keys of those types raises KeyError.
    merged = out.merge(out, on="k")
    assert sorted(merged["k"].tolist()) == [1, 1, 1, 1, 2]
