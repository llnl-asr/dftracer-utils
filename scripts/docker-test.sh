#!/usr/bin/env bash
# Build and test dftracer-utils inside a Linux toolchain container.
#
# Usage:
#   scripts/docker-test.sh [variant] [scope] [options]
#     variant : gcc12 | latest                  (default: gcc12)
#     scope   : cpp | python | both | valgrind  (default: cpp)
#               (valgrind reuses tests/valgrind/run.sh cpp under the image)
#
# Options (override the corresponding env var):
#   --variant <v>      gcc12 | latest
#   --scope <s>        cpp | python | both
#   --monitor <mode>   set DFTRACER_UTILS_MONITOR (e.g. tree, 1, trace)
#   --mpi <on|off>     build/run the MPI C++ tests       (default: on)
#   --no-mpi           shorthand for --mpi off
#   -j, --jobs <n>     build parallelism                 (default: 4)
#   -h, --help         show this help
#
# Env equivalents: DFTRACER_UTILS_MONITOR, MPI, JOBS (flags take precedence).
#
# Examples:
#   scripts/docker-test.sh gcc12
#   scripts/docker-test.sh latest both --monitor tree -j 8
#   scripts/docker-test.sh --variant gcc12 --scope python
#   MPI=off scripts/docker-test.sh gcc12        # env form
#
# The build tree persists in a bind-mounted build/build-docker-<variant>, so
# only the first run pays the full dependency compile; later runs are
# incremental. The toolchain is baked into the image (no apt per run).
set -euo pipefail

usage() { sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; }

# Defaults (env overrides built-in; flags override env).
variant="gcc12"
scope="cpp"
monitor="${DFTRACER_UTILS_MONITOR:-}"
mpi="${MPI:-ON}"
jobs="${JOBS:-4}"

positional=()
while [ $# -gt 0 ]; do
    case "$1" in
        --variant) variant="$2"; shift 2 ;;
        --scope) scope="$2"; shift 2 ;;
        --monitor) monitor="$2"; shift 2 ;;
        --mpi) mpi="$2"; shift 2 ;;
        --no-mpi) mpi="OFF"; shift ;;
        -j | --jobs) jobs="$2"; shift 2 ;;
        -h | --help)
            usage
            exit 0
            ;;
        --*)
            echo "unknown option: $1" >&2
            usage
            exit 1
            ;;
        *)
            positional+=("$1")
            shift
            ;;
    esac
done
[ "${#positional[@]}" -ge 1 ] && variant="${positional[0]}"
[ "${#positional[@]}" -ge 2 ] && scope="${positional[1]}"

# Normalize.
mpi="$(printf '%s' "$mpi" | tr '[:lower:]' '[:upper:]')"
[ "$mpi" = "1" ] && mpi="ON"
[ "$mpi" = "0" ] && mpi="OFF"

case "$variant" in
    gcc12) image="dftracer-gcc12" dockerfile="docker/gcc12.Dockerfile" ;;
    latest) image="dftracer-latest" dockerfile="docker/gcc-latest.Dockerfile" ;;
    *)
        echo "invalid variant: $variant (gcc12|latest)" >&2
        exit 1
        ;;
esac
case "$scope" in
    cpp | python | both | valgrind) ;;
    *)
        echo "invalid scope: $scope (cpp|python|both|valgrind)" >&2
        exit 1
        ;;
esac

root="$(cd "$(dirname "$0")/.." && pwd)"
builddir="build/build-docker-${variant}"

docker build -t "$image" -f "$root/${dockerfile}" "$root"

docker run --rm \
    -v "$root":/work -w /work \
    -e "DFTRACER_UTILS_MONITOR=${monitor}" \
    -e "DFTRACER_UTILS_THREADS=${DFTRACER_UTILS_THREADS:-}" \
    "$image" bash -c "
        set -e
        scope='${scope}'

        if [ \"\$scope\" = cpp ] || [ \"\$scope\" = both ]; then
            cmake -S . -B '${builddir}' -G Ninja \
                -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                -DDFTRACER_UTILS_TESTS=ON \
                -DDFTRACER_UTILS_ENABLE_MPI=${mpi} \
                -DDFTRACER_UTILS_BUILD_PYTHON=OFF \
                -DDFTRACER_UTILS_COVERAGE=OFF \
                -DCPM_SOURCE_CACHE=/work/.cpmsource
            cmake --build '${builddir}' -j ${jobs}
            # Skip install/discovery tests: they need an installed package plus
            # pkg-config/CMake config on the system (FIXTURES_REQUIRED
            # installed_package), which the container does not set up.
            ( cd '${builddir}' && ctest --output-on-failure -E 'discovery|test_target_' )
        fi

        if [ \"\$scope\" = python ] || [ \"\$scope\" = both ]; then
            # Editable install builds the extension via scikit-build-core and
            # pulls dev deps. The venv lives in the persistent build dir, so it
            # is reused on later runs.
            venv='${builddir}/.venv'
            python3 -m venv \"\$venv\"
            \"\$venv/bin/pip\" install --quiet --upgrade pip
            \"\$venv/bin/pip\" install --quiet -e '.[dev]'
            \"\$venv/bin/pytest\" tests/python
        fi

        if [ \"\$scope\" = valgrind ]; then
            # Reuse the existing valgrind harness (suppressions, memcheck wrap,
            # VALGRIND_MODE) but in this image and a persistent per-variant dir.
            VALGRIND_BUILD_DIR=/work/'${builddir}'-valgrind \
            VALGRIND_BUILD_JOBS=${jobs} \
                ./tests/valgrind/run.sh cpp
        fi
    "
