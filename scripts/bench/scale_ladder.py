"""Run dftracer-utils CLI tools on large trace sets and write one TSV row per run.

Run it with: uv run --no-project --python '>=3.8' scripts/bench/scale_ladder.py ...

  scale_ladder.py --build build/build-tests --set big=/data/traces \
      --scratch /scratch/ladder --out ladder.tsv --threads 1,2,4,8
  scale_ladder.py --check ladder.tsv

Each run is one tool process with a fresh index dir and output dir under
--scratch. Scratch dirs are never deleted. The script refuses to start when the
scratch dir is inside a set.

Each run reads its set through a fresh dir of symlinks in the run dir (in/),
one link per trace file, named by its path in the set with "/" replaced by
"__". Tools that do not walk subdirs then see every file of a nested set, and
files a tool writes next to its inputs land in the run dir. After each run the
script stops with exit 2 when a file or dir owned by this user in the set is
newer than the run start.

Tools: index event_count info stats view split validate pgzip comparator replay.
  - A single-file set is passed with --files where the tool has it (info,
    stats, view, validate) and as a path to comparator and replay. index,
    event_count and split only take -d and fail on a file, so they get the
    link dir.
  - view runs with --group-by name, which reads every event.
  - replay runs with --dry-run --dftracer-mode --no-sleep: it parses and
    analyzes the events and does no I/O on the trace paths. --dry-run alone
    makes the posix executor reject events of other categories (exit 2).
  - comparator compares the set with itself, with two index dirs.
  - pgzip compresses .pfw files in place and removes the source, so it runs on
    a staging copy: up to --pgzip-max-mib MiB (default 256) of uncompressed
    .pfw data made from the first trace files of the set (.pfw.gz files are
    decompressed, .pfw files copied) in the run dir, always at least one file.
    The size is in the note column. A set with no .pfw or .pfw.gz file has no
    pgzip row.

Before the first timed run of each set, one untimed pass reads every input
file once (--warm-threads reader threads) so the first tool and thread count
do not pay cold reads. Its wall time goes to stderr and to the note column of
the first row of the set. --no-warm skips it.

Every tool runs with DFTRACER_UTILS_MEMORY_REPORT=1; heap_peak and peak come
from the "dftracer-utils memory:" line on its stderr. Threads: --executor-threads
N and DFTRACER_UTILS_THREADS=N. Without --threads the tool default is used and
the threads column says "default".

Measurement: the script runs itself as a helper (--exec-measure) per tool run.
The helper spawns the tool, takes wall time with time.monotonic and reaps it
with wait4, which gives rusage for that one process. On Linux the I/O columns
are the change in the helper's own /proc/self/io across the reap, since the
kernel adds a reaped child's counts to its parent. It prints one JSON
line. ru_maxrss is stored as MiB. Tool stdout and stderr go to stdout.log and
stderr.log in the run dir.

perf: on Linux, when `perf stat` works, the helper runs under
`perf stat -x,` for instructions, cycles, cache-references and cache-misses.
The counters include the python helper itself. The helper is measured once per
invocation running /bin/true and that overhead is SUBTRACTED from the counters
of every run (clamped at 0). Without a usable perf the perf columns are blank.

speedup = wall of the smallest thread count / wall; efficiency = speedup /
(threads / smallest threads). Both use the mean wall over the repeats, per
(tool, set), within this invocation.

The run exits 0 even when a tool fails; the rc column has the exit code. Use
--check to fail on any non-zero rc. --known FILE lists failures that are
measured but not fixed yet, one per line: tool, set, threads (or *), rc and a
reason, tab separated; lines starting with # are comments. --check accepts a run
that matches an entry, and fails on an entry whose runs in the table all exit 0,
so a fix must remove its entry.
"""

import argparse
import csv
import gzip
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

TOOLS = [
    "index",
    "event_count",
    "info",
    "stats",
    "view",
    "split",
    "validate",
    "pgzip",
    "comparator",
    "replay",
]

FILES_FLAG = {"info", "stats", "view", "validate"}
PATH_ARG = {"comparator", "replay"}

COLUMNS = [
    "commit", "host", "tool", "set", "input_bytes", "input_files", "threads",
    "repeat", "rc", "wall_s", "user_s", "sys_s", "parallelism", "max_rss_mib",
    "heap_peak_mib", "peak_mib", "rchar", "wchar", "read_bytes", "write_bytes",
    "instructions", "cycles", "ipc", "cache_refs", "cache_misses", "miss_pct",
    "minflt", "majflt", "nvcsw", "nivcsw", "speedup", "efficiency", "run_dir",
    "note",
]

