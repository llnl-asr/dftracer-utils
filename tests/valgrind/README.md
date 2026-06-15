# Valgrind tests

Valgrind memory-checks the dftracer-utils C++ tests, the native Python binding, and (opt-in) the MPI binaries. 
Valgrind does not run on macOS - especially Apple Silicon - so on macOS these targets run inside a Linux Docker container.
On Linux (and CI) they run natively. 
The `make valgrind*` targets auto-detect which mode to use.

## Quick start

```sh
make valgrind          # C++ + Python under Valgrind
make valgrind-cpp      # C++ only
make valgrind-py       # native-binding Python tests only
make valgrind-mpi      # MPI tests, each rank wrapped in Valgrind (opt-in)
```

- On **Linux** with `valgrind` on `PATH`, these run natively.
- On **macOS** (no `valgrind`), they build/run inside the Docker image defined by `tests/valgrind/Dockerfile`. 
  The image is built automatically on first use.

Force a mode with `VALGRIND_MODE=native` or `VALGRIND_MODE=docker`.

Other helpers:

```sh
make valgrind-build    # build the Docker image (no-op when running natively)
make valgrind-shell    # interactive shell in the Docker image
make valgrind-clean    # remove build/venv/log artifacts
```

## What runs

| Target          | Scope                                                                 |
|-----------------|-----------------------------------------------------------------------|
| `valgrind-cpp`  | Every doctest unit binary (single-command ctest tests), full leak check. |
| `valgrind-py`   | Native-binding-focused pytest files (see below).                      |
| `valgrind-mpi`  | `test_dftracer_call_tree_mpi`, `test_dftracer_aggregator_mpi`.        |


`test_reader_robustness` and `test_reader_formats` are **excluded by default**: they're slow-and-redundant for leak detection (no new alloc/free *path*, just scale/repetition).

```sh
make valgrind-cpp                                      # default set, parallel
make valgrind-cpp VALGRIND_CTEST_FILTER='reader_'      # just the reader tests
make valgrind-cpp VALGRIND_TEST_JOBS=8                 # more concurrency
make valgrind-cpp VALGRIND_CTEST_EXCLUDE=''            # everything incl. robustness
make valgrind-cpp VALGRIND_TEST_TIMEOUT=0              # no per-test timeout
```

**Python** runs the whole `tests/python` suite (dask suites included) under
`PYTHONMALLOC=malloc` (so Valgrind sees real allocations instead of pymalloc
arenas). dask/distributed, numpy and pyarrow come from the `[dev]` extra.

## Suppressions

Interpreter/runtime noise that is not a dftracer-utils bug is filtered via:

- `valgrind-cpp.supp` — loader/libc/zlib noise for C++ runs (always applied).
- `valgrind-python.supp` — CPython interpreter noise (Python runs).
- `valgrind-mpi.supp` — OpenMPI/PMIx runtime noise (MPI runs).

## Tuning

| Variable                 | Effect                                                        |
|--------------------------|---------------------------------------------------------------|
| `VALGRIND_MODE`          | `native` or `docker` (default: auto-detect).                  |
| `VALGRIND_BUILD_JOBS`    | Build parallelism (default: ~1 job/2 GB RAM, capped at CPUs). |
| `VALGRIND_CTEST_FILTER`  | Regex; run only matching C++ tests.                           |
| `VALGRIND_CTEST_EXCLUDE` | Regex of C++ tests to skip (default `test_reader_robustness\|test_reader_formats`; `''` runs everything). |
| `VALGRIND_TEST_JOBS`     | Concurrent C++ valgrind runs (default: ~1 per 3 GB, capped at CPUs; binary tests use `--trace-children`, so two valgrind instances each). |
| `VALGRIND_TEST_TIMEOUT`  | Per-C++-test timeout in seconds (default 1800).               |
| `VALGRIND_PY_TIMEOUT`    | Timeout for the whole Python run in seconds (default 3600).   |
| `VALGRIND_PY_TRACE_CHILDREN` | `1` follows dask worker subprocesses into the native binding (default 0; OFF because each worker runs as a full Python interpreter under Valgrind and can OOM CI). |
| `VALGRIND_TRACK_ORIGINS` | `1` adds `--track-origins=yes` (slower; pinpoints uninit reads). |
| `VALGRIND_PYTEST_FILES`  | Override the Python test set.                                 |
| `VALGRIND_EXTRA_OPTS`    | Extra options appended to every valgrind invocation.          |


