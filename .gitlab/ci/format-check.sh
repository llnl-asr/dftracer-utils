#!/bin/bash
# SPDX-License-Identifier: MIT
# format-check.yaml -> clang-format 19 + ruff.
set -eo pipefail
cd "$CI_PROJECT_DIR"
pip install --quiet --upgrade pip
pip install --quiet "clang-format==19.*" ruff
./scripts/formatting/check-formatting.sh "$(command -v clang-format)"
ruff check python/ tests/python/
ruff format --check python/ tests/python/
