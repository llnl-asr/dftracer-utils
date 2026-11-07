#! /bin/sh
# SPDX-License-Identifier: MIT
# Runs clang-tidy readability-identifier-naming (config in
# cmake/clang_tidy_naming.yaml) on every header in include/ except
# dstd/vendor, one run per header.
# Exits 77 (ctest skip) when no clang-tidy can be run, 1 on a naming error, a
# tool failure or when half of the headers do not parse.
# Usage: check_naming.sh <source dir> <build dir>
# CLANG_TIDY overrides the tool; without it uvx fetches the pinned major.

SRC=$1
BUILD=$2

if [ -n "${CLANG_TIDY:-}" ]; then
    set -- "$CLANG_TIDY"
elif command -v uvx >/dev/null 2>&1; then
    set -- uvx -q --from "clang-tidy>=19,<20" clang-tidy
else
    echo "naming: no clang-tidy and no uvx; skipped"
    exit 77
fi

if ! "$@" --version >/dev/null 2>&1; then
    echo "naming: clang-tidy cannot run; skipped"
    exit 77
fi

SYSROOT=
if command -v xcrun >/dev/null 2>&1; then
    SYSROOT="-isysroot$(xcrun --show-sdk-path 2>/dev/null)"
fi

JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cd "$SRC" || exit 1
TIDY="$*"
export TIDY SRC BUILD SYSROOT WORK
find include -name '*.h' ! -path '*/dstd/vendor/*' | sort >"$WORK/headers"
TOTAL=$(wc -l <"$WORK/headers" | tr -d ' ')
if [ "$TOTAL" -eq 0 ]; then
    echo "naming: no headers found under include/"
    exit 1
fi

xargs -P "$JOBS" -n 1 sh -c '
    id=$(printf "%s" "$1" | tr "/" "_")
    $TIDY --quiet --config-file="$SRC/cmake/clang_tidy_naming.yaml" \
        --header-filter="^\$" "$1" -- $SYSROOT -std=c++20 -x c++-header \
        -I"$SRC/include" -I"$BUILD/include" >"$WORK/$id.out" 2>&1
    echo $? >"$WORK/$id.rc"' sh <"$WORK/headers"

NAMING=0
TOOL=0
PARSE=0
for rcfile in "$WORK"/*.rc; do
    out=${rcfile%.rc}.out
    rc=$(cat "$rcfile")
    if grep -q "readability-identifier-naming" "$out"; then
        NAMING=1
        grep -E "readability-identifier-naming" "$out" | sort -u
    elif [ "$rc" -gt 1 ] || { [ "$rc" -ne 0 ] && [ ! -s "$out" ]; }; then
        TOOL=$((TOOL + 1))
        echo "naming: clang-tidy failed (status $rc) on a header:"
        head -3 "$out"
    elif grep -q "error:" "$out"; then
        PARSE=$((PARSE + 1))
        FIRST_PARSE=$(grep -m1 "error:" "$out")
    fi
done
RAN=$(ls "$WORK"/*.rc 2>/dev/null | wc -l | tr -d ' ')
ANALYSED=$((TOTAL - PARSE - TOOL))

if [ "$NAMING" -ne 0 ]; then
    echo "naming: identifier names break the rules in CLAUDE.md"
    exit 1
fi
if [ "$RAN" -ne "$TOTAL" ] || [ "$TOOL" -ne 0 ]; then
    echo "naming: clang-tidy failed on $TOOL of $TOTAL headers ($RAN ran)"
    exit 1
fi
if [ $((PARSE * 2)) -gt "$TOTAL" ]; then
    echo "naming: $PARSE of $TOTAL headers do not parse with this clang-tidy;" \
        "too few analysed. First error:"
    echo "$FIRST_PARSE"
    exit 1
fi
if [ "$PARSE" -ne 0 ]; then
    echo "naming: $PARSE headers do not parse with this clang-tidy (SDK mismatch), not naming; first error:"
    echo "$FIRST_PARSE"
fi
echo "naming: $ANALYSED of $TOTAL headers analysed, none break readability-identifier-naming"
