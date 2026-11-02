#!/bin/bash
# SPDX-License-Identifier: MIT
# ci.yml -> valgrind-mpi. Already has a build dir of its own.
set -eo pipefail
cd "$CI_PROJECT_DIR"
source .gitlab/ci/toolchain.sh valgrind
make valgrind-mpi
