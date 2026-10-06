#! /bin/sh
# Run clang-format 19, the major version .clang-format is written for and CI
# installs. Other versions disagree on DerivePointerAlignment ties, so a file
# formatted by one fails the check of another. Without a local clang-format 19,
# uvx runs the pinned release from PyPI.

MAJOR=19
PINNED=19.1.7

for exe in "${CLANG_FORMAT:-}" clang-format-$MAJOR clang-format; do
    [ -n "$exe" ] || continue
    command -v "$exe" >/dev/null 2>&1 || continue
    case "$("$exe" --version)" in
    *"clang-format version $MAJOR."*) exec "$exe" "$@" ;;
    esac
done

if command -v uvx >/dev/null 2>&1; then
    exec uvx -q --from "clang-format==$PINNED" clang-format "$@"
fi

echo "clang-format $MAJOR not found: install clang-format $MAJOR, set CLANG_FORMAT, or install uv" >&2
exit 1
