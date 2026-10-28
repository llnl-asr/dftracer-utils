# Sourced by every phase script. Runs inside a podman container, no LC modules.
set -eo pipefail

# APT::Sandbox::User=root: rootless podman has no mapped _apt uid, so apt's
# privilege drop fails with "setgroups (22: Invalid argument)".
export DEBIAN_FRONTEND=noninteractive
echo ">>> installing toolchain packages (a few hundred MB, quiet until done)"
apt-get -o APT::Sandbox::User=root update -qq
# OpenMPI, matching the github valgrind jobs: valgrind-mpi.supp is written
# against OpenMPI/PMIx, and under mpich every rank came up as a singleton.
apt-get -o APT::Sandbox::User=root install -y -qq \
  build-essential cmake ninja-build ccache pkg-config git \
  curl ca-certificates \
  zlib1g-dev libzstd-dev libsqlite3-dev libopenmpi-dev openmpi-bin \
  python3 python3-pip python3-venv python3-dev "$@"
echo ">>> toolchain ready"
cmake --version

# Every phase container sees the whole node, so five of them each sized to the
# full machine oversubscribe it. Split the node PHASE_COUNT ways, using the
# same cpu/memory shape as build_jobs()/test_jobs() in tests/valgrind/run.sh.
if [ -n "${PHASE_COUNT:-}" ] && [ "${PHASE_COUNT}" -gt 1 ] 2>/dev/null; then
  _cpus=$(nproc 2>/dev/null || echo 4)
  _mem_gb=$(awk '/MemTotal/{printf "%d", $2/1024/1024}' /proc/meminfo 2>/dev/null || echo 0)
  _share() { # $1 = memory divisor
    _b=$_cpus
    if [ "$_mem_gb" -gt 0 ] && [ $((_mem_gb / $1)) -lt "$_cpus" ]; then _b=$((_mem_gb / $1)); fi
    _j=$((_b / PHASE_COUNT))
    [ "$_j" -lt 1 ] && _j=1
    echo "$_j"
  }
  CMAKE_BUILD_PARALLEL_LEVEL=$(_share 2)
  VALGRIND_BUILD_JOBS=$CMAKE_BUILD_PARALLEL_LEVEL
  VALGRIND_TEST_JOBS=$(_share 3)
  export CMAKE_BUILD_PARALLEL_LEVEL VALGRIND_BUILD_JOBS VALGRIND_TEST_JOBS
  echo "node share (${PHASE_COUNT} phases, ${_cpus} cpus, ${_mem_gb}G): build=${CMAKE_BUILD_PARALLEL_LEVEL} test=${VALGRIND_TEST_JOBS}"
fi