PERF_EVENTS = "instructions,cycles,cache-references,cache-misses,task-clock"
MEM_RE = re.compile(
    r"dftracer-utils memory: peak=(\d+) live=(\d+) measured=(\d+) "
    r"limit=(\d+) heap_peak=(\d+)"
)
MIB = 1024.0 * 1024.0


def decode_status(status):
    if os.WIFEXITED(status):
        return os.WEXITSTATUS(status)
    if os.WIFSIGNALED(status):
        return 128 + os.WTERMSIG(status)
    return -1


def read_proc_io(path):
    out = {}
    try:
        with open(path) as f:
            for line in f:
                key, _, val = line.partition(":")
                if key in ("rchar", "wchar", "read_bytes", "write_bytes"):
                    out[key] = int(val)
    except (OSError, ValueError):
        pass
    return out


def exec_measure(args):
    env = dict(os.environ)
    env.update(json.loads(args.env_json))
    cmd = args.cmd
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    result = {}
    with open(args.stdout, "wb") as out, open(args.stderr, "wb") as err:
        start = time.monotonic()
        try:
            proc = subprocess.Popen(cmd, stdout=out, stderr=err, env=env)
        except OSError as e:
            print(json.dumps({"rc": 127, "error": str(e)}))
            return 0
        pid = proc.pid
        # Linux refuses /proc/<pid>/io of a zombie, but reaping adds the
        # child's counts to this process, so the change in /proc/self/io
        # across wait4 is the tool's I/O.
        before = read_proc_io("/proc/self/io")
        _, status, ru = os.wait4(pid, 0)
        wall = time.monotonic() - start
        after = read_proc_io("/proc/self/io")
        for key in after:
            result[key] = after[key] - before.get(key, 0)
        proc.returncode = decode_status(status)
    rss = ru.ru_maxrss * (1.0 if sys.platform == "darwin" else 1024.0)
    result.update(
        {
            "rc": decode_status(status),
            "wall_s": wall,
            "user_s": ru.ru_utime,
            "sys_s": ru.ru_stime,
            "max_rss_mib": rss / MIB,
            "minflt": ru.ru_minflt,
            "majflt": ru.ru_majflt,
            "nvcsw": ru.ru_nvcsw,
            "nivcsw": ru.ru_nivcsw,
        }
    )
    print(json.dumps(result))
    return 0


def parse_perf_csv(path):
    vals = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split(",")
                if len(parts) < 3:
                    continue
                try:
                    v = float(parts[0])
                except ValueError:
                    continue
                ev = parts[2]
                if "instructions" in ev:
                    key = "instructions"
                elif "cache-references" in ev:
                    key = "cache_refs"
                elif "cache-misses" in ev:
                    key = "cache_misses"
                elif "cycles" in ev:
                    key = "cycles"
                elif "task-clock" in ev:
                    key = "task_clock"
                else:
                    continue
                vals[key] = vals.get(key, 0.0) + v
    except OSError:
        pass
    return vals


