#!/usr/bin/env bash
#
# bump-version.sh -- bump the project version.
#
# The latest `v*` git tag is the source of truth for the version: the Python
# package derives its version from tags via setuptools_scm. The C++ build
# embeds the version as MAJOR/MINOR/PATCH variables in CMakeLists.txt, which
# this script rewrites to match. If the two ever disagree, the tag wins and
# CMakeLists.txt is corrected to it.
#
# This script only bumps the version in CMakeLists.txt. It does NOT create a
# git tag -- tagging the release is left to the maintainers.
#
# Usage:
#   scripts/bump-version.sh auto                  derive the bump from commits
#   scripts/bump-version.sh <major|minor|patch>   bump a component
#   scripts/bump-version.sh <X.Y.Z>               set an explicit version
#   scripts/bump-version.sh --show                print the current versions
#
# `auto` inspects Conventional Commit messages since the last `v*` tag:
#   - a `!` in the type/scope, or `BREAKING CHANGE` in a body -> major
#   - any `feat:` commit                                      -> minor
#   - anything else (fix, chore, ...)                          -> patch
#
# By default the script only edits CMakeLists.txt, leaving the change unstaged
# for you to review. Pass --commit to also commit it.
#
# Options:
#   --commit      commit the CMakeLists.txt change (chore: bump version ...)
#   --dry-run     print what would happen without changing anything
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE_FILE="${REPO_ROOT}/CMakeLists.txt"

die() { echo "error: $*" >&2; exit 1; }

# Latest v* tag reachable from HEAD, with the `v` stripped. Empty if none.
# Always exits 0 so callers under `set -e` can treat an absent tag as normal.
read_tag_version() {
  local tag
  tag=$(git -C "$REPO_ROOT" describe --tags --abbrev=0 --match 'v*' 2>/dev/null || true)
  if [[ -n "$tag" ]]; then
    echo "${tag#v}"
  fi
}

# Extract a single VERSION component from CMakeLists.txt. Tolerant of any
# whitespace before the number. Prints nothing (exit 0) if not found, so the
# caller can emit a clear error instead of `set -e` aborting silently. Uses
# sed rather than `grep -oP`, whose PCRE mode is absent on BSD/macOS grep.
read_cmake_component() {
  { sed -nE "s/.*set\(DFTRACER_UTILS_VERSION_$1[[:space:]]+([0-9]+).*/\1/p" "$CMAKE_FILE" | head -n1; } || true
}

# Version embedded in CMakeLists.txt.
read_cmake_version() {
  local major minor patch
  major=$(read_cmake_component MAJOR)
  minor=$(read_cmake_component MINOR)
  patch=$(read_cmake_component PATCH)
  [[ -n "$major" && -n "$minor" && -n "$patch" ]] || die "could not parse version from $CMAKE_FILE"
  echo "${major}.${minor}.${patch}"
}

# The version a bump is computed from: the tag if there is one, else whatever
# CMakeLists.txt currently holds.
base_version() {
  local tag_ver
  tag_ver="$(read_tag_version)"
  if [[ -n "$tag_ver" ]]; then
    echo "$tag_ver"
  else
    read_cmake_version
  fi
}

# Determine the bump level (major|minor|patch) from Conventional Commit
# messages on commits since the most recent v* tag (or the whole history
# if no tag exists).
detect_bump() {
  local last_tag range subjects
  last_tag=$(git -C "$REPO_ROOT" describe --tags --abbrev=0 --match 'v*' 2>/dev/null || true)
  if [[ -n "$last_tag" ]]; then
    range="${last_tag}..HEAD"
  else
    range="HEAD"
  fi

  if [[ -z "$(git -C "$REPO_ROOT" log "$range" --oneline 2>/dev/null)" ]]; then
    die "no commits since ${last_tag:-the start of history}; nothing to bump"
  fi

  # %B = raw body, %s = subject; scan every commit message in the range.
  if git -C "$REPO_ROOT" log "$range" --format='%B' | grep -qE '^BREAKING[ -]CHANGE'; then
    echo major; return
  fi
  # Any `!` anywhere in the type/scope prefix (before the first colon) marks a
  # breaking change. This tolerates every common (mis)placement: `feat!:`,
  # `feat(api)!:`, and `feat!(api):`.
  subjects=$(git -C "$REPO_ROOT" log "$range" --format='%s')
  if echo "$subjects" | grep -qE '^[a-z]+[^:]*!:|^[a-z]+[^:]*!\([^:]*\):'; then
    echo major; return
  fi
  # A `feat` commit bumps the minor version, with or without a scope and
  # tolerant of a stray `!` in either spot.
  if echo "$subjects" | grep -qE '^feat([!(][^:]*)?:'; then
    echo minor; return
  fi
  echo patch
}

