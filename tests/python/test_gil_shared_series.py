#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Eight threads run kernels on one shared Series and DataFrame with the GIL
released, and every result equals the single-thread result."""

import threading

import numpy as np

import dftracer.utils as dft

ROWS = 1_000_000
THREADS = 8
ROUNDS = 50


def _tasks(s, words, df):
    return {
        "abs": lambda: s.abs().to_numpy().tobytes(),
        "sum": lambda: s.sum(),
        "sort": lambda: s.sort().to_numpy().tobytes(),
        "filter": lambda: s.filter(s > 0).to_numpy().tobytes(),
        "str_upper": lambda: words.str.upper().to_list(),
        "str_contains": lambda: words.str.contains("7").to_numpy().tobytes(),
        "group_by": lambda: sorted(
            df.group_by("k").agg("sum:v").to_pandas().itertuples(index=False)
        ),
    }


def test_threads_share_one_series_and_dataframe():
    rng = np.random.default_rng(7)
    values = rng.integers(-1000, 1000, ROWS).astype(np.int64)
    s = dft.Series(values)
    words = dft.Series([str(i % 997) for i in range(ROWS)])
    df = dft.DataFrame({"k": values % 16, "v": values})
    tasks = _tasks(s, words, df)
    expected = {name: fn() for name, fn in tasks.items()}
    errors = []

    def worker(index):
        names = list(tasks)
        try:
            for round_ in range(ROUNDS):
                name = names[(index + round_) % len(names)]
                if tasks[name]() != expected[name]:
                    errors.append((index, round_, name))
        except BaseException as exc:  # noqa: BLE001
            errors.append((index, repr(exc)))

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(THREADS)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert errors == []
