#!/bin/sh
set -eu
LC_ALL=C
export LC_ALL

source_dir=$1
cxx=$2
scripts=$source_dir/scripts/layering
fixture=$source_dir/tests/layering/fixture
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ "$(uname -s)" = Darwin ]; then ext=dylib; else ext=so; fi
mkdir -p "$work/build/lib"
lib=$work/build/lib
"$cxx" -shared -fPIC -o "$lib/libdftracer_utils_fixture.0.0.2.$ext" "$fixture/lib/fixlib.cpp"
ln -s "libdftracer_utils_fixture.0.0.2.$ext" "$lib/libdftracer_utils_fixture.$ext"
cp "$lib/libdftracer_utils_fixture.0.0.2.$ext" "$lib/libdftracer_utils_fixture.0.0.1.$ext"
cp "$lib/libdftracer_utils_fixture.0.0.2.$ext" "$lib/libdftracer_utils_removed.$ext"

{
    echo "== component_cycles"
    python3 -I "$scripts/component_cycles.py" "$fixture"
    echo "== header_map"
    python3 -I "$scripts/header_map.py" "$fixture"
    echo "== library_report"
    sh "$scripts/library_report.sh" "$work/build" "$fixture" |
        awk '$1 == "library" {
                 print "library", $2, "symbols>=3", ($4 >= 3 ? "yes" : "no"), "bytes>0", ($6 > 0 ? "yes" : "no")
                 next
             }
             { print }'
} > "$work/actual.txt"

sh "$scripts/baseline.sh" "$work/build" "$fixture" > "$work/baseline1.txt"
sh "$scripts/baseline.sh" "$work/build" "$fixture" > "$work/baseline2.txt"
cmp "$work/baseline1.txt" "$work/baseline2.txt"

expected_lines=$(grep -vc '^==' "$work/actual.txt")
if [ "$(wc -l < "$work/baseline1.txt")" -ne "$expected_lines" ]; then
    echo "baseline.sh output does not hold every script line" >&2
    exit 1
fi

diff -u "$fixture/expected.txt" "$work/actual.txt"
