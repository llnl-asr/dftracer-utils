# SPDX-License-Identifier: MIT
"""Fail fast when a Dask cluster has no worker."""

from __future__ import annotations

import os
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from dask.distributed import Client

_TIMEOUT_ENV = "DFTRACER_UTILS_DASK_WORKER_TIMEOUT"
_DEFAULT_TIMEOUT = 60.0


def _worker_timeout() -> float:
    raw = os.environ.get(_TIMEOUT_ENV)
    if raw is None or not raw.strip():
        return _DEFAULT_TIMEOUT
    try:
        value = float(raw)
    except ValueError:
        return _DEFAULT_TIMEOUT
    return value if value >= 0 else _DEFAULT_TIMEOUT


def _require_workers(client: Client) -> None:
    """Raise ``TimeoutError`` when ``client`` has no worker within the timeout.

    A timeout of 0 does not wait. A client without ``wait_for_workers`` passes when
    ``nthreads()`` is not empty.
    """
    if client.nthreads():
        return
    timeout = _worker_timeout()
    wait = getattr(client, "wait_for_workers", None)
    if wait is not None and timeout > 0:
        try:
            wait(1, timeout=timeout)
            return
        except TimeoutError:
            pass
    scheduler = getattr(getattr(client, "scheduler", None), "address", None)
    raise TimeoutError(
        f"no Dask worker joined the scheduler at {scheduler} within {timeout:g} s; "
        f"start a worker or raise {_TIMEOUT_ENV}"
    )
