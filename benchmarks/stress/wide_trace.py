#!/usr/bin/env python3
"""Write a wide dftracer trace for the index build memory benchmark.

Each event carries a few of PATHS distinct args keys (numbers and strings),
so a file holds many distinct paths across many gzip members.

    wide_trace.py OUT.pfw.gz [--events N] [--paths N] [--member-bytes N]
"""

import argparse
import gzip
import io


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("--events", type=int, default=2_000_000)
    p.add_argument("--paths", type=int, default=4000)
    p.add_argument("--member-bytes", type=int, default=1 << 20)
    a = p.parse_args()

    ts = 1_000_000
    buf = io.StringIO()
    buf.write("[\n")
    with open(a.out, "wb") as out:
        for i in range(a.events):
            ts += 7 + i % 13
            keys = []
            for k in range(4):
                path = (i * 7 + k * 131) % a.paths
                value = str(i % 50) if path % 2 else f'"v{i % 11}"'
                keys.append(f'"k{path}":{value}')
            buf.write(
                f'{{"id":{i},"name":"op{(i // 50) % 7}","cat":"C{i % 3}",'
                f'"pid":{1 + i % 4},"tid":{10 + i % 4},"ph":"X","ts":{ts},'
                f'"dur":{5 + i % 97},"args":{{{",".join(keys)}}}}}\n'
            )
            if buf.tell() >= a.member_bytes:
                out.write(gzip.compress(buf.getvalue().encode()))
                buf = io.StringIO()
        buf.write("]\n")
        out.write(gzip.compress(buf.getvalue().encode()))


if __name__ == "__main__":
    main()