def perf_usable(tmpdir):
    if not sys.platform.startswith("linux") or not shutil.which("perf"):
        return False
    path = os.path.join(tmpdir, "perf_probe.csv")
    try:
        r = subprocess.run(
            ["perf", "stat", "-x,", "-o", path, "-e", "instructions", "--",
             "true"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    except OSError:
        return False
    return r.returncode == 0 and "instructions" in parse_perf_csv(path)


def helper_cmd(stdout_log, stderr_log, env, cmd):
    return [
        sys.executable, os.path.abspath(__file__), "--exec-measure",
        "--stdout", stdout_log, "--stderr", stderr_log,
        "--env-json", json.dumps(env), "--",
    ] + cmd


def run_measured(run_dir, env, cmd, use_perf):
    stdout_log = os.path.join(run_dir, "stdout.log")
    stderr_log = os.path.join(run_dir, "stderr.log")
    full = helper_cmd(stdout_log, stderr_log, env, cmd)
    perf_csv = os.path.join(run_dir, "perf.csv")
    if use_perf:
        full = ["perf", "stat", "-x,", "-o", perf_csv, "-e", PERF_EVENTS,
                "--"] + full
    proc = subprocess.run(full, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True)
    data = None
    for line in reversed(proc.stdout.splitlines()):
        try:
            data = json.loads(line)
            break
        except ValueError:
            continue
    if data is None:
        data = {"rc": -1, "error": "helper failed: " + proc.stderr[-300:]}
    if use_perf:
        data["perf"] = parse_perf_csv(perf_csv)
    mem = None
    try:
        with open(stderr_log, errors="replace") as f:
            for m in MEM_RE.finditer(f.read()):
                mem = m
    except OSError:
        pass
    if mem:
        data["peak"] = int(mem.group(1))
        data["heap_peak"] = int(mem.group(5))
    return data


def measure_overhead(tmpdir):
    d = os.path.join(tmpdir, "overhead")
    os.makedirs(d)
    data = run_measured(d, {}, ["/bin/true"], True)
    return data.get("perf", {})


def real(p):
    return os.path.realpath(p)


def is_inside(path, parent):
    path, parent = real(path), real(parent)
    return path == parent or path.startswith(parent.rstrip(os.sep) + os.sep)


def walk_set(path):
    files = []
    total = 0
    if os.path.isfile(path):
        return [path], os.path.getsize(path)
    for root, dirs, names in os.walk(path):
        dirs.sort()
        for n in sorted(names):
            p = os.path.join(root, n)
            try:
                if os.path.islink(p) or not os.path.isfile(p):
                    continue
                total += os.path.getsize(p)
            except OSError:
                continue
            files.append(p)
    return files, total


def read_file(path):
    n = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 22)
            if not chunk:
                return n
            n += len(chunk)


def warm_inputs(files, threads):
    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=max(1, threads)) as pool:
        nbytes = sum(pool.map(read_file, files))
    return time.monotonic() - start, nbytes


def git_commit(root):
    for exe in ("rgit", "git"):
        try:
            r = subprocess.run(
                [exe, "rev-parse", "--short", "HEAD"], cwd=root,
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                universal_newlines=True, timeout=30,
            )
            m = re.search(r"\b[0-9a-f]{7,40}\b", r.stdout)
            if r.returncode == 0 and m:
                return m.group(0)
        except (OSError, subprocess.SubprocessError):
            continue
    return ""


def find_binary(build, tool):
    name = "dftracer_" + tool
    for cand in (os.path.join(build, "bin", name), os.path.join(build, name)):
        if os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    return None


def stage_pgzip(files, dest, max_bytes):
    os.makedirs(dest)
    used = 0
    n = 0
    for p in files:
        if p.endswith(".pfw.gz"):
            name = os.path.basename(p)[:-3]
        elif p.endswith(".pfw"):
            name = os.path.basename(p)
        else:
            continue
        if n and used >= max_bytes:
            break
        target = os.path.join(dest, "%06d_%s" % (n, name))
        with open(target, "wb") as out:
            if p.endswith(".gz"):
                with gzip.open(p, "rb") as src:
                    shutil.copyfileobj(src, out, 1 << 20)
            else:
                with open(p, "rb") as src:
                    shutil.copyfileobj(src, out, 1 << 20)
        used += os.path.getsize(target)
        n += 1
    return n, used


def link_inputs(set_path, files, link_dir):
    os.makedirs(link_dir)
    if os.path.isfile(set_path):
        link = os.path.join(link_dir, os.path.basename(set_path))
        os.symlink(set_path, link)
        return link
    for f in files:
        name = os.path.relpath(f, set_path).replace(os.sep, "__")
        os.symlink(f, os.path.join(link_dir, name))
    return link_dir


def dir_times(set_path):
    top = set_path if os.path.isdir(set_path) else os.path.dirname(set_path)
    times = {}
    for root, dirs, _ in os.walk(top):
        if not os.path.isdir(set_path):
            dirs[:] = []
        try:
            times[root] = os.stat(root).st_mtime_ns
        except OSError:
            continue
    return times


def owned_writes(set_path, before, since):
    """Entries owned by this user, changed at or after since, in a dir of the
    set whose mtime differs from before."""
    uid = os.getuid()
    floor = int(since)
    hits = []
    for d, mtime in dir_times(set_path).items():
        if before.get(d) == mtime:
            continue
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for n in names:
            p = os.path.join(d, n)
            try:
                st = os.lstat(p)
            except OSError:
                continue
            if st.st_uid == uid and max(st.st_mtime, st.st_ctime) >= floor:
                hits.append(p)
    return hits


