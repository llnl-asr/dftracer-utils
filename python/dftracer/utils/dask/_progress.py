# SPDX-License-Identifier: MIT
"""Progress of Dask workers, published to the coordinator."""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Callable,
    TypedDict,
)

if TYPE_CHECKING:
    from dask.distributed import Client


# All worker-side progress (parse, scan, ...) is published to one topic with a
# `phase` label; the coordinator aggregates per (phase, batch).
_PROGRESS_TOPIC = "dft-progress"


class _ProgressMsg(TypedDict):
    """The per-event payload published to _PROGRESS_TOPIC."""

    phase: str
    batch: str
    done: int
    total: int


def worker_progress_forwarder(phase: str, batch: str) -> Callable[[int, int], None]:
    """Callback that publishes (done, total) progress for `phase`/`batch` to the
    coordinator. Runs on a Dask worker; no-op off-worker (inline mode)."""
    try:
        from distributed import get_worker

        worker = get_worker()
    except (ImportError, ValueError):
        worker = None

    def _cb(done: int, total: int) -> None:
        if worker is None:
            return
        msg: _ProgressMsg = {
            "phase": phase,
            "batch": batch,
            "done": int(done),
            "total": int(total),
        }
        # Called from a C++ worker thread; hop to the IOLoop so the event is
        # sent on the worker's own loop rather than a foreign thread.
        try:
            worker.loop.add_callback(worker.log_event, _PROGRESS_TOPIC, msg)
        except Exception:
            try:
                worker.log_event(_PROGRESS_TOPIC, msg)
            except Exception:
                pass

    return _cb


class ProgressAggregator:
    """Subscribe to the progress topic and forward aggregated
    (done, total, phase) to `callback`, summing per (phase, batch)."""

    def __init__(self, client: Client, callback: Callable[[int, int, str], None] | None) -> None:
        self._client = client
        self._callback = callback
        self._state: dict[tuple[str, str], tuple[int, int]] = {}

    # event is a distributed topic payload: (timestamp, msg).
    def _handler(self, event: tuple[float, _ProgressMsg]) -> None:
        if self._callback is None:
            return
        try:
            _, msg = event
            phase = msg["phase"]
            self._state[(phase, msg["batch"])] = (int(msg["done"]), int(msg["total"]))
            done = sum(d for (p, _), (d, _t) in self._state.items() if p == phase)
            total = sum(t for (p, _), (_d, t) in self._state.items() if p == phase)
            self._callback(done, total, phase)
        except Exception:
            pass

    def __enter__(self) -> ProgressAggregator:
        # NullClient (in-process) has no pub/sub topic; skip quietly.
        if self._callback is not None and hasattr(self._client, "subscribe_topic"):
            self._client.subscribe_topic(_PROGRESS_TOPIC, self._handler)
        return self

    def __exit__(self, *exc: object) -> None:
        if self._callback is not None and hasattr(self._client, "unsubscribe_topic"):
            try:
                self._client.unsubscribe_topic(_PROGRESS_TOPIC)
            except Exception:
                pass