BUMP=""
DO_COMMIT=0
DRY_RUN=0

for arg in "$@"; do
  case "$arg" in
    --commit)  DO_COMMIT=1 ;;
    --dry-run) DRY_RUN=1 ;;
    --show)
      echo "latest tag:      $(read_tag_version || echo '(none)')"
      echo "CMakeLists.txt:  $(read_cmake_version)"
      exit 0
      ;;
    -h|--help)
      sed -n '2,31p' "${BASH_SOURCE[0]}" | sed -E 's/^# ?//'
      exit 0
      ;;
    auto|major|minor|patch|*.*.*) BUMP="$arg" ;;
    *) die "unknown argument: $arg" ;;
  esac
done

[[ -n "$BUMP" ]] || die "specify auto, major, minor, patch, or an explicit X.Y.Z version (see --help)"

if [[ "$BUMP" == "auto" ]]; then
  BUMP="$(detect_bump)"
  echo "detected bump from commits: $BUMP"
fi

CURRENT="$(base_version)"
IFS='.' read -r CUR_MAJOR CUR_MINOR CUR_PATCH <<< "$CURRENT"

case "$BUMP" in
  major) NEW_MAJOR=$((CUR_MAJOR + 1)); NEW_MINOR=0;                 NEW_PATCH=0 ;;
  minor) NEW_MAJOR=$CUR_MAJOR;         NEW_MINOR=$((CUR_MINOR + 1)); NEW_PATCH=0 ;;
  patch) NEW_MAJOR=$CUR_MAJOR;         NEW_MINOR=$CUR_MINOR;         NEW_PATCH=$((CUR_PATCH + 1)) ;;
  *.*.*)
    [[ "$BUMP" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "invalid version: $BUMP"
    IFS='.' read -r NEW_MAJOR NEW_MINOR NEW_PATCH <<< "$BUMP"
    ;;
esac

NEW="${NEW_MAJOR}.${NEW_MINOR}.${NEW_PATCH}"
CMAKE_VER="$(read_cmake_version)"

echo "base version (tag): $CURRENT"
echo "new version:        $NEW"
[[ "$CMAKE_VER" != "$CURRENT" ]] && \
  echo "note: CMakeLists.txt was $CMAKE_VER; it will be synced to $NEW"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "(dry run) would update $CMAKE_FILE"
  [[ "$DO_COMMIT" -eq 1 ]] && echo "(dry run) would commit the change"
  exit 0
fi

# Match whatever digits are currently in CMakeLists.txt, so the rewrite works
# even when CMakeLists.txt has drifted from the tag. Write through a temp file
# rather than `sed -i`, whose syntax differs between GNU and BSD/macOS sed.
TMP_CMAKE="$(mktemp)"
sed -E \
  -e "s/(set\(DFTRACER_UTILS_VERSION_MAJOR[[:space:]]+)[0-9]+/\1${NEW_MAJOR}/" \
  -e "s/(set\(DFTRACER_UTILS_VERSION_MINOR[[:space:]]+)[0-9]+/\1${NEW_MINOR}/" \
  -e "s/(set\(DFTRACER_UTILS_VERSION_PATCH[[:space:]]+)[0-9]+/\1${NEW_PATCH}/" \
  "$CMAKE_FILE" > "$TMP_CMAKE"
cat "$TMP_CMAKE" > "$CMAKE_FILE"
rm -f "$TMP_CMAKE"

[[ "$(read_cmake_version)" == "$NEW" ]] || die "version update failed; check $CMAKE_FILE"
echo "updated $CMAKE_FILE"

if [[ "$DO_COMMIT" -eq 0 ]]; then
  echo "done (CMakeLists.txt edited; not committed -- pass --commit to commit)"
  exit 0
fi

git -C "$REPO_ROOT" add CMakeLists.txt
git -C "$REPO_ROOT" commit -m "chore: bump version to ${NEW}"
echo "committed version bump"
echo
echo "done. push the commit; a maintainer tags the release as v${NEW}."
