#!/usr/bin/env bash
# Regenerates docs/doxygen/xml, the source of truth for
# scripts/check_op_parity_doxygen.py. Output is gitignored and cheap to
# rebuild (a couple of seconds), so this is not wired into the CMake build -
# run it by hand before that script if docs/doxygen/xml is missing or stale
# relative to include/.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

command -v doxygen >/dev/null 2>&1 || {
    echo "doxygen not found on PATH" >&2
    exit 1
}

cd "${REPO_ROOT}/docs"
doxygen Doxyfile
