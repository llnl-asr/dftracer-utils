# SPDX-License-Identifier: MIT
"""Write the trace that the JIT tutorial reads: 300 events on two processes."""

import gzip
import os
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
with gzip.open(os.path.join(out, "trace.pfw.gz"), "wt", encoding="utf-8") as f:
    for i in range(300):
        pid = 2 if i % 3 == 0 else 1
        f.write(
            f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
            f'"ts":{1000 + i},"dur":{10 + i % 50},"ph":"X","args":{{}}}}\n'
        )
