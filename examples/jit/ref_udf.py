# SPDX-License-Identifier: MIT
import math

import numpy as np
import numpy.typing as npt

import dftracer.utils as dft
from dftracer.utils import col

lazy = dft.DataFrame.from_dict(
    {"dur": [1.0, 4.0, 9.0, 16.0], "size": [8192, 100, 8192, 100]}
).lazy()


@dft.udf  # compiled to native code
def score(dur: float, size: int) -> float:
    if size > 4096:
        return dur * 2.0 + math.sqrt(abs(dur))
    return min(dur, 10.0)


@dft.udf(batch=True)  # called once per batch
def zscore(x: npt.NDArray[np.float64]) -> npt.NDArray[np.float64]:
    return (x - x.mean()) / x.std()


out = lazy.with_columns(s=score(col("dur"), col("size")), z=zscore(col("dur"))).collect()
assert out.to_dict()["s"] == [3.0, 4.0, 21.0, 10.0]
score.close()
zscore.close()

with dft.udf.scope():

    @dft.udf
    def tmp(x: float) -> float:
        return x + 1.0

    lazy.with_columns(t=tmp(col("dur"))).collect()
# "tmp" is not registered here
assert "tmp" not in dft.udf
