#!/bin/sh
# usage: run_baseline.sh <build-dir> <rounds> <output-file>
# Runs view_api_bench, index_bench, duql_eval_bench, group_transform_bench and
# the Python import once per round, in that order, for <rounds> rounds. Writes
# a markdown record: the median wall time of each over the rounds, then the
# output of the last round and its index_bench.json. view_api_bench exits 1 when a case is slower than its reference; that
# is a result here, not a failure.
set -eu

if [ $# -ne 3 ]; then
    echo "usage: run_baseline.sh <build-dir> <rounds> <output-file>" >&2
    exit 2
fi
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$(cd "$1" && pwd)
rounds=$2
case $3 in /*) out=$3 ;; *) out=$(pwd)/$3 ;; esac

exec python3 -I - "$root" "$build" "$rounds" "$out" <<'PY'
import glob
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time

root, build, rounds, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]


def cache_value(name):
    with open(os.path.join(build, "CMakeCache.txt")) as handle:
        for line in handle:
            match = re.match(r"%s:[A-Z]+=(.*)" % re.escape(name), line)
            if match:
                return match.group(1)
    sys.exit("CMakeCache.txt has no %s" % name)


def timed(command, env=None, ok=(0,)):
    start = time.perf_counter()
    done = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True
    )
    elapsed = time.perf_counter() - start
    if done.returncode not in ok:
        sys.exit("failed (%d): %s\n%s" % (done.returncode, " ".join(command), done.stdout))
    return elapsed, done.stdout


python = cache_value("_Python_EXECUTABLE")
with tempfile.TemporaryDirectory() as work:
    package = os.path.join(work, "pkg", "dftracer", "utils")
    os.makedirs(package)
    sources = glob.glob(os.path.join(root, "python", "dftracer", "utils", "*"))
    sources += glob.glob(os.path.join(build, "dftracer", "utils", "dftracer_utils_ext*"))
    for source in sources:
        os.symlink(source, os.path.join(package, os.path.basename(source)))
    py_env = dict(os.environ, PYTHONPATH=os.path.join(work, "pkg"))

    view_dir = os.path.join(work, "view")
    bin_dir = os.path.join(build, "bin")

    def exe(name):
        found = glob.glob(os.path.join(build, "**", name), recursive=True)
        found = [f for f in found if os.access(f, os.X_OK) and os.path.isfile(f)]
        if not found:
            sys.exit("%s is not built in %s" % (name, build))
        return found[0]

    benches = {name: exe(name) for name in (
        "view_api_bench", "index_bench", "duql_eval_bench", "group_transform_bench")}

    steps = [
        ("view_api_bench", lambda: timed([benches["view_api_bench"], "2000000", view_dir], ok=(0, 1))),
        ("index_bench", lambda: timed([
            benches["index_bench"], "--workdir", os.path.join(work, "index"),
            "--out", os.path.join(work, "index_bench.json"),
            os.path.join(view_dir, "bench.pfw.gz")])),
        ("duql_eval_bench", lambda: timed([benches["duql_eval_bench"]])),
        ("group_transform_bench", lambda: timed([benches["group_transform_bench"]])),
        ("python_import", lambda: (statistics.median(
            timed([python, "-c", "import dftracer.utils"], py_env)[0] for _ in range(5)),
            "")),
    ]
    seconds = {name: [] for name, _ in steps}
    last = {}
    for _ in range(rounds):
        for name, step in steps:
            elapsed, text = step()
            seconds[name].append(elapsed)
            last[name] = text
        with open(os.path.join(work, "index_bench.json")) as handle:
            last["index_bench json"] = handle.read()

env = dict(os.environ, RGIT_OUTPUT="human")
commit = subprocess.run(["rgit", "rev-parse", "--short", "HEAD"], cwd=root, env=env,
                        stdout=subprocess.PIPE, text=True).stdout.strip()
cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout.strip()
if not cpu:
    with open("/proc/cpuinfo") as handle:
        cpu = next((l.split(":", 1)[1].strip() for l in handle if l.startswith("model name")), "unknown")
lines = [
    "# Benchmark baseline, %s" % time.strftime("%Y-%m-%d"),
    "",
    "Recorded on %s %s, %s (%d cores), commit %s. Command:" % (
        os.uname().sysname, os.uname().release, cpu, os.cpu_count(), commit),
    "",
    "```",
    "scripts/bench/run_baseline.sh <build-dir> %d <output-file>" % rounds,
    "```",
    "",
    "Each round runs the five measurements in this order; the table is the median over %d rounds of the wall time of the whole run. `view_api_bench` exits 1 when a case is slower than its reference, which is a result here." % rounds,
    "",
    "| Measurement | Median wall seconds | Rounds |",
    "|---|---|---|",
]
for name, _ in steps:
    lines.append("| `%s` | %.3f | %s |" % (
        name, statistics.median(seconds[name]), " ".join("%.3f" % v for v in seconds[name])))
lines += ["", "`python_import` is the median of 5 `import dftracer.utils` runs per round.",
          "", "## Output of the last round", ""]
for name in [n for n, _ in steps] + ["index_bench json"]:
    if last.get(name):
        body = "\n".join(l for l in last[name].splitlines() if not l.startswith("[20"))
        lines += ["### %s, last round" % name if name == "index_bench json" else "### " + name,
                  "", "```", body.strip("\n"), "```", ""]
with open(out, "w") as handle:
    handle.write("\n".join(lines).rstrip("\n") + "\n")
print("wrote", out)
PY
