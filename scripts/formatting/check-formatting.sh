#! /bin/bash

clang_format_exe="clang-format"
if [ $# -ge 1 ]; then
    clang_format_exe="$1"
fi

SUPPORTED_CLANG_FORMAT_VERSION="19.1.7"

if ! command -v "$clang_format_exe" >/dev/null 2>&1; then
    echo "You must have 'clang-format' in PATH to use 'check-formatting.sh'"
    exit 1
fi

clang_format_version_str=$($clang_format_exe --version)
# Portable version parse: `grep -P`/`\K` is GNU-only and fails on macOS/BSD
# grep. `sed` works on both, and the [0-9][0-9.]* class also strips distro
# suffixes like "14.0.0-1ubuntu1".
clang_format_version=$(echo "$clang_format_version_str" | sed -n 's/.*clang-format version \([0-9][0-9.]*\).*/\1/p')

if [ "$clang_format_version" != "$SUPPORTED_CLANG_FORMAT_VERSION" ]; then
    echo "WARNING: the .clang-format file in this repo is designed for version $SUPPORTED_CLANG_FORMAT_VERSION."
    echo "         You are running with clang-format v$clang_format_version."
    echo "         The resulting check is highly likely to be incorrect."
fi

if ! command -v find >/dev/null 2>&1; then
    echo "You must have 'find' in PATH to use 'check-formatting.sh'"
    exit 1
fi

if ! command -v dirname >/dev/null 2>&1; then
    echo "You must have 'dirname' in PATH to use 'check-formatting.sh'"
    exit 1
fi

if ! command -v xargs >/dev/null 2>&1; then
    echo "You must have 'xargs' in PATH to use 'check-formatting.sh'"
    exit 1
fi

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

curr_dir=$(pwd)

# Navigate to project root (fixed: removed extra 'cd ..')
cd "$SCRIPT_DIR/../.." || exit 1

# Cross-platform CPU count detection
if command -v nproc >/dev/null 2>&1; then
    NUM_JOBS=$(nproc)
elif command -v sysctl >/dev/null 2>&1; then
    # macOS
    NUM_JOBS=$(sysctl -n hw.ncpu)
else
    NUM_JOBS=4
fi

check_rc=0

# Check all directories in parallel
check_dir() {
    local dir="$1"
    local clang_format="$2"
    local num_jobs="$3"

    if [ ! -d "$dir" ]; then
        echo "Skipping '$dir' (directory does not exist)"
        return 0
    fi

    echo "Check formatting of C/C++ code in '$dir'"
    find "$dir" \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) -print0 | \
        xargs -0 -P "$num_jobs" "$clang_format" --dry-run -Werror
    return $?
}

export -f check_dir

# Launch checks for all directories in parallel, capturing exit codes
pids=()
temp_dir=$(mktemp -d)

for dir in src include tests; do
    # Run check_dir in subshell background so all directories check in parallel
    (check_dir "$dir" "$clang_format_exe" "$NUM_JOBS" > "$temp_dir/$dir.out" 2>&1; echo $? > "$temp_dir/$dir.rc") &
    pids+=($!)
done

# Wait for all background jobs
for pid in "${pids[@]}"; do
    wait "$pid"
done

# Check results and display output
for dir in src include tests; do
    if [ -f "$temp_dir/$dir.out" ]; then
        cat "$temp_dir/$dir.out"
    fi
    if [ -f "$temp_dir/$dir.rc" ]; then
        rc=$(cat "$temp_dir/$dir.rc")
        if [ "$rc" -ne 0 ]; then
            check_rc=1
        fi
    fi
done

# Cleanup
rm -rf "$temp_dir"

cd "$curr_dir" || exit 1

if [ $check_rc -ne 0 ]; then
    echo "Some formatting checks failed. Please run 'autoformat.sh' to fix the issues."
    exit 2
else
    echo "All checks passed successfully."
    exit 0
fi
