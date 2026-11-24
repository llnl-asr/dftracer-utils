# SPDX-License-Identifier: MIT
"""Whether a workload fits in the memory of this process."""

from __future__ import annotations

from dataclasses import dataclass

from .dftracer_utils_ext import _memory_budget_advice

__all__ = ["MemoryBudgetAdvice", "memory_budget_advice"]


@dataclass(frozen=True)
class MemoryBudgetAdvice:
    """The answer of :func:`memory_budget_advice`."""

    fits: bool
    """Whether ``peak_bytes`` is within ``available_bytes``."""
    required_bytes: int
    """The footprint that was asked about."""
    peak_bytes: int
    """The expected peak: ``required_bytes`` times the peak factor."""
    available_bytes: int
    """The memory the peak was judged against."""
    suggested_nodes: int
    """How many nodes spread the peak under ``available_bytes`` (1 when it fits)."""
    warning: str
    """A message that says how much memory or how many nodes are needed; empty when it fits."""


def memory_budget_advice(required_bytes: int, available_bytes: int = 0) -> MemoryBudgetAdvice:
    """Whether an aggregated workload of ``required_bytes`` fits in this process.

    ``available_bytes=0`` detects the available memory, counting the cgroup limit.

    Raises:
        ValueError: a byte count is negative.
    """
    if required_bytes < 0 or available_bytes < 0:
        raise ValueError(f"byte counts must not be negative: {required_bytes}, {available_bytes}")
    return MemoryBudgetAdvice(*_memory_budget_advice(required_bytes, available_bytes))
