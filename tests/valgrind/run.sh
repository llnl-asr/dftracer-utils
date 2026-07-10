#!/usr/bin/env bash
#
# Valgrind test runner for dftracer-utils, executed inside the Linux container
# defined by tests/valgrind/Dockerfile. Invoked by the `valgrind*` Makefile
# targets, but can also be run directly inside the container.
#
# Subcommands:
#   cpp    Build the C++ tests and run each doctest binary under Valgrind.
#   py     Build/install the Python package and run the native-binding-focused
#          tests under Valgrind.
#   mpi    Build with MPI enabled and run the MPI tests with each rank wrapped
#          in Valgrind (opt-in; requires an MPI toolchain).
#   all    Run cpp then py (default; MPI is opt-in via the `mpi` subcommand).
#
# Useful environment overrides:
#   VALGRIND_BUILD_DIR     C++ build directory (default build/build-valgrind)
#   VALGRIND_CTEST_FILTER  Regex; only C++ tests whose name matches run
#   VALGRIND_PYTEST_FILES  Space-separated pytest targets (overrides default set)
#   VALGRIND_EXTRA_OPTS    Extra options appended to every valgrind invocation
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

export DFTRACER_UTILS_VALGRIND=1
export DFTRACER_UTILS_HW_CONCURRENCY="${DFTRACER_UTILS_HW_CONCURRENCY:-2}"
# pyarrow runtime-dispatches to AVX-512 kernels on capable CPUs, whose EVEX
# (0x62-prefixed) instructions Valgrind cannot decode and aborts with SIGILL.
# Cap Arrow's SIMD level so it stays within what Valgrind emulates.
export ARROW_USER_SIMD_LEVEL="${ARROW_USER_SIMD_LEVEL:-AVX2}"

SUPP_DIR="$REPO_ROOT/tests/valgrind"
BUILD_DIR="${VALGRIND_BUILD_DIR:-$REPO_ROOT/build/build-valgrind}"
LOG_DIR="$REPO_ROOT/build/valgrind-logs"
VENV_DIR="$REPO_ROOT/.venv_valgrind"

# Common valgrind options
COMMON_OPTS=(
  --tool=memcheck
  --error-exitcode=1
  --num-callers=20
  # OS-fair scheduling instead of Valgrind's serial scheduler, which otherwise
  # starves the executor's worker threads and livelocks the threaded tests.
  --fair-sched=yes
  ${VALGRIND_EXTRA_OPTS:-}
)

DEFAULT_PYTEST_FILES=(tests/python)

log() { printf '\033[1;34m[valgrind]\033[0m %s\n' "$*"; }
err() { printf '\033[1;31m[valgrind]\033[0m %s\n' "$*" >&2; }

# Seconds since the epoch, sub-second where available. BSD date has no %N.
now_s() {
  local t
  t="$(date +%s.%N 2>/dev/null || true)"
  if [[ "$t" =~ ^[0-9]+\.[0-9]+$ ]]; then
    printf '%s' "$t"
  else
    python3 -c 'import time; print(time.time())'
  fi
}

# Override with VALGRIND_BUILD_JOBS.
build_jobs() {
  if [[ -n "${VALGRIND_BUILD_JOBS:-}" ]]; then
    echo "$VALGRIND_BUILD_JOBS"
    return
  fi
  local cpus mem_gb jobs
  cpus="$(nproc 2>/dev/null || echo 4)"
  if [[ -r /proc/meminfo ]]; then
    mem_gb=$(awk '/MemTotal/{printf "%d", $2/1024/1024}' /proc/meminfo)
    jobs=$((mem_gb / 2))
    ((jobs < 1)) && jobs=1
    ((jobs > cpus)) && jobs="$cpus"
  else
    jobs="$cpus"
  fi
  echo "$jobs"
}

