#!/bin/bash
# SPDX-License-Identifier: MIT
# Usage: run-in-podman.sh <image> <phase-script> [args...]
# Runs a CI phase inside podman on the allocated node, mirroring the images the
# GitHub workflows use.
set -ex

IMAGE=$1
shift

# Rootless podman needs node-local storage; overlayfs does not work on NFS.
PODMAN_STORE=/var/tmp/$USER/podman-root
PODMAN_RUNROOT=/var/tmp/$USER/podman-run
mkdir -p "$PODMAN_STORE" "$PODMAN_RUNROOT"

# CMakeLists sets ccache as the compiler launcher, but without CCACHE_DIR each
# container caches into its own $HOME and --rm discards it: every phase, and
# every pipeline, compiled from cold. Node-local so it survives both.
# BASEDIR+NOHASHDIR are what let the per-phase build dirs share entries.
CCACHE_HOST=/var/tmp/$USER/dftracer-utils-ccache
mkdir -p "$CCACHE_HOST"

# Mounted at the same path so --find-links reads the same inside and out.
DFTRACER_DIST="${DFTRACER_DIST:-/usr/workspace/dldl/dftracer/distributions}"
DIST_MOUNT=()
if [ -d "$DFTRACER_DIST" ]; then
  DIST_MOUNT=(-v "$DFTRACER_DIST:$DFTRACER_DIST:ro")
fi

# --user 0:0: container root maps to the host user, so the bind-mounted
# checkout stays readable for images with a non-root USER.
podman --root "$PODMAN_STORE" --runroot "$PODMAN_RUNROOT" run --rm \
  --user 0:0 \
  -v "$PWD:/ws" -w /ws \
  -v "$CCACHE_HOST:/ccache" \
  "${DIST_MOUNT[@]}" \
  -e CI_PROJECT_DIR=/ws \
  -e DFTRACER_DIST \
  -e CMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -e PHASE_COUNT \
  -e CCACHE_DIR=/ccache \
  -e CCACHE_BASEDIR=/ws \
  -e CCACHE_COMPILERCHECK=content \
  -e CCACHE_NOHASHDIR=true \
  -e CCACHE_MAXSIZE=5G \
  -e CCACHE_SLOPPINESS=time_macros,include_file_mtime,include_file_ctime,pch_defines,system_headers,locale \
  -e VALGRIND_SHARD \
  "$IMAGE" bash "$@"
