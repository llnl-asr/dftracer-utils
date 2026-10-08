#!/bin/sh
# Build the RocksDB install that the Linux wheels use, inside the manylinux
# container, so it matches the compiler and glibc the wheels build with.
#
#   prebuild_wheel_deps.sh <deps directory>
#
# build_rocksdb.sh skips the build when the prefix is complete, so a restored
# install costs nothing.

set -eu

deps="${1:?deps directory}"
here="$(cd "$(dirname "$0")" && pwd)"

dnf install -y zlib-devel >/dev/null 2>&1 ||
	yum install -y zlib-devel >/dev/null 2>&1 || true
/opt/python/cp312-cp312/bin/python -m pip install --quiet cmake ninja
export PATH="/opt/python/cp312-cp312/bin:${PATH}"

# Release, not the script's RelWithDebInfo: the wheels link it statically and
# should not carry its debug info.
export DEPS_BUILD_TYPE=Release ZSTD_STATIC=1
export ROCKSDB_PREFIX="${deps}/rocksdb-$("${here}/rocksdb_version.sh")"
bash "${here}/build_rocksdb.sh"
