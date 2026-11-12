#!/bin/bash
# SPDX-License-Identifier: MIT
# ci.yml -> test, python half. Its own container so it runs beside the C++ and
# valgrind phases; it builds into its own venv, so the trees do not collide.
set -eo pipefail
cd "$CI_PROJECT_DIR"
source .gitlab/ci/toolchain.sh
make test-py
