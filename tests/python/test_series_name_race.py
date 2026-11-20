# SPDX-License-Identifier: MIT
"""Renaming a Series in place while other threads run kernels on it.

A kernel over a long column runs with the GIL released and copies the column
name into its result; the setter must never free a name such a kernel reads.
The race ends the process, so it runs in a subprocess.
"""

import subprocess
import sys
import textwrap

_CODE = textwrap.dedent(
    """
    import threading
    from dftracer.utils.series import Series

    n = 400000
    s = Series(["abc%d" % i for i in range(n)])
    s.name = "start"
    ints = Series(list(range(n)), name="i")
    stop = False
    seen = []

    def reader():
        while not stop:
            s.str_len_bytes().name
            s.to_uppercase().name
            (s == "abc1").name
            ints.abs().name
            ints.cumsum().name

    threads = [threading.Thread(target=reader) for _ in range(3)]
    for t in threads:
        t.start()
    for i in range(300000):
        s.name = "n%d" % (i % 7)
        ints.name = "m%d" % (i % 5)
    stop = True
    for t in threads:
        t.join()
    assert s.str_len_bytes().name in {"n%d" % k for k in range(7)}
    print("ok")
    """
)


def test_rename_while_kernels_run_on_other_threads():
    for _ in range(5):
        out = subprocess.run(
            [sys.executable, "-I", "-c", _CODE], capture_output=True, text=True, timeout=300
        )
        assert out.returncode == 0 and "ok" in out.stdout, (out.returncode, out.stderr[-500:])
