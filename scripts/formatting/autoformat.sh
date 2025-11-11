#! /bin/bash

clang_format_exe="clang-format"
if [ $# -ge 1 ]; then
    clang_format_exe="$1"
fi

if ! command -v "$clang_format_exe" >/dev/null 2>&1; then
    echo "You must have 'clang-format' in PATH to use 'autoformat.sh'"
    exit 1
fi

if ! command -v find >/dev/null 2>&1; then
    echo "You must have 'find' in PATH to use 'autoformat.sh'"
    exit 1
fi

if ! command -v dirname >/dev/null 2>&1; then
    echo "You must have 'dirname' in PATH to use 'autoformat.sh'"
    exit 1
fi

if ! command -v xargs >/dev/null 2>&1; then
    echo "You must have 'xargs' in PATH to use 'autoformat.sh'"
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

# Format all directories in parallel
format_dir() {
    local dir="$1"
    local clang_format="$2"
    local num_jobs="$3"

    if [ ! -d "$dir" ]; then
        echo "Skipping '$dir' (directory does not exist)"
        return
    fi

    echo "Formatting C/C++ code in '$dir'"
    find "$dir" \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) -print0 | \
        xargs -0 -P "$num_jobs" "$clang_format" -i
}

export -f format_dir

# Launch formatting for all directories in parallel
for dir in src include tests; do
    format_dir "$dir" "$clang_format_exe" "$NUM_JOBS" &
done

# Wait for all background jobs to complete
wait

cd "$curr_dir" || exit 1
