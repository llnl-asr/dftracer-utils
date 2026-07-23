#!/bin/bash
# Coverage script for dftracer-utils
# Generates code coverage reports using gcov/lcov

set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

BUILD_DIR="build/build-coverage"  # matches the `coverage` preset binaryDir
COVERAGE_DIR="coverage"
MIN_COVERAGE=80

# Colors
readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly BLUE='\033[0;34m'
readonly NC='\033[0m'

# ============================================================================
# Utility Functions
# ============================================================================

log_info() {
	echo -e "${BLUE}[INFO]${NC} $*"
}

log_success() {
	echo -e "${GREEN}[SUCCESS]${NC} $*"
}

log_warning() {
	echo -e "${YELLOW}[WARNING]${NC} $*"
}

log_error() {
	echo -e "${RED}[ERROR]${NC} $*"
}

# ============================================================================
# Dependency Checks
# ============================================================================

check_dependencies() {
	log_info "Checking dependencies..."

	local missing_deps=()

	for dep in lcov genhtml gcov; do
		if ! command -v "$dep" &>/dev/null; then
			missing_deps+=("$dep")
		fi
	done

	if [ ${#missing_deps[@]} -ne 0 ]; then
		log_error "Missing dependencies: ${missing_deps[*]}"
		log_info "Install missing dependencies:"
		if [[ "$OSTYPE" == "darwin"* ]]; then
			echo "  brew install lcov"
		elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
			echo "  sudo apt-get install lcov"
		fi
		exit 1
	fi

	log_success "All dependencies found"
}

# ============================================================================
# Build System Detection
# ============================================================================

detect_build_system() {
	local generator="Unix Makefiles"
	local build_tool="make"

	if command -v ninja &>/dev/null; then
		generator="Ninja"
		build_tool="ninja"
	fi

	echo "$generator|$build_tool"
}

get_num_jobs() {
	if [[ "$OSTYPE" == "darwin"* ]]; then
		sysctl -n hw.ncpu
	else
		nproc
	fi
}

# ============================================================================
# Cleanup
# ============================================================================

clean_previous() {
	log_info "Cleaning previous coverage data..."

	rm -rf "$BUILD_DIR" "$COVERAGE_DIR"
	find . -name "*.gcda" -o -name "*.gcno" | xargs rm -f 2>/dev/null || true

	log_success "Cleaned previous coverage data"
}

# ============================================================================
# Build
# ============================================================================

build_with_coverage() {
	# The `coverage` preset uses the Ninja generator (inherited from `dev`),
	# so Ninja must be available.
	local generator build_tool
	IFS='|' read -r generator build_tool <<<"$(detect_build_system)"
	if [[ "$build_tool" != "ninja" ]]; then
		log_error "The coverage preset requires Ninja. Please install ninja."
		exit 1
	fi

	log_info "Building project with coverage enabled (preset: coverage)..."

	# Configure + build via the `coverage` preset, the single source of truth
	# for coverage instrumentation. CC/CXX from the environment are still
	# honored by CMake (e.g. for Nix).
	cmake --preset coverage
	cmake --build --preset coverage -j "$(get_num_jobs)"

	log_success "Build completed"
}

# ============================================================================
# Testing
# ============================================================================

run_tests() {
	log_info "Running tests..."

	if ! ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 300; then
		log_error "Tests failed"
		exit 1
	fi

	log_success "All tests passed"
}

# ============================================================================
# Python Binding Tests (coverage via gcov-instrumented .so)
# ============================================================================

run_python_tests() {
	log_info "Running Python tests against coverage-instrumented build..."

	# The coverage build places the gcov-instrumented .so under
	# BUILD_DIR/dftracer/utils/. The extension has rpath set to
	# $ORIGIN/../../lib (or @loader_path/../../lib on macOS), which
	# resolves to BUILD_DIR/lib/ from that location.
	#
	# Strategy: symlink the instrumented .so into the Python source tree
	# so `from .dftracer_utils_ext` resolves via PYTHONPATH, and symlink
	# the shared libs into the matching rpath-relative location so the
	# dynamic linker finds them without DYLD_LIBRARY_PATH (which macOS
	# SIP can strip).
	local ext_so
	ext_so=$(find "$BUILD_DIR/dftracer/utils" -name 'dftracer_utils_ext*' \( -name '*.so' -o -name '*.dylib' \) -print -quit 2>/dev/null)
	if [ -z "$ext_so" ]; then
		log_warning "Could not find built dftracer_utils_ext .so — skipping Python coverage"
		return
	fi

	# Symlink the instrumented extension into the Python source tree
	ln -sf "${PWD}/${ext_so}" "python/dftracer/utils/$(basename "$ext_so")"

	# Create lib/ at the rpath-relative location: python/dftracer/utils/../../lib -> python/lib
	# This matches $ORIGIN/../../lib relative to python/dftracer/utils/
	mkdir -p python/lib
	for lib in "${BUILD_DIR}/lib"/*; do
		[ -e "$lib" ] && ln -sf "${PWD}/${lib}" "python/lib/$(basename "$lib")"
	done

	# Install test dependencies only (not the package itself)
	pip install pytest pyarrow pandas numpy

	PYTHONPATH="${PWD}/python" pytest tests/python -v
	local pytest_status=$?

	# Clean up symlinks
	rm -f "python/dftracer/utils/$(basename "$ext_so")"
	rm -rf python/lib

	if [ "$pytest_status" -ne 0 ]; then
		log_error "Python tests failed"
		exit "$pytest_status"
	fi

	log_success "Python coverage tests completed"
}

# ============================================================================
# Coverage Generation
# ============================================================================

generate_coverage_report() {
	log_info "Generating coverage report..."

	mkdir -p "$COVERAGE_DIR"

	# lcov 2.x changed RC option names and added --ignore-errors.
	# lcov 1.x (Ubuntu 22.04) only understands the old names and has no
	# --ignore-errors flag.
	local lcov_major
	lcov_major=$(lcov --version | sed 's/[^0-9]*//' | cut -d. -f1)

	local lcov_flags=()
	local genhtml_flags=()
	if [ "$lcov_major" -ge 2 ]; then
		# Double-specify each category to fully suppress (not just downgrade to warning).
		# Categories needed for macOS + LLVM gcov:
		#   inconsistent  – Xcode libc++ headers have hit-line-without-branch data
		#   gcov           – some constants-only TUs produce no .gcda data
		#   format         – LLVM gcov emits line-number 0 for coroutine thunks
		#   unsupported    – function begin/end lines not supported by this gcov
		#   deprecated     – old RC option name warnings
		lcov_flags=(
			--rc branch_coverage=1
			--ignore-errors inconsistent,inconsistent
			--ignore-errors gcov,gcov
			--ignore-errors format,format
			--ignore-errors unsupported,unsupported
			--ignore-errors unused,unused
			--ignore-errors deprecated,deprecated
		)
		genhtml_flags=(
			--ignore-errors inconsistent,inconsistent
			--ignore-errors format,format
			--ignore-errors unsupported,unsupported
			--ignore-errors category,category
			--ignore-errors deprecated,deprecated
		)
	else
		lcov_flags=(--rc lcov_branch_coverage=1)
	fi

	# lcov >= 2.5 appends --no-strip-underscores (a GNU c++filt flag) to the
	# demangler on macOS, where Xcode's llvm-cxxfilt only knows the singular
	# form and exits non-zero, leaving genhtml with an empty tracefile.
	if printf '_Z1fv\n' | c++filt --no-strip-underscores >/dev/null 2>&1; then
		genhtml_flags+=(--demangle-cpp)
	else
		log_warning "c++filt lacks --no-strip-underscores; disabling --demangle-cpp"
	fi

	# Capture coverage data
	lcov --capture \
		--directory "$BUILD_DIR" \
		--output-file "$COVERAGE_DIR/coverage.info" \
		"${lcov_flags[@]}"

	# Filter to include only source files
	lcov --extract "$COVERAGE_DIR/coverage.info" \
		"*/src/*" \
		--output-file "$COVERAGE_DIR/coverage_src.info" \
		"${lcov_flags[@]}"

	# Remove unwanted files
	lcov --remove "$COVERAGE_DIR/coverage_src.info" \
		"*/test*" \
		"*/.cpmsource/*" \
		"*/external/*" \
		"*/third_party/*" \
		--output-file "$COVERAGE_DIR/coverage_filtered.info" \
		"${lcov_flags[@]}"

	# Generate HTML report
	# No --sort: lcov 2.4 renamed it to --sort-tables, and sorting is on by default.
	genhtml "$COVERAGE_DIR/coverage_filtered.info" \
		--output-directory "$COVERAGE_DIR/html" \
		--title "dftracer-utils Coverage Report" \
		--num-spaces 4 \
		--function-coverage \
		--branch-coverage \
		--legend \
		"${genhtml_flags[@]}"

	log_success "Coverage report generated in $COVERAGE_DIR/html/"
}

# ============================================================================
# Summary
# ============================================================================

show_coverage_summary() {
	log_info "Coverage Summary:"
	echo ""

	local summary
	local lcov_major
	lcov_major=$(lcov --version | sed 's/[^0-9]*//' | cut -d. -f1)

	local summary_flags=()
	if [ "$lcov_major" -ge 2 ]; then
		summary_flags=(
			--ignore-errors inconsistent,inconsistent
			--ignore-errors format,format
			--ignore-errors unsupported,unsupported
		)
	fi

	summary=$(lcov --summary "$COVERAGE_DIR/coverage_filtered.info" \
		"${summary_flags[@]}" 2>&1)

	# Extract coverage percentages
	local line_cov function_cov branch_cov
	line_cov=$(echo "$summary" | grep -i "lines" | awk '{print $2}' | sed 's/%//' || echo "0")
	function_cov=$(echo "$summary" | grep -i "functions" | awk '{print $2}' | sed 's/%//' || echo "0")
	branch_cov=$(echo "$summary" | grep -i "branches" | awk '{print $2}' | sed 's/%//' || echo "0")

	printf "  %-20s %6s%%\n" "Line Coverage:" "$line_cov"
	printf "  %-20s %6s%%\n" "Function Coverage:" "$function_cov"
	printf "  %-20s %6s%%\n" "Branch Coverage:" "$branch_cov"
	echo ""

	# Check minimum threshold
	if (($(echo "$line_cov >= $MIN_COVERAGE" | bc -l 2>/dev/null || echo 0))); then
		log_success "Coverage meets minimum threshold of ${MIN_COVERAGE}%"
	else
		log_warning "Coverage ($line_cov%) is below minimum threshold of ${MIN_COVERAGE}%"
	fi

	echo ""
	log_info "View detailed report: file://$(pwd)/$COVERAGE_DIR/html/index.html"
}

# ============================================================================
# Browser Opening
# ============================================================================

open_report() {
	local report_path
	report_path="$(pwd)/$COVERAGE_DIR/html/index.html"

	if [[ "$OSTYPE" == "darwin"* ]]; then
		open "$report_path"
	elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
		if command -v xdg-open &>/dev/null; then
			xdg-open "$report_path"
		else
			log_warning "xdg-open not found, cannot open browser automatically"
		fi
	fi
}

# ============================================================================
# Main
# ============================================================================

show_help() {
	cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Generate code coverage report for dftracer-utils.

OPTIONS:
    --open              Open coverage report in browser after generation
    --no-clean          Skip cleaning previous coverage data
    --min-coverage NUM  Set minimum coverage threshold (default: $MIN_COVERAGE)
    -h, --help          Show this help message

EXAMPLES:
    $(basename "$0")                    # Generate coverage report
    $(basename "$0") --open             # Generate and open in browser
    $(basename "$0") --min-coverage 90  # Set 90% minimum threshold

EOF
}

main() {
	local open_browser=false
	local do_clean=true

	# Parse arguments
	while [[ $# -gt 0 ]]; do
		case $1 in
		--open)
			open_browser=true
			shift
			;;
		--no-clean)
			do_clean=false
			shift
			;;
		--min-coverage)
			MIN_COVERAGE="$2"
			shift 2
			;;
		-h | --help)
			show_help
			exit 0
			;;
		*)
			log_error "Unknown option: $1"
			show_help
			exit 1
			;;
		esac
	done

	log_info "Starting coverage analysis for dftracer-utils"
	echo ""

	check_dependencies

	if [ "$do_clean" = true ]; then
		clean_previous
	fi

	build_with_coverage
	run_tests
	run_python_tests
	generate_coverage_report
	show_coverage_summary

	if [ "$open_browser" = true ]; then
		open_report
	fi

	echo ""
	log_success "Coverage analysis completed!"
}

# Run main if executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	main "$@"
fi
