# Valgrind test runner.
#
# Use from the repo root via the delegating `make valgrind-*` targets, or
# directly here: `make -C tests/valgrind cpp`. Runs natively when `valgrind` is
# on PATH (Linux/CI), otherwise inside the Docker image built from ./Dockerfile
# (the macOS fallback). Force a mode with VALGRIND_MODE=native|docker.
.PHONY: all cpp py mpi build shell clean help

# This Makefile lives in tests/valgrind/; the repo is two levels up.
REPO_ROOT := $(abspath $(CURDIR)/../..)

VALGRIND_IMAGE ?= dftracer-utils-valgrind
DOCKER ?= docker
VALGRIND_MODE ?= $(shell command -v valgrind >/dev/null 2>&1 && echo native || echo docker)

# Tuning knobs, exported so both the native run.sh and the docker `-e`
# forwarding pick them up whether set in the environment or on the make command
# line (command-line vars flow down from the root Makefile via MAKEFLAGS).
export VALGRIND_SHARD VALGRIND_EMIT_DURATIONS VALGRIND_CTEST_FILTER VALGRIND_CTEST_EXCLUDE VALGRIND_TEST_JOBS \
       VALGRIND_TEST_TIMEOUT VALGRIND_BUILD_JOBS VALGRIND_TRACK_ORIGINS \
       VALGRIND_PY_TIMEOUT VALGRIND_PY_TRACE_CHILDREN VALGRIND_PYTEST_FILES \
       VALGRIND_OURS_MATCH VALGRIND_EXTRA_OPTS VALGRIND_DUMP_AFTER \
       DFTRACER_UTILS_HW_CONCURRENCY

VALGRIND_ENV_FWD = -e VALGRIND_SHARD -e VALGRIND_EMIT_DURATIONS -e VALGRIND_CTEST_FILTER -e VALGRIND_CTEST_EXCLUDE \
	-e VALGRIND_TEST_JOBS -e VALGRIND_TEST_TIMEOUT -e VALGRIND_BUILD_JOBS \
	-e VALGRIND_TRACK_ORIGINS -e VALGRIND_PY_TIMEOUT -e VALGRIND_PY_TRACE_CHILDREN \
	-e VALGRIND_PYTEST_FILES -e VALGRIND_OURS_MATCH -e VALGRIND_EXTRA_OPTS \
	-e VALGRIND_DUMP_AFTER -e DFTRACER_UTILS_HW_CONCURRENCY
VALGRIND_DOCKER_RUN = $(DOCKER) run --rm -t \
	-v "$(REPO_ROOT)":/work -w /work \
	-e CCACHE_DIR=/work/.ccache -e CPM_SOURCE_CACHE=/work/.cpmsource \
	$(VALGRIND_ENV_FWD) \
	$(VALGRIND_IMAGE)

# $(call run_valgrind,<subcommand>)
ifeq ($(VALGRIND_MODE),native)
run_valgrind = bash $(CURDIR)/run.sh $(1)
else
run_valgrind = $(MAKE) build && $(VALGRIND_DOCKER_RUN) $(1)
endif

help:
	@echo "Valgrind targets (mode: $(VALGRIND_MODE)):"
	@echo "  all    - C++ and Python tests under Valgrind"
	@echo "  cpp    - C++ tests"
	@echo "  py     - Python tests (native-binding focus)"
	@echo "  mpi    - MPI tests with each rank wrapped in Valgrind"
	@echo "  build  - build the Docker image (macOS fallback)"
	@echo "  shell  - open a shell in the Docker image"
	@echo "  clean  - remove Valgrind build/venv/logs"

all:
	@echo "Running Valgrind tests (mode: $(VALGRIND_MODE))..."
	@$(call run_valgrind,all)

cpp:
	@echo "Running C++ Valgrind tests (mode: $(VALGRIND_MODE))..."
	@$(call run_valgrind,cpp)

py:
	@echo "Running Python Valgrind tests (mode: $(VALGRIND_MODE))..."
	@$(call run_valgrind,py)

mpi:
	@echo "Running MPI Valgrind tests (mode: $(VALGRIND_MODE))..."
	@$(call run_valgrind,mpi)

# Diagnostic: dump thread stacks of a single hanging test (VALGRIND_CTEST_FILTER).
debug-hang:
	@echo "Running Valgrind debug-hang (mode: $(VALGRIND_MODE))..."
	@$(call run_valgrind,debug-hang)

# Build the Docker image (no-op message when running natively).
build:
ifeq ($(VALGRIND_MODE),native)
	@echo "Valgrind is available natively; no Docker image needed."
else
	@echo "Building Valgrind Docker image '$(VALGRIND_IMAGE)'..."
	@$(DOCKER) build -t $(VALGRIND_IMAGE) $(CURDIR)
endif

shell: build
	@$(DOCKER) run --rm -it \
		-v "$(REPO_ROOT)":/work -w /work \
		-e CCACHE_DIR=/work/.ccache -e CPM_SOURCE_CACHE=/work/.cpmsource \
		--entrypoint bash $(VALGRIND_IMAGE)

clean:
	@echo "Cleaning Valgrind build artifacts..."
	@rm -rf $(REPO_ROOT)/build/build-valgrind $(REPO_ROOT)/build/build-valgrind-mpi \
		$(REPO_ROOT)/build/valgrind-logs $(REPO_ROOT)/.venv_valgrind
