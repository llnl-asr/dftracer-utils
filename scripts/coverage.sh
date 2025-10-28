#!/bin/bash
# Coverage script for dftracer-utils
# Generates code coverage reports using gcov/lcov

set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

BUILD_DIR="build_coverage"
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
        if ! command -v "$dep" &> /dev/null; then
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

    if command -v ninja &> /dev/null; then
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
    local build_info
    IFS='|' read -r generator build_tool <<< "$(detect_build_system)"

    log_info "Building project with coverage enabled using $build_tool..."

    mkdir -p "$BUILD_DIR"

    # Prepare CMake arguments
    local cmake_args=(
        "-DCMAKE_BUILD_TYPE=Debug"
        "-DDFTRACER_UTILS_TESTS=ON"
        "-DDFTRACER_UTILS_COVERAGE=ON"
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
        "-G$generator"
    )

    # Use environment compilers if available (for Nix)
    if [ -n "${CC:-}" ]; then
        cmake_args+=("-DCMAKE_C_COMPILER=$CC")
    fi
    if [ -n "${CXX:-}" ]; then
        cmake_args+=("-DCMAKE_CXX_COMPILER=$CXX")
    fi

    # Configure
    cmake -S . -B "$BUILD_DIR" "${cmake_args[@]}"

    # Build
    local num_jobs
    num_jobs=$(get_num_jobs)

    if [[ "$build_tool" == "ninja" ]]; then
        ninja -C "$BUILD_DIR" -j "$num_jobs"
    else
        make -C "$BUILD_DIR" -j "$num_jobs"
    fi

    log_success "Build completed with $build_tool"
}

# ============================================================================
# Testing
# ============================================================================

run_tests() {
    log_info "Running tests..."

    if ctest --test-dir "$BUILD_DIR" --output-on-failure; then
        log_success "All tests passed"
    else
        log_warning "Some tests failed, continuing with coverage analysis..."
    fi
}

# ============================================================================
# Coverage Generation
# ============================================================================

generate_coverage_report() {
    log_info "Generating coverage report..."

    mkdir -p "$COVERAGE_DIR"

    # Capture coverage data
    lcov --capture \
         --directory "$BUILD_DIR" \
         --output-file "$COVERAGE_DIR/coverage.info" \
         --rc lcov_branch_coverage=1

    # Filter to include only source files
    lcov --extract "$COVERAGE_DIR/coverage.info" \
         "*/src/*" \
         --output-file "$COVERAGE_DIR/coverage_src.info" \
         --rc lcov_branch_coverage=1

    # Remove unwanted files
    lcov --remove "$COVERAGE_DIR/coverage_src.info" \
         "*/test*" \
         "*/.cpmsource/*" \
         "*/external/*" \
         "*/third_party/*" \
         --output-file "$COVERAGE_DIR/coverage_filtered.info" \
         --rc lcov_branch_coverage=1

    # Generate HTML report
    genhtml "$COVERAGE_DIR/coverage_filtered.info" \
            --output-directory "$COVERAGE_DIR/html" \
            --title "dftracer-utils Coverage Report" \
            --num-spaces 4 \
            --sort \
            --function-coverage \
            --branch-coverage \
            --legend \
            --demangle-cpp

    log_success "Coverage report generated in $COVERAGE_DIR/html/"
}

# ============================================================================
# Summary
# ============================================================================

show_coverage_summary() {
    log_info "Coverage Summary:"
    echo ""

    local summary
    summary=$(lcov --summary "$COVERAGE_DIR/coverage_filtered.info" 2>&1)

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
    if (( $(echo "$line_cov >= $MIN_COVERAGE" | bc -l 2>/dev/null || echo 0) )); then
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
        if command -v xdg-open &> /dev/null; then
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
    cat << EOF
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
            -h|--help)
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
