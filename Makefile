.PHONY: coverage coverage-clean coverage-view coverage-open test test-coverage test-py build clean format check-format cmake-format lint typecheck help \
        valgrind valgrind-cpp valgrind-py valgrind-mpi valgrind-build valgrind-shell valgrind-clean \
        docker-test-gcc12 docker-test-latest

RUN_TY ?= 0

# Detect build system
BUILD_GENERATOR := $(shell command -v ninja >/dev/null 2>&1 && echo "Ninja" || echo "Unix Makefiles")
BUILD_TOOL := $(shell command -v ninja >/dev/null 2>&1 && echo "ninja" || echo "make")
NUM_JOBS := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Default target
help:
	@echo "Available targets:"
	@echo "  coverage        - Build with coverage and generate HTML report"
	@echo "  coverage-open   - Build coverage and open report in browser"
	@echo "  coverage-clean  - Clean coverage build directory"
	@echo "  coverage-view   - Open existing coverage report in browser"
	@echo "  test            - Build and run tests without coverage"
	@echo "  test-coverage   - Run tests with coverage (requires prior coverage build)"
	@echo "  test-py         - Run Python tests in isolated venv"
	@echo "  format          - Format code using clang-format"
	@echo "  check-format    - Check code formatting"
	@echo "  cmake-format    - Format CMake files"
	@echo "  lint            - Run ruff linter on Python code"
	@echo "  typecheck       - Run ty type checker on Python code"
	@echo "  valgrind        - Run C++ and Python Valgrind tests (native if available, else Docker)"
	@echo "  valgrind-cpp    - Run C++ tests under Valgrind"
	@echo "  valgrind-py     - Run native-binding Python tests under Valgrind"
	@echo "  valgrind-mpi    - Run MPI tests with each rank wrapped in Valgrind"
	@echo "  valgrind-build  - Build the Valgrind Docker image (macOS only)"
	@echo "  valgrind-shell  - Open a shell in the Valgrind Docker image"
	@echo "  valgrind-clean  - Remove Valgrind build/venv/logs"
	@echo "  docker-test-gcc12  - Build+test under GCC 12 in Docker (incremental build/build-docker-gcc12)"
	@echo "  docker-test-latest - Build+test under newest GCC (Ubuntu 24.04) in Docker"
	@echo "  clean           - Clean all build directories"
	@echo "  help            - Show this help"
	@echo ""
	@echo "Build system: $(BUILD_GENERATOR) ($(BUILD_TOOL))"

# Generate coverage report
coverage:
	@./scripts/coverage.sh

# Generate coverage report and open in browser
coverage-open:
	@./scripts/coverage.sh --open

# Clean coverage build
coverage-clean:
	@echo "Cleaning coverage build directory..."
	@rm -rf build_coverage build/build-coverage coverage

# View existing coverage report
coverage-view:
	@./scripts/coverage.sh --no-clean --open || \
		(echo "Coverage report not found. Run 'make coverage' first." && exit 1)

# Build and run tests without coverage
test:
	@echo "Building and running tests..."
	@cmake --preset tests
	@cmake --build --preset tests
	@ctest --preset tests

# Run tests with coverage (requires coverage build)
test-coverage:
	@if [ -d "build/build-coverage" ]; then \
		ctest --preset coverage; \
	else \
		echo "Coverage build not found. Run 'make coverage' first."; \
		exit 1; \
	fi

# Run Python tests in isolated environment
test-py:
	@echo "Running Python tests in isolated environment..."
	@rm -rf .venv_test_py
	@python3 -m venv .venv_test_py
	@.venv_test_py/bin/pip install --upgrade pip setuptools wheel
	@if [ "$(RUN_TY)" = "1" ]; then \
		.venv_test_py/bin/pip install -e .[dev] ty; \
	else \
		.venv_test_py/bin/pip install -e .[dev]; \
	fi
	@.venv_test_py/bin/pytest tests/python -v
	@if [ "$(RUN_TY)" = "1" ]; then \
		.venv_test_py/bin/ty check --python "$$(pwd)/.venv_test_py/bin/python" python/; \
	fi
	@rm -rf .venv_test_py
	@echo "Python tests completed successfully!"

# Valgrind tests
VALGRIND_MAKE = $(MAKE) --no-print-directory -C tests/valgrind

# Build + test in a Linux toolchain container (persistent incremental build dir)
docker-test-gcc12:
	@./scripts/docker-test.sh gcc12

docker-test-latest:
	@./scripts/docker-test.sh latest

valgrind:
	@$(VALGRIND_MAKE) all

valgrind-cpp:
	@$(VALGRIND_MAKE) cpp

valgrind-py:
	@$(VALGRIND_MAKE) py

valgrind-mpi:
	@$(VALGRIND_MAKE) mpi

valgrind-debug-hang:
	@$(VALGRIND_MAKE) debug-hang

valgrind-build:
	@$(VALGRIND_MAKE) build

valgrind-shell:
	@$(VALGRIND_MAKE) shell

valgrind-clean:
	@$(VALGRIND_MAKE) clean

# Python linting
lint:
	@echo "Running ruff..."
	@uvx ruff check python/ tests/python/
	@uvx ruff format --check python/ tests/python/
	@echo "Ruff passed!"

# Python type checking
typecheck:
	@echo "Running ty..."
	@uvx ty check python/
	@echo "Type check passed!"

# Code formatting
format:
	@echo "Formatting code..."
	@./scripts/formatting/autoformat.sh

check-format:
	@echo "Checking code format..."
	@./scripts/formatting/check-formatting.sh

cmake-format:
	@echo "Formatting CMake files..."
	@cmake-format CMakeLists.txt src/CMakeLists.txt cmake/**/*.cmake --in-place

# Clean all build artifacts
clean:
	@echo "Cleaning all build directories..."
	@rm -rf build_* coverage .venv_test_py
	@find . -name "*.gcda" -o -name "*.gcno" -o -name "*.gcov" | xargs rm -f 2>/dev/null || true
	@echo "Clean complete!"
