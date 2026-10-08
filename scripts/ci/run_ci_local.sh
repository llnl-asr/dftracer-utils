#!/usr/bin/env bash
#
# Reproduce the ci.yml jobs in a container (podman/docker) so they can be run on
# a Linux cluster. Builds a small ubuntu image with CI's apt deps, bind-mounts
# the repo at /work, and runs the selected jobs (the same make targets CI uses).
#
# Usage:
#   scripts/ci/run_ci_local.sh [options] <job> [<job> ...]
#
# Jobs: test test-py lint typecheck format valgrind-cpp valgrind-mpi valgrind-py
#       all = test test-py lint typecheck format (no Valgrind)
#
# Options:
#   --engine E           podman|docker|auto (default: auto)
#   --image REF          base image (default: ubuntu:22.04)
#   --storage-root DIR   podman graphroot; must be LOCAL, not NFS/Lustre
#                        (default: node-local /tmp)
#   --storage-driver D   podman storage driver (e.g. vfs) if overlay fails
#   --rebuild            rebuild the CI image (refresh apt deps)
#   --keep               keep the container after the run
#   --shell              interactive shell in the CI image
#   --shard I/N          run only Valgrind shard I of N (CI uses N=4)
#   --shards N           run Valgrind shards 1..N sequentially (CI uses N=4)
#   --jobs N             Valgrind concurrency (VALGRIND_TEST_JOBS); use 2 for CI
#   --filter REGEX       only Valgrind tests whose ctest name matches REGEX
#   --timeout S          per-test Valgrind timeout, seconds (CI default: 2400)
#   --emit-durations     regenerate tests/valgrind/durations.tsv from this run
#   --run-ty             also run `ty` in the test-py job
#   -h, --help           show this help
#
# Examples:
#   scripts/ci/run_ci_local.sh test test-py
#   scripts/ci/run_ci_local.sh --shards 4 --jobs 2 valgrind-cpp
#   scripts/ci/run_ci_local.sh --jobs 1 --filter 'test_view_scan|test_dftracer_server' \
#     --emit-durations valgrind-cpp
#   scripts/ci/run_ci_local.sh --shell

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

ENGINE="auto"
BASE_IMAGE="ubuntu:22.04"
CI_IMAGE="dftracer-utils-ci:local"
REBUILD=0
KEEP=0
SHELL_MODE=0
IN_CONTAINER=0
STORAGE_ROOT=""
STORAGE_DRIVER=""

SHARD=""
SHARDS=""
JOBS=""
FILTER=""
TIMEOUT=""
EMIT_DURATIONS=0
RUN_TY=0
JOB_LIST=()

log() { echo "[run_ci_local] $*"; }
usage() { sed -n '2,37p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --engine) ENGINE="$2"; shift 2 ;;
    --image) BASE_IMAGE="$2"; shift 2 ;;
    --storage-root) STORAGE_ROOT="$2"; shift 2 ;;
    --storage-driver) STORAGE_DRIVER="$2"; shift 2 ;;
    --rebuild) REBUILD=1; shift ;;
    --keep) KEEP=1; shift ;;
    --shell) SHELL_MODE=1; shift ;;
    --shard) SHARD="$2"; shift 2 ;;
    --shards) SHARDS="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --filter) FILTER="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --emit-durations) EMIT_DURATIONS=1; shift ;;
    --run-ty) RUN_TY=1; shift ;;
    --in-container) IN_CONTAINER=1; shift ;;
    -h|--help) usage; exit 0 ;;
    all) JOB_LIST+=(test test-py lint typecheck format); shift ;;
    test|test-py|lint|typecheck|format|valgrind-cpp|valgrind-mpi|valgrind-py)
      JOB_LIST+=("$1"); shift ;;
    *) echo "Unknown job or option: $1" >&2; usage; exit 1 ;;
  esac
done

# Valgrind is memory-bandwidth-bound, so the repo's default concurrency
# (MemTotal_GB/3) melts a fat cluster node into per-test timeouts. Default to 2,
# matching CI's small runner; override with --jobs.
[ -z "${JOBS}" ] && JOBS=2

# --- In-container: run the selected jobs (the CI make targets) ---
if [ "${IN_CONTAINER}" -eq 1 ]; then
  cd "${REPO_ROOT}"
  export CCACHE_DIR="${REPO_ROOT}/.ccache"
  export CPM_SOURCE_CACHE="${REPO_ROOT}/.cpmsource"
  export OMPI_ALLOW_RUN_AS_ROOT=1 OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1

  valgrind_env=()
  [ -n "${JOBS}" ] && valgrind_env+=("VALGRIND_TEST_JOBS=${JOBS}")
  [ -n "${FILTER}" ] && valgrind_env+=("VALGRIND_CTEST_FILTER=${FILTER}")
  [ -n "${TIMEOUT}" ] && valgrind_env+=("VALGRIND_TEST_TIMEOUT=${TIMEOUT}")
  [ "${EMIT_DURATIONS}" -eq 1 ] && valgrind_env+=("VALGRIND_EMIT_DURATIONS=1")

  run_valgrind_target() {
    local target="$1"
    if [ -n "${SHARDS}" ]; then
      local i
      for ((i = 1; i <= SHARDS; i++)); do
        log "${target}: shard ${i}/${SHARDS}"
        env ${valgrind_env[@]+"${valgrind_env[@]}"} \
          VALGRIND_SHARD="${i}/${SHARDS}" make "${target}"
      done
    elif [ -n "${SHARD}" ]; then
      env ${valgrind_env[@]+"${valgrind_env[@]}"} \
        VALGRIND_SHARD="${SHARD}" make "${target}"
    else
      env ${valgrind_env[@]+"${valgrind_env[@]}"} make "${target}"
    fi
  }

  run_job() {
    local job="$1"
    echo ""
    log "JOB: ${job}"
    case "${job}" in
      test)
        cmake --preset tests
        cmake --build --preset tests
        ctest --preset tests --output-on-failure ;;
      test-py)
        if [ "${RUN_TY}" -eq 1 ]; then make test-py RUN_TY=1; else make test-py; fi ;;
      lint) make lint ;;
      typecheck) make typecheck ;;
      format) make check-format ;;
      valgrind-cpp) run_valgrind_target valgrind-cpp ;;
      valgrind-mpi) run_valgrind_target valgrind-mpi ;;
      valgrind-py) run_valgrind_target valgrind-py ;;
      *) echo "Unknown job: ${job}" >&2; exit 1 ;;
    esac
  }

  status=0
  results=()
  for job in "${JOB_LIST[@]}"; do
    if run_job "${job}"; then results+=("PASS  ${job}");
    else status=1; results+=("FAIL  ${job}"); log "FAILED: ${job}"; fi
  done
  echo ""
  log "Summary"
  for r in "${results[@]}"; do echo "  ${r}"; done
  exit "${status}"
