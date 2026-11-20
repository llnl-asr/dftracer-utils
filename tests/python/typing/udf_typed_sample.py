# SPDX-License-Identifier: MIT
"""Typed @dft.udf sample: ty and mypy --strict check this clean.

Both forms decorate a plain function and give an owning Udf; the registry stays
a read-only mapping and the scope is a context manager.
"""

import math
from typing import Mapping

import numpy as np
import numpy.typing as npt

import dftracer.utils as dft
from dftracer.utils import Expr, Udf, col


@dft.udf
def score(x: float, n: int) -> float:
    if n > 2:
        return x * 2.0 + math.sqrt(abs(x))
    return min(x, 10.0) - n


@dft.udf(batch=True)
def scaled(x: npt.NDArray[np.float64]) -> npt.NDArray[np.float64]:
    return x * 2.0


@dft.udf(batch=True, name="flagged", returns=np.bool_, volatile=True)
def is_flagged(x: npt.NDArray[np.int64]) -> npt.NDArray[np.bool_]:
    return x > 3


call: Expr = score(col("x"), col("n"))
batch_call: Expr = scaled(col("x"))
owned: Udf = score
name: str = score.name
closed: bool = score.is_closed
function = score.func
registry: Mapping[str, Udf] = dft.udf

with dft.udf.scope():

    @dft.udf(name="inner")
    def inner(x: float) -> float:
        return x + 1.0

    inner.close()

with score as handle:
    handle.close()
