#!/usr/bin/env bash
#
# Build and install RocksDB once so CI jobs can reuse the tree instead of
# recompiling ~350 translation units per build.
#
# The install prefix must be identical across every job that consumes it: the
# precompiled header embeds <rocksdb/*.h>, so a different -I path invalidates
# ccache for the whole project.
#
# The option list below must stay in sync with need_rocksdb() in
# cmake/modules/Dependencies.cmake. Any drift changes the ABI silently.

set -euo pipefail

VERSION="${ROCKSDB_VERSION:-10.10.1}"
PREFIX="${ROCKSDB_PREFIX:-/opt/dftracer-deps/rocksdb-${VERSION}}"
SRC_CACHE="${ROCKSDB_SRC_CACHE:-${TMPDIR:-/tmp}/rocksdb-src}"
BUILD_DIR="${ROCKSDB_BUILD_DIR:-${TMPDIR:-/tmp}/rocksdb-build}"
JOBS="${JOBS:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu) )}"

# Match Dependencies.cmake. WITH_ZLIB is unconditional there; LZ4/ZSTD track
# DFTRACER_UTILS_ENABLE_LZ4 / _ZSTD, whose defaults are OFF / ON.
WITH_LZ4="${WITH_LZ4:-OFF}"
WITH_ZSTD="${WITH_ZSTD:-ON}"

is_complete() {
	{ [ -f "${PREFIX}/lib/cmake/rocksdb/RocksDBConfig.cmake" ] ||
		[ -f "${PREFIX}/lib64/cmake/rocksdb/RocksDBConfig.cmake" ]; } &&
		[ -x "${PREFIX}/bin/ldb" ] && [ -x "${PREFIX}/bin/sst_dump" ]
}

if is_complete; then
	echo "RocksDB ${VERSION} already installed at ${PREFIX}"
	exit 0
fi

if [ ! -d "${SRC_CACHE}/rocksdb-${VERSION}" ]; then
	mkdir -p "${SRC_CACHE}"
	echo "Fetching RocksDB ${VERSION}"
	curl -fsSL "https://github.com/facebook/rocksdb/archive/refs/tags/v${VERSION}.tar.gz" |
		tar -xz -C "${SRC_CACHE}"
fi

# RelWithDebInfo, not Release: the Valgrind jobs need DWARF in RocksDB frames,
# and -O2 keeps memcheck's inlining behaviour as it was.
rm -rf "${BUILD_DIR}"
cmake -S "${SRC_CACHE}/rocksdb-${VERSION}" -B "${BUILD_DIR}" -G Ninja \
	-DCMAKE_BUILD_TYPE="${ROCKSDB_BUILD_TYPE:-RelWithDebInfo}" \
	-DCMAKE_INSTALL_PREFIX="${PREFIX}" \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DROCKSDB_BUILD_SHARED=OFF \
	-DPORTABLE=1 \
	-DWITH_TESTS=OFF \
	-DWITH_TOOLS=OFF \
	-DWITH_CORE_TOOLS=ON \
	-DWITH_TRACE_TOOLS=OFF \
	-DWITH_BENCHMARK_TOOLS=OFF \
	-DWITH_GFLAGS=OFF \
	-DWITH_SNAPPY=OFF \
	-DWITH_LZ4="${WITH_LZ4}" \
	-DWITH_ZLIB=ON \
	-DWITH_ZSTD="${WITH_ZSTD}" \
	-DWITH_BZ2=OFF \
	-DUSE_RTTI=ON \
	-DFAIL_ON_WARNINGS=OFF

cmake --build "${BUILD_DIR}" -j "${JOBS}"
cmake --install "${BUILD_DIR}"

# RocksDB's tools/CMakeLists.txt has no install() rule, but Dependencies.cmake
# ships ldb and sst_dump, so lift them out of the build tree.
mkdir -p "${PREFIX}/bin"
for tool in ldb sst_dump; do
	found="$(find "${BUILD_DIR}" -maxdepth 3 -type f -name "${tool}" -perm -u+x -print -quit)"
	if [ -n "${found}" ]; then
		install -m 0755 "${found}" "${PREFIX}/bin/${tool}"
	else
		echo "error: ${tool} not found under ${BUILD_DIR}" >&2
		exit 1
	fi
done

echo "RocksDB ${VERSION} installed to ${PREFIX}"
