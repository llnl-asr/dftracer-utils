# SPDX-License-Identifier: MIT
"""Fail fast when a Dask cluster has no worker, and give each worker its share."""

from __future__ import annotations

import os
import socket
import weakref
from collections import defaultdict
from typing import TYPE_CHECKING

from .._workers import apply_worker_share

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
    ``nthreads()`` is not empty. Once workers are there, each takes its share of its
    host (see :func:`_share_worker_processes`).
    """
    _wait_for_workers(client)
    _share_worker_processes(client)


def _wait_for_workers(client: Client) -> None:
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


class _Shared:
    """What one client has already shared out: the pid of each worker address, and
    the worker addresses a host's share was last applied to."""

    def __init__(self) -> None:
        self.pids: dict[str, int] = {}
        self.applied: dict[str, frozenset[str]] = {}


# One entry per client; a closed client is forgotten.
_shared: weakref.WeakKeyDictionary[Client, _Shared] = weakref.WeakKeyDictionary()


def _host_of(address: str, info: dict[str, object]) -> str:
    host = info.get("host")
    return str(host) if host else address.split("://")[-1].rsplit(":", 1)[0]


def _is_this_host(host: str) -> bool:
    if host in ("127.0.0.1", "localhost", "::1", "0.0.0.0"):
        return True
    try:
        name = socket.gethostname()
        return host == name or host in socket.gethostbyname_ex(name)[2]
    except OSError:
        return False


def _share_worker_processes(client: Client) -> None:
    """Give each worker process its share of the host it runs on.

    Worker processes are counted per host from ``scheduler_info()``, and each runs
    :func:`~dftracer.utils.runtime.apply_worker_share` with its host's count, so ``n``
    worker processes on one host do not each size to the whole host. Each call
    compares the workers the scheduler lists with the ones already shared out
    (a local comparison, and one ``os.getpid`` call for an address not seen
    before): when the worker processes of a host changed, because workers joined,
    left or were restarted, every worker process of that host gets the share of
    the new count. A worker that lives in this process (a cluster of threads)
    is not a worker process, so nothing is split or applied there. A client
    without a scheduler (an in-process ``NullClient``) passes through.
    """
    scheduler_info = getattr(client, "scheduler_info", None)
    run = getattr(client, "run", None)
    if scheduler_info is None or run is None:
        return
    state = _shared.setdefault(client, _Shared())
    workers = scheduler_info().get("workers") or {}
    state.pids = {a: p for a, p in state.pids.items() if a in workers}
    unseen = [a for a in workers if a not in state.pids]
    if unseen:
        state.pids.update(run(os.getpid, workers=unseen))
    by_host: dict[str, list[str]] = defaultdict(list)
    for address, info in workers.items():
        host = _host_of(address, info)
        in_this_process = address.startswith("inproc://") or _is_this_host(host)
        if in_this_process and state.pids[address] == os.getpid():
            continue
        by_host[host].append(address)
    state.applied = {h: a for h, a in state.applied.items() if h in by_host}
    for host, addresses in by_host.items():
        current = frozenset(addresses)
        if state.applied.get(host) == current:
            continue
        processes = len({state.pids[a] for a in addresses})
        run(apply_worker_share, processes, workers=addresses)
        state.applied[host] = current
