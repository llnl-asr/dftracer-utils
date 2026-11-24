# SPDX-License-Identifier: MIT
"""``IndexRecords``: the stored records of the aggregation index."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Generic, TypeVar

T = TypeVar("T")


@dataclass(frozen=True)
class IndexRecords(Generic[T]):
    """The stored records of the aggregation index, one family per field."""

    regular: T
    """The ``ph=1`` events."""

    aggregated: T
    """The aggregated records, which carry arbitrary extra-key dimensions."""

    counters: T
    """The ``ph=2`` counters, system counters included."""
