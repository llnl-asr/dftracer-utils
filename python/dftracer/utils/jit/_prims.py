# SPDX-License-Identifier: MIT
"""Numeric primitives usable in a jit body (authoring markers)."""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Callable,
)

from .._jit_errors import JitError

if TYPE_CHECKING:
    pass


# ---- numeric primitives -----------------------------------------------------
# Authoring markers, not implementations. Inside @jit.each_event each call is
# AST-lowered to the matching prims.h helper (see _PRIM_C) - the C in prims.h is
# the single source of truth, so there is no Python reimplementation to drift
# from it. Calling one directly in Python raises, just like a monoid factory's
# value cannot be read at Python runtime.


def _prim(name: str) -> Callable[..., int]:
    def marker(*args: int) -> int:
        raise JitError(
            f"jit.{name} is a plugin primitive; use it inside a @jit.each_event "
            f"body, where it lowers to dftu_{name}_* in C."
        )

    marker.__name__ = name
    return marker


ilog2 = _prim("ilog2")
"""Floor of log2(x): the highest set-bit index - the natural log-scale histogram
bucket. Lowers to ``dftu_ilog2_u64``."""

bit_width = _prim("bit_width")
"""Bits needed to represent x (``ilog2 + 1``); 0 for 0. ``dftu_bit_width_u64``."""

clz = _prim("clz")
"""Leading zero bits of a 64-bit x (64 for 0). ``dftu_clz_u64``."""

ctz = _prim("ctz")
"""Trailing zero bits of a 64-bit x (64 for 0). ``dftu_ctz_u64``."""

popcount = _prim("popcount")
"""Number of set bits in a 64-bit x. ``dftu_popcount_u64``."""

ceil_pow2 = _prim("ceil_pow2")
"""Smallest power of two >= x (1 for 0). ``dftu_ceil_pow2_u64``."""

floor_pow2 = _prim("floor_pow2")
"""Largest power of two <= x; 0 for 0. ``dftu_floor_pow2_u64``."""

rotl = _prim("rotl")
"""Rotate a 64-bit x left by r bits (mod 64). ``dftu_rotl_u64``."""

rotr = _prim("rotr")
"""Rotate a 64-bit x right by r bits (mod 64). ``dftu_rotr_u64``."""

abs = _prim("abs")
"""Absolute value. ``dftu_abs_i64``."""

clamp = _prim("clamp")
"""Clamp x to the inclusive range [lo, hi]. ``dftu_clamp_i64``."""

div_ceil = _prim("div_ceil")
"""Ceiling of a / b; 0 when b is 0. ``dftu_div_ceil_i64``."""

div_round = _prim("div_round")
"""a / b rounded to nearest, half away from zero; 0 when b is 0. ``dftu_div_round_i64``."""

align_up = _prim("align_up")
"""Round x up to the next multiple of a; x when a is 0. ``dftu_align_up_u64``."""

align_down = _prim("align_down")
"""Round x down to the previous multiple of a; x when a is 0. ``dftu_align_down_u64``."""

isqrt = _prim("isqrt")
"""Integer square root (largest r with r*r <= x). ``dftu_isqrt_u64``."""

gcd = _prim("gcd")
"""Greatest common divisor. ``dftu_gcd_u64``."""

mul_hi = _prim("mul_hi")
"""High 64 bits of the 128-bit product a*b. ``dftu_mul_hi_u64``."""

mix64 = _prim("mix64")
"""SplitMix64 finalizer: a fast, well-avalanched hash of x - good for
hash-bucketing or deterministic sampling. ``dftu_mix64``."""

fastrange = _prim("fastrange")
"""Map x uniformly into [0, n) without a modulo (Lemire's reduction); pair with
:func:`mix64` for hash bucketing. 0 when n is 0. ``dftu_fastrange_u64``."""


def _primf(name: str) -> Callable[..., float]:
    def marker(*args: float) -> float:
        raise JitError(
            f"jit.{name} is a plugin primitive; use it inside a @jit.each_event "
            f"body, where it lowers to dftu_{name}_* in C."
        )

    marker.__name__ = name
    return marker


fmin = _primf("fmin")
"""Lesser of two doubles. ``dftu_min_f64``."""

fmax = _primf("fmax")
"""Greater of two doubles. ``dftu_max_f64``."""

clampf = _primf("clampf")
"""Clamp a double x to the inclusive range [lo, hi]. ``dftu_clamp_f64``."""

sqrt = _primf("sqrt")
"""Square root. ``dftu_sqrt_f64``."""

log2 = _primf("log2")
"""Base-2 logarithm (contrast :func:`ilog2`, the integer bit index). ``dftu_log2_f64``."""

log = _primf("log")
"""Natural logarithm. ``dftu_log_f64``."""

exp = _primf("exp")
"""Natural exponential. ``dftu_exp_f64``."""

fma = _primf("fma")
"""Fused multiply-add ``a*b + c`` with a single rounding. ``dftu_fma_f64``."""

lerp = _primf("lerp")
"""Linear interpolation from a to b by t (unclamped). ``dftu_lerp_f64``."""

copysign = _primf("copysign")
"""Magnitude of x with the sign of y. ``dftu_copysign_f64``."""
