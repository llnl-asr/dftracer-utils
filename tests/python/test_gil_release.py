#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A long Series kernel runs without the GIL, so two Python threads run two
kernels at once."""

import os
import threading
import time

import numpy as np
import pytest

import dftracer.utils as dft

# 1 MiB stays in cache, so two threads do not share memory bandwidth.
ROWS = 131_072
LIMIT = 1.5


def _run(fn, threads, reps):
    workers = [threading.Thread(target=lambda: [fn() for _ in range(reps)]) for _ in range(threads)]
    start = time.perf_counter()
    for w in workers:
        w.start()
    for w in workers:
        w.join()
    return time.perf_counter() - start


def _min_times(fn, reps, rounds=10):
    min_one = min_two = None
    for done in range(1, rounds + 1):
        one = _run(fn, 1, reps)
        two = _run(fn, 2, reps)
        min_one = one if min_one is None else min(min_one, one)
        min_two = two if min_two is None else min(min_two, two)
        if min_two <= LIMIT * min_one:
            break
    return min_one, min_two, done


@pytest.mark.skipif((os.cpu_count() or 1) < 2, reason="needs two cores")
@pytest.mark.skipif(
    bool(os.environ.get("DFTRACER_UTILS_VALGRIND")),
    reason="valgrind runs one thread at a time, so two threads cannot beat one",
)
@pytest.mark.skipif(
    bool(os.environ.get("DFTRACER_UTILS_COVERAGE")),
    reason="coverage counters are shared memory written on every loop step, so two threads contend",
)
def test_two_threads_run_sum_in_parallel():
    s = dft.Series(np.arange(ROWS, dtype=np.int64))
    expected = int(np.arange(ROWS, dtype=np.int64).sum())
    assert s.sum() == expected
    t0 = time.perf_counter()
    s.sum()
    reps = max(10, int(0.3 / max(time.perf_counter() - t0, 1e-6)))
    min_one, min_two, rounds = _min_times(s.sum, reps)
    assert min_two <= LIMIT * min_one, (
        f"min one thread {min_one:.3f}s, min two threads {min_two:.3f}s, "
        f"ratio {min_two / min_one:.2f} over {rounds} rounds"
    )
