#!/bin/bash
# SPDX-License-Identifier: MIT
# ci.yml -> valgrind-python.
set -eo pipefail
cd "$CI_PROJECT_DIR"
source .gitlab/ci/toolchain.sh valgrind
VALGRIND_BUILD_DIR=$PWD/build/build-valgrind-py make valgrind-py