configure_and_build_cpp() {
  log "Configuring C++ tests in $BUILD_DIR (RelWithDebInfo, debug symbols)"
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DDFTRACER_UTILS_TESTS=ON \
    -DDFTRACER_UTILS_DEBUG=OFF \
    -DDFTRACER_UTILS_COVERAGE=OFF \
    -DDFTRACER_UTILS_BUILD_BINARIES=ON \
    -DDFTRACER_UTILS_BUILD_PYTHON=OFF \
    -DDFTRACER_UTILS_BUILD_STATIC=OFF \
    -DDFTRACER_UTILS_ROCKSDB_PREFIX="${DFTRACER_UTILS_ROCKSDB_PREFIX:-}" \
    -DDFTRACER_UTILS_VALGRIND_MODE=ON \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  local jobs
  jobs="$(build_jobs)"
  log "Building C++ tests (-j $jobs)"
  cmake --build "$BUILD_DIR" -j "$jobs"
}

run_cpp() {
  configure_and_build_cpp

  mkdir -p "$LOG_DIR/cpp"

  local exclude_pat="${VALGRIND_CTEST_EXCLUDE:-test_reader_robustness}"

  local selector
  selector="$(
    python3 - "$BUILD_DIR" "${VALGRIND_CTEST_FILTER:-}" "$exclude_pat" \
             "${VALGRIND_SHARD:-1/1}" "$SUPP_DIR/durations.tsv" <<'PY'
import base64, json, os, re, subprocess, sys
build_dir, name_filter, exclude = sys.argv[1], sys.argv[2], sys.argv[3]
shard_spec, durations_path = sys.argv[4], sys.argv[5]

shard_index, shard_count = (int(x) for x in shard_spec.split("/", 1))
if not 1 <= shard_index <= shard_count:
    sys.exit(f"VALGRIND_SHARD={shard_spec} is out of range")

inc = re.compile(name_filter) if name_filter else None
exc = re.compile(exclude) if exclude else None
out = subprocess.check_output(
    ["ctest", "--test-dir", build_dir, "--show-only=json-v1"])
data = json.loads(out)

selected = []
known = set()                    # every test ctest knows about, before filtering
for t in data.get("tests", []):
    name = t.get("name", "")
    known.add(name)
    cmd = t.get("command") or []
    if len(cmd) != 1:            # skip multi-arg tests (discovery / CLI)
        continue
    exe = cmd[0]
    if not (os.path.isfile(exe) and os.access(exe, os.X_OK)):
        continue
    if inc and not inc.search(name):
        continue
    if exc and exc.search(name):
        print(f"SKIP\t{name}")
        continue
    wd = build_dir
    env = []
    for p in t.get("properties", []):
        n = p.get("name")
        if n == "WORKING_DIRECTORY":
            wd = p.get("value") or wd
        elif n == "ENVIRONMENT":          # e.g. DFTRACER_*_PATH=<real binary>
            v = p.get("value")
            env.extend(v if isinstance(v, list) else [v] if v else [])
    # base64 so VAR=value entries survive the tab-delimited line intact.
    env_b64 = base64.b64encode("\n".join(env).encode()).decode() if env else ""
    selected.append((name, wd, exe, env_b64))

# Durations are a hint for shard balancing only: a stale or missing entry costs
# balance, never correctness.
hints = {}
try:
    with open(durations_path) as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if line.startswith("#") or not line.strip():
                continue
            test_name, _, seconds = line.partition("\t")
            try:
                hints[test_name] = float(seconds)
            except ValueError:
                print(f"durations.tsv:{lineno}: malformed line, ignoring",
                      file=sys.stderr)
except OSError:
    pass

if hints:
    # `missing` is over the tests about to run; `stale` is over everything ctest
    # knows about, so a filter or an exclusion cannot fake a stale entry.
    missing = sum(1 for e in selected if e[0] not in hints)
    stale = sum(1 for name in hints if name not in known)
    if missing or stale:
        print(f"HINTS\t{missing}\t{stale}")

if shard_count > 1:
    default = sorted(hints.values())[len(hints) // 2] if hints else 1.0
    # No hints: every weight is 1.0 and the stable sort preserves the ctest
    # order, degrading to round-robin. The partition is over the discovered
    # tests, so every test lands in exactly one shard either way.
    bins = [[] for _ in range(shard_count)]
    loads = [0.0] * shard_count
    for entry in sorted(selected, key=lambda e: -hints.get(e[0], default)):
        i = loads.index(min(loads))
        bins[i].append(entry)
        loads[i] += hints.get(entry[0], default)
    selected = bins[shard_index - 1]
    estimate = f"{loads[shard_index - 1]:.0f}" if hints else "-1"
    print(f"SHARD\t{shard_index}/{shard_count}\t{len(selected)}\t{estimate}")

for name, wd, exe, env_b64 in selected:
    print(f"RUN\t{name}\t{wd}\t{exe}\t{env_b64}")
PY
  )"

  if [[ -z "$selector" ]]; then
    err "No C++ unit-test binaries selected (filter='${VALGRIND_CTEST_FILTER:-}')."
    return 1
  fi

  # --track-origins doubles runtime; off by default, on with VALGRIND_TRACK_ORIGINS=1.
  CPP_OPTS=()
  if [[ "${VALGRIND_TRACK_ORIGINS:-0}" == "1" ]]; then
    CPP_OPTS+=(--track-origins=yes)
  fi

  local test_timeout="${VALGRIND_TEST_TIMEOUT:-2400}"
  local max_jobs
  max_jobs="$(test_jobs)"

  local results_dir="$LOG_DIR/cpp/.results"
  rm -rf "$results_dir"
  mkdir -p "$results_dir"

  local total=0 skipped=0 skipped_names=()
  log "Running C++ tests under Valgrind ($max_jobs concurrent, ${test_timeout}s/test timeout)"
  while IFS=$'\t' read -r tag name wd exe env_b64; do
    [[ -z "$tag" ]] && continue
    if [[ "$tag" == "SKIP" ]]; then
      skipped=$((skipped + 1))
      skipped_names+=("$name")
      continue
    fi
    if [[ "$tag" == "HINTS" ]]; then
      # name=<tests with no hint> wd=<hints for tests that no longer exist>
      if ((name > 0)); then
        err "durations.tsv: $name test(s) have no entry"
      fi
      if ((wd > 0)); then
        err "durations.tsv: $wd stale entry(ies) for tests that no longer exist"
      fi
      err "Regenerate with: VALGRIND_EMIT_DURATIONS=1 make valgrind-cpp"
      continue
    fi
    if [[ "$tag" == "SHARD" ]]; then
      # name=<i/N> wd=<test count> exe=<estimated seconds, -1 if unknown>
      if [[ "$exe" == "-1" ]]; then
        log "Shard $name: $wd test(s), no duration hints (balanced by count)"
      else
        log "Shard $name: $wd test(s), ~${exe}s estimated"
      fi
      continue
    fi
    total=$((total + 1))
    while (($(jobs -rp | wc -l) >= max_jobs)); do wait -n || true; done
    run_cpp_one "$name" "$wd" "$exe" "$results_dir" "$test_timeout" "$env_b64" &
  done <<<"$selector"
  wait || true

  if [[ "${VALGRIND_EMIT_DURATIONS:-0}" == "1" ]]; then
    local out="$SUPP_DIR/durations.tsv"
    if [[ "${VALGRIND_SHARD:-1/1}" != "1/1" ]]; then
      out="$LOG_DIR/cpp/durations-shard.tsv"
      err "Sharded run: writing partial durations to $out (run unsharded for a full manifest)"
    fi
    local dur_files=("$results_dir"/*.dur)
    {
      echo "# Valgrind wall time per C++ test, seconds. A hint for shard balancing"
      echo "# only (VALGRIND_SHARD); a stale or missing entry costs balance, never"
      echo "# correctness. run.sh warns when this file drifts from the test list."
      echo "# Regenerate: VALGRIND_EMIT_DURATIONS=1 make valgrind-cpp"
      if [[ -e "${dur_files[0]}" ]]; then
        cat "${dur_files[@]}" | sort -t"$(printf '\t')" -k2 -rn
      fi
    } >"$out"
    log "Wrote $out"
  fi

  local failed=0 failed_names=()
  local f rc rname
  for f in "$results_dir"/*.rc; do
    [[ -e "$f" ]] || continue
    IFS=$'\t' read -r rc rname <"$f"
    ((rc == 0)) && continue
    failed=$((failed + 1))
    local logf="$LOG_DIR/cpp/${rname//\//_}.log"
    if ((rc == 124)); then
      failed_names+=("$rname (TIMEOUT ${test_timeout}s)")
    elif ((rc == 137)); then
      failed_names+=("$rname (KILLED: timeout SIGKILL or OOM)")
    else
      failed_names+=("$rname")
      sed -n '1,120p' "$logf" >&2 || true
    fi
  done

  if ((skipped > 0)); then
    log "C++ skipped ${skipped} test(s) via VALGRIND_CTEST_EXCLUDE: ${skipped_names[*]}"
  fi
  log "C++ summary: $((total - failed))/$total passed under Valgrind"
  if ((failed > 0)); then
    err "C++ Valgrind failures: ${failed_names[*]}"
    return 1
  fi
}

# Concurrency for parallel valgrind runs
test_jobs() {
  if [[ -n "${VALGRIND_TEST_JOBS:-}" ]]; then
    echo "$VALGRIND_TEST_JOBS"
    return
  fi
  local cpus mem_gb jobs
  cpus="$(nproc 2>/dev/null || echo 4)"
  if [[ -r /proc/meminfo ]]; then
    mem_gb=$(awk '/MemTotal/{printf "%d", $2/1024/1024}' /proc/meminfo)
    jobs=$((mem_gb / 3))
    ((jobs < 1)) && jobs=1
    ((jobs > cpus)) && jobs="$cpus"
  else
    jobs="$cpus"
  fi
  echo "$jobs"
}

# Run one test binary under Valgrind and record its exit code
run_cpp_one() {
  local name="$1" wd="$2" exe="$3" rdir="$4" tmo="$5" env_b64="${6:-}"
  local logf="$LOG_DIR/cpp/${name//\//_}.log"
  local rc=0
  local start_ts
  start_ts="$(now_s)"

  # Apply the test's ctest ENVIRONMENT
  local -a test_env=()
  if [[ -n "$env_b64" ]]; then
    local line
    while IFS= read -r line; do
      [[ -n "$line" ]] && test_env+=("$line")
    done < <(printf '%s' "$env_b64" | base64 -d)
  fi

  local -a extra=()
  if [[ "$name" == binaries/* ]]; then
    extra+=(--trace-children=yes)
  fi

  local -a doctest_args=()
  case "${name##*/}" in
  test_channel | test_reader | test_reader_stream | \
    test_reader_line_byte_stream | test_reader_multi_lines_byte_stream | \
    test_reader_formats | test_indexed_file_line_iterator | \
    test_indexed_file_bytes_iterator | \
    test_async_indexed_file_line_generator | \
    test_async_indexed_file_bytes_generator)
    doctest_args=(--test-suite=vg)
    ;;
  esac

  # tmo=0 means no per-test timeout: run without `timeout` rather than rely on
  # `timeout 0` (GNU-only) so the documented behaviour is portable and explicit.
  local -a tmo_cmd=()
  [[ "$tmo" != "0" ]] && tmo_cmd=(timeout --kill-after=30 "$tmo")

  log "C++  ▶ $name"
  (cd "$wd" && env "${test_env[@]}" "${tmo_cmd[@]}" \
    valgrind "${COMMON_OPTS[@]}" "${CPP_OPTS[@]}" "${extra[@]}" \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect \
    --suppressions="$SUPP_DIR/valgrind-cpp.supp" \
    --log-file="$logf" \
    "$exe" "${doctest_args[@]}" >/dev/null 2>&1) || rc=$?
  printf '%s\t%s\n' "$rc" "$name" >"$rdir/${name//\//_}.rc"
  printf '%s\t%s\n' "$name" \
    "$(awk -v a="$start_ts" -v b="$(now_s)" 'BEGIN { printf "%.1f", b - a }')" \
    >"$rdir/${name//\//_}.dur"
  if ((rc == 0)); then
    log "C++  ✓ $name"
  elif ((rc == 124)); then
    err "C++  ✗ TIMEOUT ${tmo}s: $name"
  elif ((rc == 137)); then
    err "C++  ✗ KILLED (timeout SIGKILL or OOM): $name"
  else
    err "C++  ✗ FAILED (exit $rc): $name"
  fi
}

