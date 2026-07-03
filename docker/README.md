# Docker test toolchains

Linux build/test environments for dftracer-utils, mainly for validating the
coroutine core on real GCC (the frame-layout bug originates in GCC 12) from a
macOS host.

- `gcc12.Dockerfile` - Ubuntu 22.04 + GCC 12 (the frame-bug compiler).
- `gcc-latest.Dockerfile` - Ubuntu 24.04 + current GCC.

Both images bake in the toolchain plus OpenMPI and Python, so a run needs no
`apt install`.

## Usage

```
scripts/docker-test.sh [variant] [scope] [options]
  variant : gcc12 | latest                  (default: gcc12)
  scope   : cpp | python | both | valgrind  (default: cpp)
```

```bash
scripts/docker-test.sh gcc12                 # C++ ctest (incl. MPI) under GCC 12
scripts/docker-test.sh latest python         # pytest under newest GCC
scripts/docker-test.sh gcc12 both --monitor tree -j 8
scripts/docker-test.sh --variant gcc12 --scope cpp --no-mpi
```

Flags (override the matching env var): `--variant`, `--scope`, `--monitor`,
`--mpi on|off` / `--no-mpi`, `-j/--jobs`. Env equivalents:
`DFTRACER_UTILS_MONITOR`, `MPI`, `JOBS`.

Make shortcuts: `make docker-test-gcc12`, `make docker-test-latest` (C++ scope).

## What each scope runs

- `cpp` - configure + build + `ctest` (includes the MPI tests; OpenMPI runs as
  root in the container).
- `python` - `pip install -e .[dev]` (builds the extension via
  scikit-build-core) + `pytest tests/python`, in a venv kept in the build dir.
- `both` - C++ then Python.
- `valgrind` - runs `tests/valgrind/run.sh cpp` in this image (memcheck +
  suppressions + VALGRIND_MODE), giving e.g. valgrind-under-GCC-12 that the
  dedicated `make valgrind` (Ubuntu 24.04) image does not cover. Uses a
  persistent `build/build-docker-<variant>-valgrind/` dir.

## Why it is fast after the first run

- The toolchain is baked into the image, so there is no `apt install` per run.
- The build tree lives in a bind-mounted `build/build-docker-<variant>/`, a
  separate folder from the host's macOS build dirs (`build/build-dev`, ...), so
  Linux and macOS objects never collide and the Linux tree persists between
  runs. Only the first run pays the full dependency compile; later runs are
  incremental. Do not delete it between runs.
- Vendored sources are reused from the host `.cpmsource/` cache; the Python
  venv is reused from the build dir.

Install/discovery tests (`*_discovery_*`, `test_target_*`) are skipped: they
require an installed package plus system pkg-config/CMake config
(`FIXTURES_REQUIRED installed_package`), which these minimal images do not set
up.
