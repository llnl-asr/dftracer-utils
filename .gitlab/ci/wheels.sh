#!/bin/bash
# SPDX-License-Identifier: MIT
# ci.yml -> wheel. Runs on the node, not in a container: cibuildwheel drives
# podman itself. The CIBW_* settings mirror the wheel job's env.
set -eo pipefail
cd "$CI_PROJECT_DIR"
source /etc/profile.d/z00_lmod.sh 2>/dev/null || true

# The rootless store defaults to $HOME, which on NFS cannot hold image-layer
# xattrs. A PATH wrapper is needed: podman 4.9 ignores CONTAINERS_STORAGE_CONF
# for the rootless graphroot.
STATE=/var/tmp/$USER/dftracer-utils-wheels
mkdir -p "$STATE/bin" "$STATE/storage" "$STATE/run" "$STATE/ccache"
cat >"$STATE/bin/podman" <<WRAP
#!/usr/bin/env bash
exec "$(command -v podman)" --root "$STATE/storage" --runroot "$STATE/run" "\$@"
WRAP
chmod +x "$STATE/bin/podman"
export PATH="$STATE/bin:$PATH"

# scikit-build-core never runs setup.py, so the .postN.dev0 scheme dftracer and
# pydftracer set there cannot be expressed here: a custom version_scheme needs a
# registered entry point. Compute it and hand it over instead, so all three
# publish the same shape. pyproject's no-guess-dev stays as the local fallback.
described=$(git describe --tags --long --match 'v*')
tag=${described%-*-g*}
distance=${described#"$tag"-}
distance=${distance%-g*}
tag=${tag#v}
if [ "$distance" = "0" ]; then
  VERSION="$tag"
else
  VERSION="$tag.post$distance.dev0"
fi
export SETUPTOOLS_SCM_PRETEND_VERSION_FOR_DFTRACER_UTILS="$VERSION"
echo "building version $VERSION"

export CIBW_BUILD="${CIBW_BUILD:-cp38-* cp39-* cp310-* cp311-* cp312-* cp313-* cp314-*}"
export CIBW_SKIP="*-win32 *-manylinux_i686 *-musllinux_* *-manylinux_ppc64le *-manylinux_s390x"
export CIBW_CONFIG_SETTINGS="cmake.define.DFTRACER_UTILS_LOCAL_PACKAGES=OFF"
export CIBW_BEFORE_ALL_LINUX="yum install -y epel-release ccache || dnf install -y epel-release ccache || true"
export CIBW_BEFORE_BUILD="ccache -s || echo 'WARNING: ccache unavailable - build is uncached'"
export CIBW_CONTAINER_ENGINE="podman; create_args: --volume=$STATE/ccache:/ccache"
export CIBW_ENVIRONMENT_LINUX="SETUPTOOLS_SCM_PRETEND_VERSION_FOR_DFTRACER_UTILS=$VERSION CCACHE_DIR=/ccache CCACHE_BASEDIR=/project CCACHE_COMPILERCHECK=content CCACHE_NOHASHDIR=true CCACHE_MAXSIZE=900M CCACHE_SLOPPINESS=time_macros,include_file_mtime,include_file_ctime,pch_defines,system_headers,locale"

# Same release as the pinned action, read from it rather than repeated: 4.x
# dropped the cp38 that CIBW_BUILD above still asks for.
CIBW_VERSION="${CIBW_VERSION:-$(grep -oE 'pypa/cibuildwheel@v[0-9]+\.[0-9]+\.[0-9]+' \
  .github/workflows/ci.yml | head -1 | sed 's/.*@v//')}"
[ -n "$CIBW_VERSION" ] || { echo "ERROR: no pinned cibuildwheel in .github/workflows/ci.yml"; exit 1; }
echo "using cibuildwheel $CIBW_VERSION"

rm -rf wheelhouse
mkdir -p wheelhouse
python3 -m venv "$STATE/venv"
"$STATE/venv/bin/pip" install -q --upgrade pip "cibuildwheel==$CIBW_VERSION"

# cibuildwheel builds the interpreters one after another. The C++ sources are
# identical across ABIs, so running them concurrently against the shared ccache
# costs little more than the first build and saves the rest of the serial time.
read -ra _tags <<<"$CIBW_BUILD"
SHARDS="${WHEEL_SHARDS:-4}"
JOBS_PER_SHARD=$(( $(nproc) / SHARDS ))
[ "$JOBS_PER_SHARD" -lt 1 ] && JOBS_PER_SHARD=1
export CIBW_ENVIRONMENT_LINUX="$CIBW_ENVIRONMENT_LINUX CMAKE_BUILD_PARALLEL_LEVEL=$JOBS_PER_SHARD"

# Pull once up front: concurrent shards would otherwise race to pull the same
# manylinux image into one podman store.
image=$("$STATE/venv/bin/python" - <<'PYEOF' 2>/dev/null || true
import configparser, pathlib, cibuildwheel
cfg = pathlib.Path(cibuildwheel.__file__).parent / "resources" / "pinned_docker_images.cfg"
c = configparser.ConfigParser(); c.read(cfg)
print(c["x86_64"]["manylinux_2_28"])
PYEOF
)
if [ -n "$image" ]; then
  echo "pre-pulling $image"
  podman pull -q "$image"
fi

echo "building ${#_tags[@]} interpreters, ${SHARDS} at a time, ${JOBS_PER_SHARD} jobs each"
rc=0
pids=()
for tag in "${_tags[@]}"; do
  while [ "$(jobs -rp | wc -l)" -ge "$SHARDS" ]; do wait -n || rc=1; done
  (
    CIBW_BUILD="$tag" "$STATE/venv/bin/cibuildwheel" \
      --output-dir "wheelhouse/.shard-${tag%%-*}" 2>&1 |
      stdbuf -oL sed -u "s/^/[${tag%%-*}] /"
  ) &
  pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid" || rc=1; done
[ "$rc" -eq 0 ] || { echo "ERROR: one or more interpreters failed"; exit 1; }

find wheelhouse -mindepth 2 -name '*.whl' -exec mv -t wheelhouse {} +
rm -rf wheelhouse/.shard-*
ls -1 wheelhouse
