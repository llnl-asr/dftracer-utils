#!/usr/bin/env python3
"""Concurrent load driver for the dftracer server viz endpoints.

Models a dashboard / multi-user session: N workers fire random overlapping
time windows at the viz endpoints and we report latency percentiles and
throughput. Overlapping windows are the point - they make concurrent queries
touch the same gzip members, which is what the member decode cache coalesces.

Stdlib only. Not a unit test; run manually against a live server.

  load_driver.py --host 127.0.0.1 --port 8080 --duration 30 --concurrency 32 \
      --mix all
"""

import argparse
import json
import random
import sys
import threading
import time
import urllib.request

ENDPOINTS = {
    "density": "/api/v1/viz/density?begin={b}&end={e}&summary=1&width=4096",
    "calltree": "/api/v1/viz/calltree?begin={b}&end={e}&summary=1",
    "events": "/api/v1/viz/events?begin={b}&end={e}&summary=1&limit=5000",
    "stats": "/api/v1/viz/stats?begin={b}&end={e}&summary=1",
}

# Zoom levels a dashboard user cycles through (fraction of the full span).
ZOOMS = [1.0, 0.5, 0.25, 0.1, 0.02, 0.005]


def http_get(base, path, timeout):
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(base + path, timeout=timeout) as r:
            body = r.read()
            ok = r.status == 200
        return ok, len(body), time.perf_counter() - t0
    except Exception:
        return False, 0, time.perf_counter() - t0


def get_span(base, timeout):
    ok, _, _ = http_get(base, "/api/v1/info", timeout)
    if not ok:
        return None
    with urllib.request.urlopen(base + "/api/v1/info", timeout=timeout) as r:
        info = json.loads(r.read())
    tr = info.get("time_range", {})
    lo = tr.get("min_timestamp_us", info.get("global_min_timestamp_us", 0))
    hi = tr.get("max_timestamp_us", info.get("global_max_timestamp_us", 0))
    return max(0, hi - lo)


def worker(base, span, mix, zooms, hotspot, deadline, timeout, rng, out,
           endpoints):
    band = max(1, int(span * hotspot))
    while time.perf_counter() < deadline:
        name = rng.choice(mix)
        zoom = rng.choice(zooms)
        window = max(1, int(span * zoom))
        # Cluster window starts into a hot band so concurrent queries overlap
        # on the same members (what the member cache coalesces).
        hi = max(0, min(band, span - window))
        begin = rng.randint(0, hi) if hi > 0 else 0
        end = begin + window
        ok, nbytes, dt = http_get(base, endpoints[name].format(b=begin, e=end),
                                  timeout)
        out.append((name, ok, nbytes, dt))


def pct(xs, p):
    if not xs:
        return 0.0
    xs = sorted(xs)
    k = min(len(xs) - 1, int(round((p / 100.0) * (len(xs) - 1))))
    return xs[k]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--duration", type=float, default=30.0)
    ap.add_argument("--concurrency", type=int, default=32)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--mix", default="all",
                    help="comma list of density,calltree,events,stats or 'all'")
    ap.add_argument("--max-zoom", type=float, default=1.0,
                    help="largest window as a fraction of span (e.g. 0.02 for "
                         "a realistic zoomed-in dashboard; 1.0 = full span)")
    ap.add_argument("--hotspot", type=float, default=1.0,
                    help="restrict window starts to the first fraction of the "
                         "span so concurrent queries overlap (1.0 = anywhere)")
    ap.add_argument("--nofast", action="store_true",
                    help="append &nofast=1 so /viz/stats forces the full live "
                         "decode instead of the stored-aggregate fast path")
    args = ap.parse_args()

    zooms = [z for z in ZOOMS if z <= args.max_zoom] or [args.max_zoom]

    endpoints = dict(ENDPOINTS)
    if args.nofast:
        endpoints["stats"] = endpoints["stats"] + "&nofast=1"

    base = f"http://{args.host}:{args.port}"
    mix = (list(ENDPOINTS) if args.mix == "all"
           else [m.strip() for m in args.mix.split(",") if m.strip()])
    for m in mix:
        if m not in ENDPOINTS:
            print(f"unknown endpoint: {m}", file=sys.stderr)
            return 2

    span = get_span(base, args.timeout)
    if not span:
        print("could not read /api/v1/info span; is the server up?",
              file=sys.stderr)
        return 1
    print(f"span={span} us  concurrency={args.concurrency}  "
          f"duration={args.duration}s  mix={mix}")

    results = []
    lock = threading.Lock()
    threads = []
    deadline = time.perf_counter() + args.duration
    t0 = time.perf_counter()
    for i in range(args.concurrency):
        buf = []
        results.append(buf)
        rng = random.Random(args.seed + i)
        t = threading.Thread(target=worker,
                             args=(base, span, mix, zooms, args.hotspot,
                                   deadline, args.timeout, rng, buf,
                                   endpoints))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    wall = time.perf_counter() - t0

    flat = [r for buf in results for r in buf]
    total = len(flat)
    errors = sum(1 for _, ok, _, _ in flat if not ok)
    print(f"\ntotal={total} requests  errors={errors}  "
          f"throughput={total / wall:.1f} req/s  wall={wall:.1f}s")
    print(f"{'endpoint':<10} {'count':>7} {'p50 ms':>9} {'p95 ms':>9} "
          f"{'p99 ms':>9} {'max ms':>9} {'errs':>6}")
    for name in mix:
        lat = [dt * 1000 for n, ok, _, dt in flat if n == name and ok]
        errs = sum(1 for n, ok, _, _ in flat if n == name and not ok)
        cnt = sum(1 for n, _, _, _ in flat if n == name)
        print(f"{name:<10} {cnt:>7} {pct(lat, 50):>9.1f} {pct(lat, 95):>9.1f} "
              f"{pct(lat, 99):>9.1f} {(max(lat) if lat else 0):>9.1f} "
              f"{errs:>6}")
    all_lat = [dt * 1000 for _, ok, _, dt in flat if ok]
    print(f"{'ALL':<10} {total:>7} {pct(all_lat, 50):>9.1f} "
          f"{pct(all_lat, 95):>9.1f} {pct(all_lat, 99):>9.1f} "
          f"{(max(all_lat) if all_lat else 0):>9.1f} {errors:>6}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
