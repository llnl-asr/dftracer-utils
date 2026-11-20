# SPDX-License-Identifier: MIT
"""Typed Series sample: ty and mypy --strict check this clean.

Each group of public Series methods has its static type asserted: element
access, the methods that keep the element type, predicates, reductions,
arithmetic promotion, ``cast`` on a dtype literal, the string accessor, the
windows and covariance.
"""

from typing import List, Optional, cast

from typing_extensions import assert_type

from dftracer.utils import DType, Series
from dftracer.utils.dftracer_utils_ext import Scalar

ints = cast("Series[int]", None)
floats = cast("Series[float]", None)
strs = cast("Series[str]", None)
bools = cast("Series[bool]", None)
anything = cast("Series[Scalar]", None)

# Element access.
assert_type(ints[0], Optional[int])
assert_type(strs[0], Optional[str])
assert_type(ints[1:2], "Series[int]")
assert_type(ints.to_list(), List[Optional[int]])
assert_type(floats.tolist(), List[Optional[float]])
assert_type(ints.item(0), Optional[int])
assert_type(len(ints), int)
assert_type(ints.name, Optional[str])
assert_type(ints.dtype, DType)
assert_type(ints.length, int)
assert_type(ints.null_count, int)

# Methods that keep the element type return Self.
assert_type(ints.head(), "Series[int]")
assert_type(ints.tail(2), "Series[int]")
assert_type(floats.sort(descending=True), "Series[float]")
assert_type(strs.unique(), "Series[str]")
assert_type(ints.take([0, 1]), "Series[int]")
assert_type(ints.slice(0, 1), "Series[int]")
assert_type(ints.shift(1), "Series[int]")
assert_type(ints.clip(0, 5), "Series[int]")
assert_type(ints.abs(), "Series[int]")
assert_type(floats.round(2), "Series[float]")
assert_type(ints.fillna(0), "Series[int]")
assert_type(ints.drop_nulls(), "Series[int]")
assert_type(ints.reverse(), "Series[int]")
assert_type(ints.sample(2, seed=1), "Series[int]")
assert_type(ints.top_k(2), "Series[int]")
assert_type(ints.ffill(), "Series[int]")
assert_type(ints.rename("x"), "Series[int]")
assert_type(ints.share(), "Series[int]")
assert_type(ints.nlargest(2), "Series[int]")
assert_type(ints.sort_values(), "Series[int]")
assert_type(ints.filter(bools), "Series[int]")

# Comparisons and predicates are boolean masks.
assert_type(ints > 1, "Series[bool]")
assert_type(ints == 1, "Series[bool]")
assert_type(ints != 1, "Series[bool]")
assert_type(ints.gt(1), "Series[bool]")
assert_type(ints.is_null(), "Series[bool]")
assert_type(ints.is_null_at(0), bool)
assert_type(ints.isna(), "Series[bool]")
assert_type(ints.notna(), "Series[bool]")
assert_type(ints.is_in([1, 2]), "Series[bool]")
assert_type(floats.is_nan(), "Series[bool]")
assert_type(ints.is_between(1, 2), "Series[bool]")
assert_type(strs.str_contains("a"), "Series[bool]")
assert_type(bools & bools, "Series[bool]")
assert_type(bools | bools, "Series[bool]")
assert_type(~bools, "Series[bool]")

# Reductions.
assert_type(ints.sum(), int)
assert_type(bools.sum(), int)
assert_type(floats.sum(), float)
assert_type(ints.mean(), "Optional[float]")
assert_type(ints.min(), "Optional[int]")
assert_type(floats.max(), "Optional[float]")
assert_type(strs.min(), "Optional[str]")
assert_type(ints.count(), int)
assert_type(ints.median(), "Optional[float]")
assert_type(ints.var(), "Optional[float]")
assert_type(ints.std(ddof=0), "Optional[float]")
assert_type(ints.quantile(0.5), "Optional[float]")
assert_type(ints.nunique(), int)
assert_type(ints.arg_max(), int)
assert_type(bools.all(), bool)
assert_type(bools.any(), bool)
assert_type(ints.is_unique, bool)

# Arithmetic follows promotion.
assert_type(ints + 1, "Series[int]")
assert_type(ints + ints, "Series[int]")
assert_type(1 + ints, "Series[int]")
assert_type(ints - 1, "Series[int]")
assert_type(ints * 2, "Series[int]")
assert_type(ints + 1.5, "Series[float]")
assert_type(floats * 2, "Series[float]")
assert_type(floats - floats, "Series[float]")
assert_type(ints / 2, "Series[float]")
assert_type(ints.div(2), "Series[float]")
assert_type(bools + 1, "Series[int]")

# cast and astype pick the type from the dtype literal.
assert_type(ints.cast(DType.FLOAT64), "Series[float]")
assert_type(floats.astype(DType.INT64), "Series[int]")
assert_type(ints.astype(DType.STRING), "Series[str]")
assert_type(ints.astype(DType.BOOL), "Series[bool]")
assert_type(ints.astype(float), "Series[float]")
assert_type(ints.astype(str), "Series[str]")
assert_type(ints.astype("int64"), "Series[Scalar]")

# Strings.
assert_type(strs.str.upper(), "Series[str]")
assert_type(strs.str.len(), "Series[int]")
assert_type(strs.str.contains("a"), "Series[bool]")
assert_type(strs.str.startswith("a"), "Series[bool]")
assert_type(strs.str.find(sub="a"), "Series[int]")
assert_type(strs.str.replace_all("a", "b"), "Series[str]")
assert_type(strs.str.zfill(3), "Series[str]")
assert_type(strs.str.pad_start(3), "Series[str]")
assert_type(anything.list.len(), "Series[int]")
assert_type(anything.list.get(0), "Series[Scalar]")
assert_type(strs.to_uppercase(), "Series[str]")
assert_type(strs.str_len_chars(), "Series[int]")
assert_type(ints.hex64_format(), "Series[str]")

# Windows and ranks.
assert_type(ints.rolling(3, "sum"), "Series[float]")
assert_type(ints.rolling(3).mean().abs(), "Series[float]")
assert_type(ints.rolling_mean(3), "Series[float]")
assert_type(ints.ewm(alpha=0.5).mean(), "Series[float]")
assert_type(ints.rolling(3).quantile(0.5), "Series[float]")
assert_type(ints.ewm(span=3).std(), "Series[float]")
assert_type(ints.rank(), "Series[float]")
assert_type(ints.cut(ints), "Series[int]")
assert_type(ints.pct_change(), "Series[float]")
assert_type(floats.sqrt(), "Series[float]")

# apply keeps the callback's result type.


def to_float(v: Optional[int]) -> float:
    return float(v or 0)


assert_type(ints.apply(lambda v: str(v)), "Series[str]")
assert_type(ints.map(to_float), "Series[float]")

# Series is covariant in its element type.


def takes_ints(s: "Series[int]") -> None: ...
def takes_floats(s: "Series[float]") -> None: ...
def takes_scalars(s: "Series[Scalar]") -> None: ...


takes_ints(bools)
takes_floats(ints)
takes_scalars(ints)
takes_scalars(strs)
takes_scalars(anything)
assert_type(anything.abs(), "Series[Scalar]")
