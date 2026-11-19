# SPDX-License-Identifier: MIT
"""A Dask cluster with no worker fails fast instead of waiting forever."""

import gzip
import threading
import time

import pytest

pytest.importorskip("distributed")

from dftracer.utils import DataFrame  # noqa: E402
from dftracer.utils.dask import DaskFrame, distributed_index  # noqa: E402

LIMIT_S = 30


@pytest.fixture(scope="module")
def empty_client():
    from dask.distributed import Client, LocalCluster

    cluster = LocalCluster(n_workers=0, processes=False, silence_logs=40)
    client = Client(cluster)
    yield client
    client.close()
    cluster.close()


def _raises_within_limit(fn):
    outcome = []

    def run():
        try:
            fn()
            outcome.append(None)
        except BaseException as exc:  # noqa: BLE001
            outcome.append(exc)

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    thread.join(LIMIT_S)
    assert not thread.is_alive(), "the call is still waiting for a worker"
    return outcome[0]


def test_distributed_index_without_workers_times_out(empty_client, tmp_path, monkeypatch):
    monkeypatch.setenv("DFTRACER_UTILS_DASK_WORKER_TIMEOUT", "1")
    trace = tmp_path / "t.pfw.gz"
    with gzip.open(trace, "wt") as f:
        f.write('[\n{"id":1,"name":"a","cat":"c","pid":1,"tid":1,"ts":1,"dur":1,"ph":"X"}\n')
    exc = _raises_within_limit(
        lambda: distributed_index(
            [str(trace)], index_dir=str(tmp_path / "idx.dftindex"), client=empty_client
        )
    )
    assert isinstance(exc, TimeoutError)
    assert "DFTRACER_UTILS_DASK_WORKER_TIMEOUT" in str(exc)


def test_daskframe_without_workers_times_out(empty_client, monkeypatch):
    monkeypatch.setenv("DFTRACER_UTILS_DASK_WORKER_TIMEOUT", "1")
    frame = DataFrame.from_dict({"a": [1, 2, 3]})
    exc = _raises_within_limit(lambda: DaskFrame.from_frames([frame], empty_client))
    assert isinstance(exc, TimeoutError)


class _NoWaitClient:
    def __init__(self, threads):
        self._threads = threads

    def nthreads(self):
        return self._threads


def test_client_without_wait_for_workers():
    from dftracer.utils.dask._workers import _require_workers

    _require_workers(_NoWaitClient({"w": 1}))
    with pytest.raises(TimeoutError):
        _require_workers(_NoWaitClient({}))


def test_zero_timeout_raises_at_once(empty_client, monkeypatch):
    monkeypatch.setenv("DFTRACER_UTILS_DASK_WORKER_TIMEOUT", "0")
    frame = DataFrame.from_dict({"a": [1, 2, 3]})
    start = time.monotonic()
    exc = _raises_within_limit(lambda: DaskFrame.from_frames([frame], empty_client))
    assert isinstance(exc, TimeoutError)
    assert time.monotonic() - start < 5


@pytest.mark.parametrize("raw", ["-1", "junk"])
def test_bad_timeout_uses_the_default(raw, monkeypatch):
    from dftracer.utils.dask._workers import _worker_timeout

    monkeypatch.setenv("DFTRACER_UTILS_DASK_WORKER_TIMEOUT", raw)
    assert _worker_timeout() == 60.0
