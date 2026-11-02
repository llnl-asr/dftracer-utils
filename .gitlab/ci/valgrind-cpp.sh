#!/bin/bash
# SPDX-License-Identifier: MIT
# ci.yml -> valgrind-cpp. Its own build dir: cpp and py both default to
# build/build-valgrind, so sharing it would race two builds.
set -eo pipefail
cd "$CI_PROJECT_DIR"
source .gitlab/ci/toolchain.sh valgrind
VALGRIND_BUILD_DIR=$PWD/build/build-valgrind-cpp make valgrind-cpp
