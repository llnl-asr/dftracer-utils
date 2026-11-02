# SPDX-License-Identifier: MIT
"""Time-unit selection for normalizing trace ``ts``/``dur``.

DFTracer traces declare their native time unit via a leading ``CM`` time_metric
metadata event, and different runs can emit different units (ns/us/ms/sec).
Consumers that need a fixed unit pass a target here; the reader scales from the
trace's native unit to it.
"""

from __future__ import annotations

from enum import Enum
from typing import Union


class TimeUnit(str, Enum):
    """Target unit for ``ts``/``dur`` scaling."""

    NS = "ns"
    US = "us"
    MS = "ms"
    SEC = "sec"


# Accepts a TimeUnit, its string value, or None (native, no scaling).
TimeUnitLike = Union[str, "TimeUnit", None]