run_mpi() {
  local mpi_build_dir="${VALGRIND_MPI_BUILD_DIR:-$REPO_ROOT/build/build-valgrind-mpi}"
  local wrapper="$SUPP_DIR/mpiexec-valgrind"

  if ! command -v mpiexec >/dev/null 2>&1; then
    err "mpiexec not found. Install an MPI toolchain (e.g. libopenmpi-dev openmpi-bin) or run via Docker."
    return 1
  fi

  log "Configuring MPI tests in $mpi_build_dir (ENABLE_MPI=ON, binaries ON)"
  cmake -S "$REPO_ROOT" -B "$mpi_build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DDFTRACER_UTILS_TESTS=ON \
    -DDFTRACER_UTILS_DEBUG=OFF \
    -DDFTRACER_UTILS_COVERAGE=OFF \
    -DDFTRACER_UTILS_ENABLE_MPI=ON \
    -DDFTRACER_UTILS_BUILD_BINARIES=ON \
    -DDFTRACER_UTILS_BUILD_PYTHON=OFF \
    -DDFTRACER_UTILS_BUILD_STATIC=OFF \
    -DDFTRACER_UTILS_ROCKSDB_PREFIX="${DFTRACER_UTILS_ROCKSDB_PREFIX:-}" \
    -DDFTRACER_UTILS_VALGRIND_MODE=ON \
    -DMPIEXEC_EXECUTABLE="$wrapper" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

  local jobs
  jobs="$(build_jobs)"
  log "Building MPI tests and binaries (-j $jobs)"
  cmake --build "$mpi_build_dir" -j "$jobs" --target \
    binaries_test_dftracer_call_tree_mpi \
    binaries_test_dftracer_aggregator_mpi \
    dftracer_call_tree dftracer_call_tree_mpi \
    dftracer_aggregator dftracer_aggregator_mpi

  mkdir -p "$LOG_DIR"
  log "Running MPI tests (ranks wrapped in Valgrind via $wrapper)"
  ctest --test-dir "$mpi_build_dir" \
    -R "test_dftracer_(call_tree|aggregator)_mpi" \
    --output-on-failure --no-tests=error
}