def build_cmd(tool, binary, in_path, is_file, run_dir, threads, stage_dir):
    idx = os.path.join(run_dir, "idx")
    out = os.path.join(run_dir, "out")
    os.makedirs(idx)
    os.makedirs(out)
    in_dir = os.path.dirname(in_path) if is_file else in_path
    if tool in FILES_FLAG:
        src = ["--files", in_path] if is_file else ["-d", in_dir]
    elif tool in PATH_ARG:
        src = []
    elif tool == "pgzip":
        src = ["-d", stage_dir]
    else:
        src = ["-d", in_dir]
    cmd = [binary]
    if threads:
        cmd += ["--executor-threads", str(threads)]
    if tool in ("index", "event_count", "info", "stats"):
        cmd += src + ["--index-dir", idx]
    elif tool == "view":
        cmd += src + ["--index-dir", idx, "--group-by", "name"]
    elif tool == "split":
        cmd += src + ["-o", out, "-n", "bench", "-s", "64"]
    elif tool in ("validate", "pgzip"):
        cmd += src
    elif tool == "comparator":
        cmd += [
            "--baseline", in_path, "--variant", in_path,
            "--baseline-index-dir", os.path.join(idx, "baseline"),
            "--variant-index-dir", os.path.join(idx, "variant"),
        ]
    elif tool == "replay":
        cmd += ["--dry-run", "--dftracer-mode", "--no-sleep", in_path]
    return cmd, idx, out


def fmt(v, digits=None):
    if v is None:
        return ""
    if digits is not None:
        return ("%." + str(digits) + "f") % v
    return str(int(v)) if float(v) == int(v) else str(v)


def sub(perf, overhead, key):
    if key not in perf:
        return None
    return max(0.0, perf[key] - overhead.get(key, 0.0))


def make_row(base, data, overhead):
    row = dict(base)
    row["rc"] = data.get("rc", -1)
    wall = data.get("wall_s")
    user = data.get("user_s")
    sysm = data.get("sys_s")
    row["wall_s"] = fmt(wall, 4)
    row["user_s"] = fmt(user, 4)
    row["sys_s"] = fmt(sysm, 4)
    if wall and user is not None and sysm is not None:
        row["parallelism"] = fmt((user + sysm) / wall, 3)
    row["max_rss_mib"] = fmt(data.get("max_rss_mib"), 1)
    if "heap_peak" in data:
        row["heap_peak_mib"] = fmt(data["heap_peak"] / MIB, 1)
        row["peak_mib"] = fmt(data["peak"] / MIB, 1)
    for k in ("rchar", "wchar", "read_bytes", "write_bytes", "minflt",
              "majflt", "nvcsw", "nivcsw"):
        if k in data:
            row[k] = fmt(data[k])
    perf = data.get("perf") or {}
    ins = sub(perf, overhead, "instructions")
    cyc = sub(perf, overhead, "cycles")
    refs = sub(perf, overhead, "cache_refs")
    miss = sub(perf, overhead, "cache_misses")
    row["instructions"] = fmt(ins)
    row["cycles"] = fmt(cyc)
    row["cache_refs"] = fmt(refs)
    row["cache_misses"] = fmt(miss)
    if ins is not None and cyc:
        row["ipc"] = fmt(ins / cyc, 3)
    if miss is not None and refs:
        row["miss_pct"] = fmt(100.0 * miss / refs, 2)
    if "error" in data:
        row["note"] = data["error"].replace("\t", " ").replace("\n", " ")
    return row


def add_scaling(rows):
    walls = {}
    for r in rows:
        if r["threads"] != "default" and str(r["rc"]) == "0" and r["wall_s"]:
            walls.setdefault(int(r["threads"]), []).append(float(r["wall_s"]))
    if len(walls) < 2:
        return
    t0 = min(walls)
    base = sum(walls[t0]) / len(walls[t0])
    for r in rows:
        if r["threads"] == "default" or str(r["rc"]) != "0" or not r["wall_s"]:
            continue
        t = int(r["threads"])
        mean = sum(walls[t]) / len(walls[t])
        sp = base / mean
        r["speedup"] = fmt(sp, 3)
        r["efficiency"] = fmt(sp / (t / float(t0)), 3)


def append_rows(path, rows):
    new = not os.path.exists(path) or os.path.getsize(path) == 0
    with open(path, "a", newline="") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        if new:
            w.writerow(COLUMNS)
        for r in rows:
            w.writerow([r.get(c, "") for c in COLUMNS])


