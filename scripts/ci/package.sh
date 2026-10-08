#!/usr/bin/env bash
# Build the C and C++ install layout (bin, lib, include, CMake package files)
# with Release dependencies and pack it.
#
#   package.sh <deps directory> <output tarball>
#
# The deps directory holds the installs that prebuild_wheel_deps.sh or
# build_rocksdb.sh made, so the build links the same RocksDB as the wheels.

set -euo pipefail

deps="$(cd "${1:?deps directory}" && pwd)"
out="$(mkdir -p "$(dirname "${2:?output tarball}")" && cd "$(dirname "$2")" && pwd)/$(basename "$2")"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${root}/build/package"
prefix="${build}/prefix"

export DFTRACER_UTILS_DEPS_PREFIX="${deps}"
export DFTRACER_UTILS_ROCKSDB_PREFIX="${deps}/rocksdb-$("${root}/scripts/ci/rocksdb_version.sh")"

launcher=()
if command -v sccache >/dev/null 2>&1; then
	launcher=(-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache
		-DCMAKE_ASM_COMPILER_LAUNCHER=sccache)
fi

rm -rf "${prefix}"
cmake --preset release -B "${build}" \
	-DDFTRACER_UTILS_LOCAL_PACKAGES=OFF \
	-DDFTRACER_UTILS_DEPS_BUILD_TYPE=Release \
	-DDFTRACER_UTILS_TESTS=OFF \
	-DCMAKE_INSTALL_PREFIX="${prefix}" \
	${launcher[@]+"${launcher[@]}"}
cmake --build "${build}"
cmake --install "${build}"
tar -czf "${out}" -C "${prefix}" .
echo "packed ${out} ($(du -h "${out}" | cut -f1))"
