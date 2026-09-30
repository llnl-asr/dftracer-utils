"""Integer window sums and differences are exact, not wrapped."""

import numpy as np
import pytest

import dftracer.utils as dftu
from dftracer.utils import dftracer_utils_ext as _ext

pytestmark = pytest.mark.skipif(
    not hasattr(_ext, "_dataframe_from_columns"), reason="extension predates native frames"
)

BIG = 1 << 63


def _frame(values):
    return dftu.DataFrame.from_numpy(
        {
            "k": np.zeros(len(values), dtype=np.int64),
            "ts": np.arange(len(values), dtype=np.int64),
            "v": np.array(values, dtype=np.uint64),
        },
        [],
    )


def test_running_sum_past_two_to_the_63():
    out = _frame([BIG, 1, 2]).window(["k"], ["ts"], [("running_sum", "v", "s")])
    assert out["s"].to_list() == [BIG, BIG + 1, BIG + 3]


def test_frame_sum_slides_exactly():
    out = _frame([BIG, 5, 6, 7]).window(["k"], ["ts"], [("frame_sum", "v", 1, 0, "s")])
    assert out["s"].to_list() == [BIG, BIG + 5, 11, 13]


def test_a_sum_outside_the_type_is_an_error():
    with pytest.raises(Exception, match="RUNNING_SUM.*uint64"):
        _frame([(1 << 64) - 1, 1]).window(["k"], ["ts"], [("running_sum", "v", "s")])


def test_delta_outside_int64_is_an_error():
    with pytest.raises(Exception, match="DELTA"):
        _frame([0, (1 << 64) - 1]).window(["k"], ["ts"], [("delta", "v", "d")])
    out = _frame([BIG + 5, BIG]).window(["k"], ["ts"], [("delta", "v", "d")])
    assert out["d"].to_list() == [None, -5]
