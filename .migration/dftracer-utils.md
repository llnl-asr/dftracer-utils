# Migration Plan: dftracer-utils

Selected: 2026-07-30. Source: git@github.com:llnl/dftracer-utils.git (develop). Target: ssh://git@czgitlab.llnl.gov:7999/dftracer/dftracer-utils.git (repo exists on czgitlab).

## Steps (subagent fills findings + status)

1. [x] Latest develop (dfanalyzer freshly cloned; utils pulled)
2. [x] gitlab remote added + verified
3. [x] Convert .github/workflows → .gitlab-ci.yml on corona batch runner (.corona-batch template); publish jobs manual
4. [x] pages job for Sphinx docs (develop + temp gitlab-migration rule)
5. [x] In-place GitLab URL switch for deps — **not needed** (no dftracer-group deps; see findings)
6. [x] Local tests (YAML OK; sphinx build succeeded, 37 warnings; build/pytest smoke deferred — see findings)
7. [x] Commit on gitlab-migration; sync .migration/ into repo
8. [x] Push develop, tags, gitlab-migration
9. [ ] Pipeline: <https://czgitlab.llnl.gov/dftracer/dftracer-utils/-/pipelines>
10. [ ] User merges after green pipeline

## Findings

- Workflows converted: `ci.yml` (dependency prebuild + os/python test matrix, valgrind shards, web/npm, wheel/cibuildwheel) → single `test` job on `.corona-batch` running `make test` + `make test-py` with gcc/11.2.1 + python/3.13.2 + mvapich2/2.3.7 modules (matrices collapsed, coveralls/paths-filter/web dropped — GitHub-only, noted in header comment). `format-check.yaml` → `lint` job (pip-installs `clang-format==19.*` + `ruff`, runs `scripts/formatting/check-formatting.sh` + `ruff check`/`ruff format --check`). `python-publish.yaml` → `publish-pypi`, `when: manual` on tags; needs CI/CD variable `PYPI_TOKEN` (listed in `.gitlab-ci.yml` header). No docker on corona → cibuildwheel replaced by `python -m build` + twine.
- Sphinx source lives in `docs/source` (not `docs/`); pages job uses `sphinx-build -b html docs/source public` with `pip install -r docs/requirements.txt`. Doxygen/breathe steps in conf.py degrade gracefully when doxygen XML is absent.
- Dependency scan: NO dftracer-group GitHub deps (cpp-logger/brahma/dftracer/dfanalyzer/dftracer-agents) in pyproject.toml, setup.py, CMakeLists, or cmake/modules/Dependencies.cmake — all CPM deps are third-party. Only `github.com/LLNL/dftracer-utils` self-references (project metadata/docs). **Zero in-place changes made.**
- Build/pytest smoke not run locally: `make test` compiles the full C++ tree plus its dependencies from source (hours on a login node; GitHub CI uses prebuilt caches); left to the corona pipeline.

## Executed changes (what to undo on revert)

- Added `gitlab` remote: ssh://git@czgitlab.llnl.gov:7999/dftracer/dftracer-utils.git
- Branch `gitlab-migration` (from develop) with commits: `.gitlab-ci.yml` (ci commit) and `.migration/` copies (docs/migration commit) — SHAs in status log.
- Pushed to gitlab: `develop`, all tags, `gitlab-migration`. Nothing pushed to origin/GitHub; `.github/` untouched.
- **In-place changes: NONE** (nothing to fix on revert beyond the additive removals). Temp pages rule for `gitlab-migration` branch in `.gitlab-ci.yml` must be removed after merge.

## Status log

- 2026-07-30: plan created.
- 2026-07-30: executed. `.gitlab-ci.yml` written (lint/test/publish-pypi/pages on `.corona-batch`); YAML validated; sphinx built locally (success, 37 warnings); no dep URL switches needed; committed on `gitlab-migration`; pushed develop + tags + gitlab-migration to czgitlab.
- 2026-07-30: CI switched to corona flux-allocation flow, single allocation per pipeline; MR opened
- 2026-07-30: Flux allocation made global via allocate/.flux-jobid artifact/release-allocation jobs; wait-event timeout removed.
- 2026-07-30: allocate switched to flux alloc --bg (blocks until granted); branch rebased on gitlab/develop before push.
- 2026-07-30: CI now runs inside podman containers (python:3.11 lint/docs, ubuntu:22.04 build/test) on the allocated node via flux run; pattern validated on cpp-logger.
- 2026-07-30: fixed allocation-id race — 'flux job last' is user-global and concurrent pipelines cancelled each other's allocations; now uses a unique per-job name (<proj>-$CI_PIPELINE_ID-$CI_JOB_ID) with 'flux jobs --name' lookup, and cleanup only cancels a non-empty .flux-jobid.
- 2026-07-30: build/test container switched ubuntu:22.04 -> ubuntu:24.04 (CMakePresets version 6 needs CMake >=3.25; 22.04 ships 3.22) and package list aligned with the GitHub workflow; pip uses --break-system-packages (PEP 668).
- 2026-07-30: fixed the 15 package-discovery test failures: GNUInstallDirs resolves CMAKE_INSTALL_LIBDIR to Debian multiarch (lib/x86_64-linux-gnu) but the tests only search <prefix>/lib and lib64. Pinning -DCMAKE_INSTALL_LIBDIR=lib does not stick (a subproject re-includes GNUInstallDirs after a prefix change and FORCE-overwrites it), so CI pre-creates the install tree and symlinks lib/{cmake,pkgconfig} to the multiarch dirs. Verified locally in podman: 157/157 tests pass, docs build.
- 2026-07-30: release-allocation no longer declares needs — a job with needs is SKIPPED when a needed job fails, so failed pipelines leaked the allocation. Now stage ordering + when: always, cancelling every job matching $ALLOC_NAME; allocate also cancels stale allocations from a retried attempt.
