#!/bin/bash
# SPDX-License-Identifier: MIT
# ci.yml -> test, C++ half. ctest presets already run jobs: 0 (all cores).
set -eo pipefail
cd "$CI_PROJECT_DIR"
source .gitlab/ci/toolchain.sh
# PEP 668: throwaway container, so installing into it directly is fine.
pip3 install --quiet --break-system-packages pytest
# The package-discovery tests only look in <prefix>/lib and lib64, not the
# multiarch dir. Pinning CMAKE_INSTALL_LIBDIR does not hold: a subproject
# re-includes GNUInstallDirs and FORCEs it back.
cmake --preset tests
cmake --build --preset tests
LIBDIR=$(sed -n "s/^CMAKE_INSTALL_LIBDIR:PATH=//p" build/build-tests/CMakeCache.txt)
PFX=$PWD/build/build-tests/test_install
if [ "$LIBDIR" != "lib" ] && [ "$LIBDIR" != "lib64" ]; then
  mkdir -p "$PFX/$LIBDIR/cmake" "$PFX/$LIBDIR/pkgconfig" "$PFX/lib"
  ln -sfn "$PFX/$LIBDIR/cmake" "$PFX/lib/cmake"
  ln -sfn "$PFX/$LIBDIR/pkgconfig" "$PFX/lib/pkgconfig"
fi
ctest --preset tests
