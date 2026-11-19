# SPDX-License-Identifier: MIT
"""worker_share and apply_worker_share: several worker processes split a host."""

from __future__ import annotations

import multiprocessing
import os
from concurrent.futures import ProcessPoolExecutor

import pytest

import dftracer.utils as dft
from dftracer.utils.dftracer_utils_ext import _process_memory_budget

HOST_CPUS = 16
HOST_BUDGET = 8 << 30


@pytest.fixture(autouse=True)
def host(monkeypatch: pytest.MonkeyPatch) -> None:
    for name in ("DFTRACER_UTILS_THREADS", "DFTRACER_UTILS_IO_THREADS"):
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("DFTRACER_UTILS_HW_CONCURRENCY", str(HOST_CPUS))
    monkeypatch.setenv("DFTRACER_UTILS_MEM_BUDGET", str(HOST_BUDGET))


def _observe(_: int) -> tuple[int, int, int]:
    rt = dft.get_default_runtime()
    return rt.threads, rt.io_threads, _process_memory_budget()


def test_share_divides_cpus_and_budget() -> None:
    share = dft.worker_share(4)
    assert (share.threads, share.io_threads) == (HOST_CPUS // 4, HOST_CPUS // 4)
    assert share.memory_budget == HOST_BUDGET // 4
    assert dict(share.env) == {
        "DFTRACER_UTILS_THREADS": str(HOST_CPUS // 4),
        "DFTRACER_UTILS_IO_THREADS": str(HOST_CPUS // 4),
        "DFTRACER_UTILS_MEM_BUDGET": str(HOST_BUDGET // 4),
    }


def test_share_has_a_floor_and_validates() -> None:
    crowded = dft.worker_share(1000)
    assert crowded.threads == 1
    assert crowded.memory_budget == 64 << 20
    with pytest.raises(ValueError):
        dft.worker_share(0)
    with pytest.raises(AttributeError):
        crowded.threads = 2  # type: ignore[misc]


@pytest.mark.skipif(os.name != "posix", reason="fork is POSIX only")
def test_forked_child_with_inherited_state_takes_the_share() -> None:
    # The parent already has a runtime and a memory budget; the fork copies the
    # budget, so only an explicit apply can change it.
    dft.get_default_runtime()
    ctx = multiprocessing.get_context("fork")
    with ProcessPoolExecutor(
        max_workers=1, mp_context=ctx, initializer=dft.apply_worker_share, initargs=(4,)
    ) as pool:
        threads, io_threads, budget = pool.submit(_observe, 0).result()
    assert (threads, io_threads) == (HOST_CPUS // 4, HOST_CPUS // 4)
    assert budget == HOST_BUDGET // 4


def test_spawned_child_takes_the_share() -> None:
    ctx = multiprocessing.get_context("spawn")
    with ProcessPoolExecutor(
        max_workers=1, mp_context=ctx, initializer=dft.apply_worker_share, initargs=(4,)
    ) as pool:
        threads, io_threads, budget = pool.submit(_observe, 0).result()
    assert (threads, io_threads) == (HOST_CPUS // 4, HOST_CPUS // 4)
    assert budget == HOST_BUDGET // 4


def _apply_after_runtime_exists(_: int) -> tuple[int, int, int]:
    dft.get_default_runtime()
    dft.apply_worker_share(8)
    return _observe(0)


def test_apply_resizes_a_runtime_that_exists() -> None:
    ctx = multiprocessing.get_context("spawn")
    with ProcessPoolExecutor(max_workers=1, mp_context=ctx) as pool:
        threads, _, budget = pool.submit(_apply_after_runtime_exists, 0).result()
    assert threads == HOST_CPUS // 8
    assert budget == HOST_BUDGET // 8


def _apply_repeatedly(_: int) -> list[int]:
    budgets = []
    for workers in (4, 4, 2, 4):
        dft.apply_worker_share(workers)
        budgets.append(_process_memory_budget())
    return budgets


def test_applying_again_gives_the_same_share() -> None:
    ctx = multiprocessing.get_context("spawn")
    with ProcessPoolExecutor(max_workers=1, mp_context=ctx) as pool:
        budgets = pool.submit(_apply_repeatedly, 0).result()
    assert budgets == [
        HOST_BUDGET // 4,
        HOST_BUDGET // 4,
        HOST_BUDGET // 2,
        HOST_BUDGET // 4,
    ]


def _threads_on_worker() -> tuple[int, int, int]:
    return _observe(0)


def _cluster(n_workers: int):
    from dask.distributed import Client, LocalCluster

    cluster = LocalCluster(
        n_workers=n_workers, threads_per_worker=1, processes=True, silence_logs=40
    )
    return cluster, Client(cluster)


def _check_split(client, expected_workers: int, io: bool = True) -> None:
    seen = client.run(_threads_on_worker)
    assert len(seen) == expected_workers
    for threads, io_threads, budget in seen.values():
        assert threads == HOST_CPUS // expected_workers
        # The I/O pool of a runtime that exists only grows.
        assert not io or io_threads == threads
        assert budget == HOST_BUDGET // expected_workers


def test_dask_workers_split_the_host() -> None:
    pytest.importorskip("distributed")
    from dftracer.utils.dask._workers import _require_workers

    cluster, client = _cluster(2)
    with cluster, client:
        _require_workers(client)
        _check_split(client, 2)
        # Another call changes nothing.
        _require_workers(client)
        _check_split(client, 2)


def test_dask_share_follows_workers_that_join() -> None:
    pytest.importorskip("distributed")
    from dftracer.utils.dask._workers import _require_workers

    cluster, client = _cluster(1)
    with cluster, client:
        _require_workers(client)
        _check_split(client, 1)
        cluster.scale(2)
        client.wait_for_workers(2)
        _require_workers(client)
        _check_split(client, 2, io=False)


def test_dask_restarted_workers_get_the_share_again() -> None:
    pytest.importorskip("distributed")
    from dftracer.utils.dask._workers import _require_workers

    cluster, client = _cluster(2)
    with cluster, client:
        _require_workers(client)
        before = set(client.scheduler_info()["workers"])
        client.restart()
        client.wait_for_workers(2)
        assert set(client.scheduler_info()["workers"]) != before
        _require_workers(client)
        _check_split(client, 2)


def test_dask_workers_in_the_callers_process_are_left_alone() -> None:
    pytest.importorskip("distributed")
    from dask.distributed import Client, LocalCluster

    from dftracer.utils.dask._workers import _require_workers

    with LocalCluster(
        n_workers=2, threads_per_worker=1, processes=False, silence_logs=40
    ) as cluster, Client(cluster) as client:
        _require_workers(client)
        assert os.environ["DFTRACER_UTILS_MEM_BUDGET"] == str(HOST_BUDGET)
        assert "DFTRACER_UTILS_THREADS" not in os.environ
