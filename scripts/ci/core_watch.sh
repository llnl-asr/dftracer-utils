#!/usr/bin/env bash
# Print a backtrace for each core dump as soon as it appears, so a CI job shows
# why a test crashed while the rest of the tests still run.
#
# Usage: scripts/ci/core_watch.sh <core dir> <backtrace dir>
# Run it in the background next to the test command and kill it at the end.
# A core is read once its size stops changing; its backtrace goes to
# <backtrace dir>/<core name>.txt, and a core that already has one is skipped,
# so the end-of-job step only handles cores this watcher missed.
set -uo pipefail

core_dir="$1"
out_dir="$2"
mkdir -p "$out_dir"

backtrace() {
  local core="$1" out="$2"
  if [[ "$(uname)" == "Darwin" ]]; then
    lldb --batch -c "$core" -o "thread backtrace all" -o "quit" >"$out" 2>&1
    return
  fi
  if ! command -v gdb >/dev/null 2>&1; then
    sudo apt-get install -y gdb >/dev/null 2>&1 || true
  fi
  # The core name is core.<pid>.<exe path with '/' as '!'> (core_pattern %p.%E).
  local enc exe
  enc="$(basename "$core")"
  enc="${enc#core.*.}"
  exe="$(printf '%s' "$enc" | tr '!' '/')"
  gdb -q -batch -ex "set pagination off" -ex "thread apply all bt full" \
    "$exe" "$core" >"$out" 2>&1
}

while true; do
  shopt -s nullglob
  for core in "$core_dir"/core.*; do
    out="$out_dir/$(basename "$core").txt"
    [[ -e "$out" ]] && continue
    size="$(wc -c <"$core" 2>/dev/null || echo 0)"
    sleep 2
    [[ "$size" == "$(wc -c <"$core" 2>/dev/null || echo 0)" ]] || continue
    # Write aside first: a watcher killed mid-run must leave no partial file,
    # or the end-of-job step would skip that core.
    backtrace "$core" "$out.part"
    mv "$out.part" "$out"
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
      printf '::error title=Crash::core dump %s\n' "$(basename "$core")"
      printf '::group::backtrace of %s\n' "$(basename "$core")"
    fi
    printf '===== backtrace of %s =====\n' "$core"
    tail -200 "$out"
    [[ -n "${GITHUB_ACTIONS:-}" ]] && printf '::endgroup::\n'
  done
  sleep 3
done
