.PHONY: coverage coverage-clean coverage-view coverage-open test test-coverage test-py build clean format check-format cmake-format help

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
	@rm -rf build_coverage coverage

# View existing coverage report
coverage-view:
	@./scripts/coverage.sh --no-clean --open || \
		(echo "Coverage report not found. Run 'make coverage' first." && exit 1)

# Build and run tests without coverage
test:
	@echo "Building and running tests with $(BUILD_TOOL)..."
	@mkdir -p build_test
	@cmake -S . -B build_test \
		-G"$(BUILD_GENERATOR)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DDFTRACER_UTILS_TESTS=ON \
		-DDFTRACER_UTILS_DEBUG=ON \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@cmake --build build_test -j $(NUM_JOBS)
	@ctest --test-dir build_test --output-on-failure -j $(NUM_JOBS)

# Run tests with coverage (requires coverage build)
test-coverage:
	@if [ -d "build_coverage" ]; then \
		ctest --test-dir build_coverage --output-on-failure; \
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
	@.venv_test_py/bin/pip install -e .[dev]
	@.venv_test_py/bin/pytest tests/python -v
	@rm -rf .venv_test_py
	@echo "Python tests completed successfully!"

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
