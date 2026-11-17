# SPDX-License-Identifier: MIT
"""Deliberately-wrong @dft.udf usage: ty and mypy must flag every line below."""

import dftracer.utils as dft
from dftracer.utils import Udf


@dft.udf
def score(x: float) -> float:
    return x


dft.udf(batch="yes")  # batch is a bool
dft.udf(name=3)  # name is a str
dft.udf(volatile=None)  # volatile is a bool
dft.udf(1, 2)  # at most the function, positionally
score.close(1)  # close takes no argument
score.nme  # not an attribute
dft.udf.scope(1)  # scope takes no argument
dft.udf["score"] = score  # the registry is read only
text: str = score  # a Udf is not a str
size: Udf = dft.udf  # the registry is not one Udf
