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


def _run(fn, threads, reps):
    workers = [threading.Thread(target=lambda: [fn() for _ in range(reps)]) for _ in range(threads)]
    start = time.perf_counter()
    for w in workers:
        w.start()
    for w in workers:
        w.join()
    return time.perf_counter() - start


@pytest.mark.skipif((os.cpu_count() or 1) < 2, reason="needs two cores")
@pytest.mark.skipif(
    bool(os.environ.get("DFTRACER_UTILS_VALGRIND")),
    reason="valgrind runs one thread at a time, so two threads cannot beat one",
)
def test_two_threads_run_sum_in_parallel():
    s = dft.Series(np.arange(ROWS, dtype=np.int64))
    expected = int(np.arange(ROWS, dtype=np.int64).sum())
    assert s.sum() == expected
    t0 = time.perf_counter()
    s.sum()
    reps = max(10, int(0.3 / max(time.perf_counter() - t0, 1e-6)))
    one = min(_run(s.sum, 1, reps) for _ in range(3))
    two = min(_run(s.sum, 2, reps) for _ in range(3))
    assert two <= 1.5 * one, f"one thread {one:.3f}s, two threads {two:.3f}s"
