#!/bin/bash
# SPDX-License-Identifier: MIT
# Usage: parallel-phases.sh <phase>...
# Runs each phase in its own podman container, concurrently inside the one
# allocation: GitHub gets a runner per job, here the container is the unit.
# Output streams live, tagged per phase, so a hung phase is visible while the
# others are still running.
set -eo pipefail
JOBID=$(flux jobs -n --name="$ALLOC_NAME" -o "{id}" | head -1)
test -n "$JOBID"

pids=()
for phase in "$@"; do
  (
    flux proxy "$JOBID" flux run -N 1 --env=PHASE_COUNT="$#" bash .gitlab/ci/run-in-podman.sh \
      docker.io/library/ubuntu:24.04 ".gitlab/ci/${phase}.sh" 2>&1 |
      stdbuf -oL sed -u "s/^/[${phase}] /"
  ) &
  pids+=("$!:$phase")
done

rc=0
for entry in "${pids[@]}"; do
  if wait "${entry%%:*}"; then
    echo "[${entry##*:}] passed"
  else
    rc=1
    echo "[${entry##*:}] FAILED"
  fi
done
exit $rc
