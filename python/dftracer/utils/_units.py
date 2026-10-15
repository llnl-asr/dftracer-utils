"""Human-readable byte and duration parsing for user-facing arguments.

Mirrors the C++ helpers in ``common/str_format.h`` so a Python caller can
pass either a raw number (in the argument's native unit) or a string such as
``"64MB"``, ``"1.5GiB"``, ``"30s"`` or ``"5m"``.

Byte units are always 1024-based, so ``KB`` and ``KiB`` are the same. A trailing
``B`` is bytes and ``b`` is bits (divided by 8); an ``i`` before it is accepted
and ignored. The magnitude prefix (k/m/g/t/p) is case-insensitive.
"""

from __future__ import annotations

import re
from typing import Optional, Union

Bytes = Union[int, str]
Duration = Union[int, float, str]

_NUM = re.compile(r"^\s*([+-]?(?:\d+\.?\d*|\.\d+))\s*([a-zA-Z]*)\s*$")

_BYTE_POWER = {"": 0, "k": 1, "m": 2, "g": 3, "t": 4, "p": 5}

_DURATION_SECONDS = {
    "": 1.0,
    "s": 1.0,
    "sec": 1.0,
    "secs": 1.0,
    "ns": 1e-9,
    "us": 1e-6,
    "ms": 1e-3,
    "m": 60.0,
    "min": 60.0,
    "mins": 60.0,
    "h": 3600.0,
    "hr": 3600.0,
    "hrs": 3600.0,
    "d": 86400.0,
    "day": 86400.0,
    "days": 86400.0,
}


def parse_bytes(text: str) -> Optional[int]:
    """Parse a size such as ``"512"``, ``"64KB"``, ``"1.5GiB"``, ``"8kb"`` into
    a byte count, or return None on malformed input."""
    match = _NUM.match(text)
    if not match:
        return None
    value = float(match.group(1))
    unit = match.group(2)
    if value < 0:
        return None
    bits = False
    if unit[-1:] == "B":
        unit = unit[:-1]
    elif unit[-1:] == "b":
        bits = True
        unit = unit[:-1]
    if unit[-1:] in ("i", "I"):
        unit = unit[:-1]
    power = _BYTE_POWER.get(unit.lower())
    if power is None:
        return None
    result = value * (1024**power)
    if bits:
        result /= 8.0
    return int(result)


def parse_duration_seconds(text: str) -> Optional[float]:
    """Parse a duration such as ``"30"``, ``"500ms"``, ``"1.5h"`` into seconds
    (a bare number is seconds), or return None on malformed input."""
    match = _NUM.match(text)
    if not match:
        return None
    value = float(match.group(1))
    if value < 0:
        return None
    factor = _DURATION_SECONDS.get(match.group(2).lower())
    if factor is None:
        return None
    return value * factor


def coerce_bytes(value: Bytes, name: str = "value") -> int:
    """Return a byte count from an int (already bytes) or a unit string.

    Raises ValueError on a malformed string.
    """
    if isinstance(value, str):
        parsed = parse_bytes(value)
        if parsed is None:
            raise ValueError(
                f"Invalid byte size for {name}: {value!r} (use e.g. 65536, 64KB, 1.5GB)"
            )
        return parsed
    return int(value)


def coerce_duration(value: Duration, native_per_second: float, name: str = "value") -> float:
    """Return a duration in the caller's native unit from a number (already in
    that unit) or a unit string (converted from seconds).

    ``native_per_second`` is how many native units are in one second, e.g. 1e3
    for milliseconds. Raises ValueError on a malformed string.
    """
    if isinstance(value, str):
        seconds = parse_duration_seconds(value)
        if seconds is None:
            raise ValueError(f"Invalid duration for {name}: {value!r} (use e.g. 30, 30s, 5m, 1.5h)")
        return seconds * native_per_second
    return float(value)
