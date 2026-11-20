# SPDX-License-Identifier: MIT
"""Sizing the runtime of one worker process out of several on a host."""

from __future__ import annotations

import os
from dataclasses import dataclass
from types import MappingProxyType
from typing import Mapping

from .dftracer_utils_ext import _apply_worker_share, _worker_share

__all__ = ["WorkerShare", "apply_worker_share", "worker_share"]


@dataclass(frozen=True)
class WorkerShare:
    """What one of several worker processes on a host may use."""

    threads: int
    """Compute threads: the usable CPUs divided by the worker count, at least 1."""
    io_threads: int
    """I/O threads, equal to ``threads``."""
    memory_budget: int
    """Bytes of memory budget: this process's budget divided by the worker count."""
    env: Mapping[str, str]
    """``DFTRACER_UTILS_THREADS``, ``DFTRACER_UTILS_IO_THREADS`` and
    ``DFTRACER_UTILS_MEM_BUDGET`` with these values, for a launcher that sets the
    environment of its workers."""


def worker_share(workers: int) -> WorkerShare:
    """The share of one of ``workers`` processes that split this host.

    The usable CPUs count the CPU affinity mask and the cgroup CPU quota. The
    memory budget is the one this process would use (``DFTRACER_UTILS_MEM_BUDGET``
    when set, else a third of the available memory) divided by ``workers``.
    """
    return _share(workers, pin=False)


def _share(workers: int, pin: bool) -> WorkerShare:
    if workers < 1:
        raise ValueError(f"workers must be at least 1, not {workers}")
    threads, io_threads, memory_budget = _worker_share(workers, pin)
    env = {
        "DFTRACER_UTILS_THREADS": str(threads),
        "DFTRACER_UTILS_IO_THREADS": str(io_threads),
        "DFTRACER_UTILS_MEM_BUDGET": str(memory_budget),
    }
    return WorkerShare(threads, io_threads, memory_budget, MappingProxyType(env))


def apply_worker_share(workers: int) -> None:
    """Make this process take its share of a host split among ``workers``.

    Use it as the initializer of a ``ProcessPoolExecutor`` or a
    ``multiprocessing.Pool``. It sets the environment variables of
    :func:`worker_share` and applies them in place: the memory budget at once,
    and the thread counts of the process runtime when it already exists (the
    I/O pool only grows). A runtime made later reads the variables. Applying
    again gives the same share for the same ``workers``, and the share of the
    new count for another, always out of the budget this process had before the
    first apply.
    """
    share = _share(workers, pin=True)
    os.environ.update(share.env)
    _apply_worker_share(share.threads, share.io_threads, share.memory_budget)
