#!/bin/sh
set -eu
LC_ALL=C
export LC_ALL

if [ $# -lt 1 ]; then
    echo "usage: library_report.sh <build-dir> [<source-root>]" >&2
    exit 2
fi
build=$1
root=${2:-$(cd "$(dirname "$0")/../.." && pwd)}

if [ "$(uname -s)" = Darwin ]; then ext=dylib; else ext=so; fi

names=$(awk '
    /create_library\(/ { call = 1; want = 0; next }
    call && $1 == "NAME" { want = 1; next }
    call && want && NF { print $1; call = 0; want = 0 }
' "$root/src/CMakeLists.txt" | sort)

for name in $names; do
    f=$build/lib/lib$name.$ext
    if [ ! -e "$f" ]; then
        echo "library_report: $name is missing from $build/lib" >&2
        exit 1
    fi
    if [ "$ext" = dylib ]; then
        symbols=$(nm -gU "$f" | wc -l)
    else
        symbols=$(nm -D --defined-only "$f" | wc -l)
    fi
    bytes=$(wc -c < "$f")
    echo "library $name symbols $((symbols)) bytes $((bytes))"
done

headers=$(find "$root/include" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.hh' \) | wc -l)
echo "public_headers $((headers))"
