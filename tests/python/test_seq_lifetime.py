#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A generator passed where the engine takes a sequence of columns or values:
its items exist only while the call holds the sequence."""

import subprocess
import sys
import textwrap

SCRIPT = textwrap.dedent(
    """
    import dftracer.utils as d
    from dftracer.utils import dftracer_utils_ext as ext

    N = 100_000
    LOAD_X_TIMES_2_PLUS_1 = [(0, 0), (1, 2), (3, 2), (1, 1), (3, 0)]

    for _ in range(150):
        cols = (d.Series(list(range(N)))._native for _ in range(1))
        out = d.Series(ext.vec_eval(LOAD_X_TIMES_2_PLUS_1, cols)).to_list()
        assert out[-1] == (N - 1) * 2 + 1, out[-1]

        cols = (d.Series(list(range(N)))._native for _ in range(1))
        many = ext.vec_eval_many([LOAD_X_TIMES_2_PLUS_1], cols)
        assert d.Series(many[0]).to_list()[-1] == (N - 1) * 2 + 1

        words = d.Series(["w%d" % (i % 50) for i in range(1000)])._native
        values = ("".join(["w", str(i)]) for i in (3, 7, 11))
        ast = [(0, 0), (18, values)]
        got = d.Series(ext.vec_eval(ast, [words])).to_list()
        want = [("w%d" % (i % 50)) in ("w3", "w7", "w11") for i in range(1000)]
        assert got == want

        ints = d.Series(list(range(1000)))._native
        values = (i * 3 for i in range(4))
        got = d.Series(ext.vec_eval([(0, 0), (18, values)], [ints])).to_list()
        assert got == [i in (0, 3, 6, 9) for i in range(1000)]
    print("ok")
    """
)


def test_generators_of_columns_and_values_keep_their_items_alive():
    done = subprocess.run([sys.executable, "-I", "-c", SCRIPT], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr[-2000:]
    assert done.stdout.strip().endswith("ok")
