# SPDX-License-Identifier: MIT
"""Rebinding a frame in place while other threads run kernels on it.

`_replace` swaps the columns of a DataFrame or the plan of a LazyFrame in
place (`df["c"] = ...`, `df.pop`, `lf.loc[...] = ...`). A kernel over a frame of
16384 rows or more runs with the GIL released, so it must read a copy of the
frame taken under the GIL. Reader threads call the cheapest such kernel in a
tight loop, so that they enter the window between releasing the GIL and reading
the frame as often as possible. A race ends the process, so it runs in a
subprocess.
"""

import subprocess
import sys
import textwrap

SECONDS = 3
READERS = 12

_CODE = textwrap.dedent(
    """
    import threading
    import time
    from dftracer.utils import DataFrame
    from dftracer.utils import dftracer_utils_ext as ext

    n = 16400
    one = DataFrame({"a": list(range(n)), "s": ["abc%d" % i for i in range(n)]})
    two = DataFrame({"a": list(range(n)), "t": ["xyz%d" % i for i in range(n)], "u": list(range(n))})
    df = one.copy()
    lf = one.lazy()
    lf_two = two.lazy()
    stop = False

    def frame_reader():
        while not stop:
            df.head(1)

    def plan_reader():
        while not stop:
            lf.collect()

    threads = [threading.Thread(target=frame_reader) for _ in range(READERS - 1)]
    threads.append(threading.Thread(target=plan_reader))
    for t in threads:
        t.start()
    swaps = 0
    end = time.time() + SECONDS
    while time.time() < end:
        ext.DataFrame._replace(df, two if swaps % 2 else one)
        ext.LazyFrame._replace(lf, lf_two if swaps % 2 else one.lazy())
        swaps += 1
    stop = True
    for t in threads:
        t.join()
    assert df.num_rows == n and swaps > 0
    print("ok")
    """
)


def test_rebinding_while_kernels_run_on_other_threads():
    code = _CODE.replace("READERS", str(READERS)).replace("SECONDS", str(SECONDS))
    for _ in range(2):
        out = subprocess.run(
            [sys.executable, "-I", "-c", code], capture_output=True, text=True, timeout=120
        )
        assert out.returncode == 0 and "ok" in out.stdout, (out.returncode, out.stderr[-500:])