setup_python() {
  if [[ ! -d "$VENV_DIR" ]]; then
    log "Creating Python venv at ${VENV_DIR#$REPO_ROOT/}"
    python3 -m venv "$VENV_DIR"
    "$VENV_DIR/bin/pip" install --quiet --upgrade pip setuptools wheel
  fi
  log "Installing dftracer-utils (editable, RelWithDebInfo) + dev deps"
  CMAKE_BUILD_PARALLEL_LEVEL="$(build_jobs)" \
    "$VENV_DIR/bin/pip" install --quiet -e ".[dev]" \
    --config-settings=cmake.build-type=RelWithDebInfo \
    --config-settings=cmake.define.DFTRACER_UTILS_VALGRIND_MODE=ON \
    --config-settings=cmake.define.CMAKE_C_COMPILER_LAUNCHER=ccache \
    --config-settings=cmake.define.CMAKE_CXX_COMPILER_LAUNCHER=ccache
}

# Run one pytest file under its own Valgrind process, recording exit code.
# Mirrors run_cpp_one so Python tests run in parallel like C++ tests do.
run_py_one() {
  local file="$1" rdir="$2" xmldir="$3" py_timeout="$4"
  local fname rc=0
  fname="$(basename "$file" .py)"
  local logf="$LOG_DIR/python-${fname}.log"

  local -a tc_opts=()
  [[ "${VALGRIND_PY_TRACE_CHILDREN:-0}" == "1" ]] && tc_opts+=(--trace-children=yes)

  local -a py_tmo_cmd=()
  [[ "$py_timeout" != "0" ]] && py_tmo_cmd=(timeout --kill-after=30 "$py_timeout")

  log "Python ▶ $fname"
  (PYTHONMALLOC=malloc PYTHONDONTWRITEBYTECODE=1 \
    "${py_tmo_cmd[@]}" \
    valgrind --tool=memcheck --num-callers=20 --fair-sched=yes "${tc_opts[@]}" ${VALGRIND_EXTRA_OPTS:-} \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --suppressions="$SUPP_DIR/valgrind-python.supp" \
    --suppressions="$SUPP_DIR/valgrind-cpp.supp" \
    --xml=yes --xml-file="$xmldir/p.%p.${fname}.xml" \
    "$VENV_DIR/bin/python" -m pytest "$file" \
    -p no:cacheprovider -q >"$logf" 2>&1) || rc=$?
  printf '%s\t%s\n' "$rc" "$fname" >"$rdir/${fname}.rc"
  if ((rc == 0)); then
    log "Python ✓ $fname"
  elif ((rc == 124 || rc == 137)); then
    err "Python ✗ TIMEOUT/KILLED ${py_timeout}s: $fname"
  else
    err "Python ✗ FAILED (exit $rc): $fname"
    sed -n '1,30p' "$logf" >&2 || true
  fi
}

