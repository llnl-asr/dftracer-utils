# SPDX-License-Identifier: MIT
"""Normalizes a window spec to the fixed tuple the native kernel reads."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, Sequence

if TYPE_CHECKING:
    from .dftracer_utils_ext import WindowTuple

# Matches WINDOW_UNBOUNDED (int64 max): a frame bound of None means that side of
# the ROWS frame runs to the partition edge.
_WINDOW_UNBOUNDED = (1 << 63) - 1

_WINDOW_NULLARY = ("row_number", "rank", "dense_rank")
_WINDOW_VALUE_ONLY = (
    "running_sum",
    "running_min",
    "running_max",
    "running_count",
    "running_prod",
    "delta",
    "first_value",
    "last_value",
    "fill_forward",
)
_WINDOW_FRAME = (
    "frame_sum",
    "frame_min",
    "frame_max",
    "frame_count",
    "frame_mean",
    "frame_var",
    "frame_std",
    "frame_count_distinct",
    "frame_collect",
)
_WINDOW_FRAME_Q = "frame_quantile"
_WINDOW_FRAME_BY = ("frame_arg_max", "frame_arg_min")


def _window_bound(v: int | None) -> int:
    return _WINDOW_UNBOUNDED if v is None else int(v)


def _window_arity(spec: Sequence[object], n: int) -> None:
    if len(spec) != n:
        raise ValueError(
            f"window: {spec[0]!r} spec expects {n} elements, got {len(spec)}: {spec!r}"
        )


# A spec is a heterogeneous positional tuple (str, col names, ints, None) read
# by index, so its elements are genuinely Any; the output 9-tuple is mixed too.
def norm_window_spec(spec: Sequence[Any]) -> WindowTuple:
    # Normalize to the fixed 12-tuple the native kernel reads: (func,
    # value|None, offset, name, time|None, threshold, counter, frame_pre,
    # frame_post, end|None, span, range_frame).
    if not isinstance(spec, (tuple, list)) or not spec:
        raise ValueError(f"window: bad spec {spec!r}")
    func = spec[0]
    value: str | None = None
    offset = 0
    name: str | None = None
    time: str | None = None
    threshold = 0.0
    counter = False
    pre = 0
    post = 0
    end: str | None = None
    span = 0.0
    range_frame = False
    if func in _WINDOW_NULLARY:
        _window_arity(spec, 2)
        name = spec[1]
    elif func in ("lag", "lead"):
        _window_arity(spec, 4)
        value, offset, name = spec[1], int(spec[2]), spec[3]
    elif func in _WINDOW_VALUE_ONLY:
        _window_arity(spec, 3)
        value, name = spec[1], spec[2]
    elif func == "rate":
        if len(spec) not in (4, 5):
            raise ValueError(f"window: 'rate' spec expects 4 or 5 elements: {spec!r}")
        value, time, name = spec[1], spec[2], spec[3]
        counter = bool(spec[4]) if len(spec) == 5 else False
    elif func == "sessionize":
        if len(spec) not in (4, 5, 6):
            raise ValueError(f"window: 'sessionize' spec expects 4 to 6 elements: {spec!r}")
        time, threshold, name = spec[1], float(spec[2]), spec[3]
        end = spec[4] if len(spec) > 4 else None
        span = float(spec[5]) if len(spec) > 5 else 0.0
    elif func in _WINDOW_FRAME or func == _WINDOW_FRAME_Q or func in _WINDOW_FRAME_BY:
        extra = func == _WINDOW_FRAME_Q or func in _WINDOW_FRAME_BY
        if len(spec) not in ((8,) if extra else (5, 6, 7)):
            raise ValueError(
                f"window: {func!r} spec expects {8 if extra else '5 to 7'} elements: {spec!r}"
            )
        if func == _WINDOW_FRAME_Q:
            threshold = float(spec[7])
        elif extra:
            end = spec[7]
            if not isinstance(end, str):
                raise ValueError(f"window: {func!r} needs a str 'by' column: {spec!r}")
        if len(spec) >= 7:
            if spec[6] not in ("rows", "range"):
                raise ValueError(f"window: frame mode must be 'rows' or 'range': {spec!r}")
            range_frame = spec[6] == "range"
        value, pre, post, name = spec[1], _window_bound(spec[2]), _window_bound(spec[3]), spec[4]
        offset = int(spec[5]) if len(spec) > 5 else 0
    elif func == "ntile":
        _window_arity(spec, 3)
        offset, name = int(spec[1]), spec[2]
    elif func == "nth_value":
        _window_arity(spec, 4)
        value, offset, name = spec[1], int(spec[2]), spec[3]
    else:
        raise ValueError(f"window: unknown function {func!r}")
    if not isinstance(name, str):
        raise ValueError(f"window: output name must be a str: {spec!r}")
    return (func, value, offset, name, time, threshold, counter, pre, post, end, span, range_frame)