def read_known(path):
    known = []
    if not path:
        return known
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 5 or not parts[4].strip():
                raise ValueError("%s:%d: want tool, set, threads, rc, reason"
                                 % (path, n))
            known.append(tuple(parts[:4]))
    return known


def check(path, known_path=None):
    try:
        known = read_known(known_path)
    except (OSError, ValueError) as e:
        print("check: %s" % e, file=sys.stderr)
        return 1
    status = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            key = (r["tool"], r["set"])
            ok = r["rc"] == "0"
            status.setdefault(key, []).append((r["threads"], r["rc"], ok))
    if not status:
        print("check: no rows in %s" % path, file=sys.stderr)
        return 1

    def matches(entry, tool, setname, threads, rc):
        return (entry[0] == tool and entry[1] == setname
                and entry[2] in ("*", threads) and entry[3] == rc)

    bad = accepted = 0
    used = set()
    for (tool, setname), runs in sorted(status.items()):
        for threads, rc, ok in runs:
            if ok:
                continue
            hit = [k for k in known if matches(k, tool, setname, threads, rc)]
            if hit:
                accepted += 1
                used.update(hit)
                print("KNOWN tool=%s set=%s threads=%s rc=%s"
                      % (tool, setname, threads, rc))
                continue
            bad += 1
            print("FAIL tool=%s set=%s threads=%s rc=%s"
                  % (tool, setname, threads, rc))
    for k in known:
        runs = status.get((k[0], k[1]))
        if runs and k not in used:
            bad += 1
            print("STALE known entry tool=%s set=%s threads=%s rc=%s: no run"
                  " fails that way" % k)
    print("check: %d pairs, %d failed runs, %d known"
          % (len(status), bad, accepted))
    return 1 if bad else 0