run_py() {
  setup_python
  mkdir -p "$LOG_DIR"

  local files=()
  if [[ -n "${VALGRIND_PYTEST_FILES:-}" ]]; then
    # shellcheck disable=SC2206
    files=(${VALGRIND_PYTEST_FILES})
  else
    # Expand the default directory to individual files for parallel execution.
    while IFS= read -r f; do
      files+=("$f")
    done < <(find "$REPO_ROOT/tests/python" -name "test_*.py" | sort)
  fi

  log "Python tests under Valgrind (${#files[@]} file(s) x $(test_jobs) parallel):"
  printf '         %s\n' "${files[@]}"

  local xmldir="$LOG_DIR/python-xml"
  rm -rf "$xmldir"
  mkdir -p "$xmldir"

  local py_timeout="${VALGRIND_PY_TIMEOUT:-3000}"
  local match="${VALGRIND_OURS_MATCH:-dftracer}"
  local max_jobs
  max_jobs="$(test_jobs)"

  if [[ "${VALGRIND_PY_TRACE_CHILDREN:-0}" == "1" ]]; then
    export DASK_DISTRIBUTED__COMM__TIMEOUTS__CONNECT=600s
    export DASK_DISTRIBUTED__COMM__TIMEOUTS__TCP=600s
    export DASK_DISTRIBUTED__DEPLOY__LOST_WORKER_TIMEOUT=600s
  fi

  local results_dir="$LOG_DIR/python-results"
  rm -rf "$results_dir"
  mkdir -p "$results_dir"

  local total=0
  for file in "${files[@]}"; do
    total=$((total + 1))
    while (($(jobs -rp | wc -l) >= max_jobs)); do wait -n || true; done
    run_py_one "$file" "$results_dir" "$xmldir" "$py_timeout" &
  done
  wait || true

  local failed=0 failed_names=()
  local f rc fname
  for f in "$results_dir"/*.rc; do
    [[ -e "$f" ]] || continue
    IFS=$'\t' read -r rc fname <"$f"
    ((rc == 0)) && continue
    failed=$((failed + 1))
    if ((rc == 124 || rc == 137)); then
      failed_names+=("$fname (TIMEOUT ${py_timeout}s)")
    else
      failed_names+=("$fname")
    fi
  done

  local nproc
  nproc=$(find "$xmldir" -name 'p.*.xml' | wc -l | tr -d ' ')
  # No XML means Valgrind never ran (bad invocation, early crash, permissions).
  if ((nproc == 0)); then
    err "Python Valgrind produced no XML reports in ${xmldir#$REPO_ROOT/}/ (Valgrind did not run?)"
    return 1
  fi
  log "Filtering Valgrind report to our code ('$match'); $nproc process report(s)"
  local filt_rc=0
  python3 "$SUPP_DIR/ours_only.py" --match "$match" "$xmldir"/p.*.xml || filt_rc=$?

  log "Python summary: $((total - failed))/$total file(s) passed under Valgrind"
  if ((failed > 0)); then
    err "Python Valgrind failures: ${failed_names[*]}"
    return 1
  fi
  if ((filt_rc != 0)); then
    err "Python summary: Valgrind findings attributable to our code (reports: ${xmldir#$REPO_ROOT/}/)"
    return 1
  fi
  log "Python summary: tests passed, no Valgrind findings in our code (reports: ${xmldir#$REPO_ROOT/}/)"
}

# Diagnostic: run ONE filtered test under Valgrind's gdbserver and, if it is
# still alive after VALGRIND_DUMP_AFTER seconds (i.e. hanging), dump every
# thread's backtrace via vgdb. Scope with VALGRIND_CTEST_FILTER=<one test>.
run_debug_hang() {
  configure_and_build_cpp
  mkdir -p "$LOG_DIR/cpp"
  local filt="${VALGRIND_CTEST_FILTER:-}"
  [[ -z "$filt" ]] && {
    err "debug-hang needs VALGRIND_CTEST_FILTER set to a single test name"
    return 2
  }
  command -v vgdb >/dev/null 2>&1 || {
    err "vgdb not found (ships with valgrind); cannot dump stacks"
    return 3
  }

  local line name exe wd
  line="$(
    python3 - "$BUILD_DIR" "$filt" <<'PY'
import json, os, re, subprocess, sys
build_dir, pat = sys.argv[1], sys.argv[2]
f = re.compile(pat)
out = subprocess.check_output(
    ["ctest", "--test-dir", build_dir, "--show-only=json-v1"])
d = json.loads(out)
for t in d.get("tests", []):
    c = t.get("command") or []
    if len(c) != 1:
        continue
    e = c[0]
    if not (os.path.isfile(e) and os.access(e, os.X_OK)):
        continue
    n = t.get("name", "")
    if not f.search(n):
        continue
    wd = next((p.get("value") for p in t.get("properties", [])
               if p.get("name") == "WORKING_DIRECTORY"), "")
    print(f"{n}\t{e}\t{wd}")
    break
PY
  )"
  [[ -z "$line" ]] && {
    err "no single-command test matched '$filt'"
    return 1
  }
  IFS=$'\t' read -r name exe wd <<<"$line"
  [[ -z "$wd" ]] && wd="$BUILD_DIR"

  local dump_after="${VALGRIND_DUMP_AFTER:-90}"
  local vglog="$LOG_DIR/cpp/debug-hang.valgrind.log"
  local stacks="$LOG_DIR/cpp/debug-hang.stacks.txt"
  log "debug-hang: $name (dump thread stacks after ${dump_after}s if still alive)"

  # exec so $! is the valgrind PID itself (not a wrapping subshell), which we
  # pass to vgdb --pid to disambiguate.
  (cd "$wd" && exec valgrind "${COMMON_OPTS[@]}" --vgdb=yes --vgdb-error=999999 \
    --leak-check=no --suppressions="$SUPP_DIR/valgrind-cpp.supp" \
    "$exe" >"$vglog" 2>&1) &
  local vgpid=$!

  local waited=0
  while ((waited < dump_after)) && kill -0 "$vgpid" 2>/dev/null; do
    sleep 2
    waited=$((waited + 2))
  done

  if ! kill -0 "$vgpid" 2>/dev/null; then
    wait "$vgpid" || true
    log "Completed within ${dump_after}s (no hang). Valgrind log: ${vglog#$REPO_ROOT/}"
    return 0
  fi

  err "Still alive after ${dump_after}s -> dumping thread states via vgdb (pid $vgpid)"
  # v.info scheduler prints the state and stack of every thread Valgrind tracks.
  # Driven straight through vgdb (no gdb) to avoid gdb<->gdbserver version skew.
  {
    echo "===== v.info scheduler ====="
    vgdb --pid="$vgpid" v.info scheduler 2>&1 || true
    echo
    echo "===== v.info exectxt ====="
    vgdb --pid="$vgpid" v.info exectxt 2>&1 || true
  } | tee "$stacks"

  kill -9 "$vgpid" 2>/dev/null || true
  pkill -9 -f "memcheck.*${exe##*/}" 2>/dev/null || true
  wait "$vgpid" 2>/dev/null || true
  err "Thread stacks written to ${stacks#$REPO_ROOT/}"
  return 1
}

main() {
  local cmd="${1:-all}"
  case "$cmd" in
  cpp) run_cpp ;;
  py) run_py ;;
  mpi) run_mpi ;;
  debug-hang) run_debug_hang ;;
  all)
    run_cpp
    run_py
    ;;
  *)
    err "Unknown subcommand: $cmd (expected cpp|py|mpi|all)"
    exit 2
    ;;
  esac
  log "Done. Full logs in ${LOG_DIR#$REPO_ROOT/}/"
}

main "$@"
