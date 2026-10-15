#!/bin/sh
set -eu
LC_ALL=C
export LC_ALL

if [ $# -lt 1 ]; then
    echo "usage: baseline.sh <build-dir> [<source-root>]" >&2
    exit 2
fi
build=$1
here=$(cd "$(dirname "$0")" && pwd)
root=${2:-$(cd "$here/../.." && pwd)}

cycles=$(python3 -I "$here/component_cycles.py" "$root")
headers=$(python3 -I "$here/header_map.py" "$root")
libraries=$(sh "$here/library_report.sh" "$build" "$root")

{
    printf '%s\n' "$cycles" | sed 's/^/component_cycles /'
    printf '%s\n' "$headers" | sed 's/^/header_map /'
    printf '%s\n' "$libraries" | sed 's/^/library_report /'
} | sort