def parse_args(argv):
    p = argparse.ArgumentParser(description="dftracer-utils scale ladder")
    p.add_argument("--build", help="dir with bin/<tool> (or the tools)")
    p.add_argument("--set", action="append", default=[], metavar="NAME=PATH")
    p.add_argument("--tools", default=",".join(TOOLS))
    p.add_argument("--threads", default="")
    p.add_argument("--scratch")
    p.add_argument("--out")
    p.add_argument("--dftracer", metavar="LIB")
    p.add_argument("--check", metavar="FILE.tsv")
    p.add_argument("--known", metavar="FILE.tsv")
    p.add_argument("--repeat", type=int, default=1)
    p.add_argument("--no-warm", action="store_true",
                   help="skip the untimed read of each set before its runs")
    p.add_argument("--warm-threads", type=int, default=8)
    p.add_argument("--pgzip-max-mib", type=int, default=256)
    p.add_argument("--commit", help="commit column (default: git HEAD)")
    p.add_argument("--exec-measure", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--stdout", help=argparse.SUPPRESS)
    p.add_argument("--stderr", help=argparse.SUPPRESS)
    p.add_argument("--env-json", default="{}", help=argparse.SUPPRESS)
    p.add_argument("cmd", nargs=argparse.REMAINDER, help=argparse.SUPPRESS)
    return p.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    if args.exec_measure:
        return exec_measure(args)
    if args.check:
        return check(args.check, args.known)
    for opt in ("build", "scratch", "out"):
        if not getattr(args, opt):
            print("error: --%s is required" % opt, file=sys.stderr)
            return 2
    if not args.set:
        print("error: at least one --set NAME=PATH is required",
              file=sys.stderr)
        return 2
    sets = []
    for s in args.set:
        name, eq, path = s.partition("=")
        if not eq or not name or not path or not os.path.exists(path):
            print("error: bad or missing --set %r" % s, file=sys.stderr)
            return 2
        sets.append((name, os.path.abspath(path)))
    tools = [t for t in args.tools.split(",") if t]
    for t in tools:
        if t not in TOOLS:
            print("error: unknown tool %r" % t, file=sys.stderr)
            return 2
    binaries = {}
    for t in tools:
        b = find_binary(args.build, t)
        if not b:
            print("error: dftracer_%s not found under %s" % (t, args.build),
                  file=sys.stderr)
            return 2
        binaries[t] = os.path.abspath(b)
    threads_list = [int(x) for x in args.threads.split(",") if x] or [0]
    if args.dftracer and not os.path.isfile(args.dftracer):
        print("error: --dftracer library not found", file=sys.stderr)
        return 2
    scratch = os.path.abspath(args.scratch)
    for name, path in sets:
        if is_inside(scratch, path):
            print("error: scratch %s is inside set %s (%s)"
                  % (scratch, name, path), file=sys.stderr)
            return 2
    os.makedirs(scratch, exist_ok=True)

    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    commit = args.commit or git_commit(root)
    host = socket.gethostname()
    stamp = "%d_%d" % (int(time.time()), os.getpid())
    meta = os.path.join(scratch, "_meta_" + stamp)
    os.makedirs(meta)
    use_perf = perf_usable(meta)
    overhead = measure_overhead(meta) if use_perf else {}
    if use_perf:
        print("perf: on, helper overhead subtracted: %s" % overhead,
              file=sys.stderr)
    else:
        print("perf: off", file=sys.stderr)

    failed = 0
    for name, path in sets:
        is_file = os.path.isfile(path)
        files, total = walk_set(path)
        warm_note = ""
        if not args.no_warm:
            secs, nbytes = warm_inputs(files, args.warm_threads)
            warm_note = "warm read %d files, %d bytes, %.2f s" % (
                len(files), nbytes, secs)
            print("%s: %s" % (name, warm_note), file=sys.stderr)
        for tool in tools:
            if tool == "pgzip" and not any(
                    f.endswith((".pfw", ".pfw.gz")) for f in files):
                print("skip pgzip on %s: no .pfw or .pfw.gz file" % name,
                      file=sys.stderr)
                continue
            rows = []
            for threads in threads_list:
                for rep in range(1, args.repeat + 1):
                    run_dir = os.path.join(
                        scratch, name, tool,
                        "t%s_r%d_%s" % (threads or "default", rep, stamp))
                    os.makedirs(run_dir)
                    note, warm_note = warm_note, ""
                    stage = os.path.join(run_dir, "pfw")
                    if tool == "pgzip":
                        n, used = stage_pgzip(
                            files, stage, args.pgzip_max_mib * 1024 * 1024)
                        note = (note + " pgzip staged %d files, %d bytes"
                                % (n, used)).strip()
                    in_path = link_inputs(
                        path, files, os.path.join(run_dir, "in"))
                    cmd, idx, out = build_cmd(
                        tool, binaries[tool], in_path, is_file, run_dir,
                        threads, stage)
                    for d in (scratch, run_dir, idx, out):
                        if is_inside(d, path):
                            print("error: %s is inside set %s" % (d, path),
                                  file=sys.stderr)
                            return 2
                    env = {"DFTRACER_UTILS_MEMORY_REPORT": "1"}
                    if threads:
                        env["DFTRACER_UTILS_THREADS"] = str(threads)
                    if args.dftracer:
                        tdir = os.path.join(run_dir, "dftracer")
                        os.makedirs(tdir)
                        preload = ("DYLD_INSERT_LIBRARIES"
                                   if sys.platform == "darwin"
                                   else "LD_PRELOAD")
                        env.update({
                            preload: os.path.abspath(args.dftracer),
                            "DFTRACER_ENABLE": "1",
                            "DFTRACER_INIT": "PRELOAD",
                            "DFTRACER_LOG_FILE": os.path.join(tdir, "trace"),
                            "DFTRACER_DATA_DIR": "all",
                            "DFTRACER_INC_METADATA": "1",
                            "DFTRACER_UTILS_IO_BACKEND": "threadpool",
                        })
                    before = dir_times(path)
                    start = time.time()
                    data = run_measured(run_dir, env, cmd, use_perf)
                    wrote = owned_writes(path, before, start)
                    if wrote:
                        print("error: %s wrote inside set %s: %s"
                              % (tool, name, " ".join(wrote[:5])),
                              file=sys.stderr)
                        return 2
                    base = {
                        "commit": commit, "host": host, "tool": tool,
                        "set": name, "input_bytes": total,
                        "input_files": len(files),
                        "threads": threads or "default", "repeat": rep,
                        "run_dir": run_dir, "note": note,
                    }
                    row = make_row(base, data, overhead)
                    if row["note"] != note:
                        row["note"] = (note + " " + row["note"]).strip()
                    if data.get("rc") != 0:
                        failed += 1
                        print("FAIL %s %s threads=%s rc=%s (%s)"
                              % (tool, name, row["threads"], data.get("rc"),
                                 os.path.join(run_dir, "stderr.log")),
                              file=sys.stderr)
                    rows.append(row)
            add_scaling(rows)
            append_rows(args.out, rows)
    print("done: %d failed runs" % failed, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