fi

# --- Host: pick engine, ensure image, run container ---
if [ "${#JOB_LIST[@]}" -eq 0 ] && [ "${SHELL_MODE}" -eq 0 ]; then
  echo "ERROR: no job selected" >&2; usage; exit 1
fi

if [ "${ENGINE}" = "auto" ]; then
  if command -v podman >/dev/null 2>&1; then ENGINE=podman
  elif command -v docker >/dev/null 2>&1; then ENGINE=docker
  else echo "ERROR: neither podman nor docker found on PATH" >&2; exit 1; fi
fi
command -v "${ENGINE}" >/dev/null 2>&1 || {
  echo "ERROR: engine '${ENGINE}' not found on PATH" >&2; exit 1; }
log "engine: ${ENGINE}"

# podman's default storage is often on NFS/Lustre (no xattr -> lsetxattr fails);
# keep graphroot/runroot on a local filesystem.
engine_opts=()
if [ "${ENGINE}" = "podman" ]; then
  root="${STORAGE_ROOT:-${TMPDIR:-/tmp}/dftracer-ci-podman-$(id -u)}"
  mkdir -p "${root}" "${root}-run"
  engine_opts+=(--root "${root}" --runroot "${root}-run")
  [ -n "${STORAGE_DRIVER}" ] && engine_opts+=(--storage-driver "${STORAGE_DRIVER}")
  log "podman storage root: ${root}"
elif [ -n "${STORAGE_DRIVER}" ]; then
  engine_opts+=(--storage-driver "${STORAGE_DRIVER}")
fi
engine() { "${ENGINE}" ${engine_opts[@]+"${engine_opts[@]}"} "$@"; }

# apt set = union of every job's deps in ci.yml; uv provides uvx for lint.
ensure_image() {
  if [ "${REBUILD}" -eq 0 ] && \
     engine image inspect "${CI_IMAGE}" >/dev/null 2>&1; then
    log "image ${CI_IMAGE} present (use --rebuild to refresh)"
    return
  fi
  log "building ${CI_IMAGE} from ${BASE_IMAGE} ..."
  engine build -t "${CI_IMAGE}" - <<EOF
FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
# APT::Sandbox::User=root: rootless podman has no uid map for apt's _apt user
# (65534), so its privilege drop fails with "setgroups Invalid argument".
RUN A='-o APT::Sandbox::User=root' \
 && apt-get \$A update \
 && apt-get \$A install -y --no-install-recommends \
      build-essential cmake ninja-build ccache valgrind gdb git \
      zlib1g-dev libzstd-dev libsqlite3-dev pkg-config ca-certificates \
      libopenmpi-dev openmpi-bin python3 python3-dev python3-venv python3-pip \
 && (pip3 install --no-cache-dir uv \
     || pip3 install --no-cache-dir --break-system-packages uv) \
 && rm -rf /var/lib/apt/lists/*
EOF
}

ensure_image

CONTAINER_NAME="dftracer-utils-ci-local-$$"
run_flags=(--name "${CONTAINER_NAME}" -v "${REPO_ROOT}:/work" -w /work)
[ "${KEEP}" -eq 0 ] && run_flags=(--rm "${run_flags[@]}")
cleanup() {
  if [ "${KEEP}" -eq 1 ]; then
    log "container '${CONTAINER_NAME}' kept; remove: ${ENGINE} rm -f ${CONTAINER_NAME}"
  fi
}
trap cleanup EXIT

if [ "${SHELL_MODE}" -eq 1 ]; then
  exec "${ENGINE}" ${engine_opts[@]+"${engine_opts[@]}"} run -it \
    "${run_flags[@]}" "${CI_IMAGE}" bash
fi

inner=(/work/scripts/ci/run_ci_local.sh --in-container)
[ -n "${SHARD}" ] && inner+=(--shard "${SHARD}")
[ -n "${SHARDS}" ] && inner+=(--shards "${SHARDS}")
[ -n "${JOBS}" ] && inner+=(--jobs "${JOBS}")
[ -n "${FILTER}" ] && inner+=(--filter "${FILTER}")
[ -n "${TIMEOUT}" ] && inner+=(--timeout "${TIMEOUT}")
[ "${EMIT_DURATIONS}" -eq 1 ] && inner+=(--emit-durations)
[ "${RUN_TY}" -eq 1 ] && inner+=(--run-ty)
inner+=("${JOB_LIST[@]}")

log "running: ${JOB_LIST[*]}"
engine run "${run_flags[@]}" "${CI_IMAGE}" bash "${inner[@]}"
