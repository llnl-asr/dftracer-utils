#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fails when this branch removes or changes a line of the plugin ABI manifest.

cmake/plugin_abi_manifest.txt is the recorded shape of the plugin ABI headers.
ctest `plugin_abi_manifest` keeps it in step with the headers, but an ABI
change is a promise to the plugins built against the last release, so the
comparison here is against the last state a release published:

  * the baseline is the newest release tag that carries the manifest. A tag
    counts only as v<major>.<minor>.<patch>: `pre-*`, `develop-latest` and the
    one uppercase `V0.0.8` are skipped, because no release publishes the file
    under those names,
  * an added line passes, and so does a struct that only grew: a new struct,
    field, enumerator, constant, typedef or function pointer, and a size that
    went up while the alignment and every old field of that struct stayed put,
    break no plugin built against the tag,
  * a removed or changed line fails, because a plugin built against the tag
    reads a field that is gone or at a new offset,
  * unless the abi major in VERSION is greater than at that tag, which is how
    a release says the plugin ABI is not compatible any more.

Until a release tag carries the file there is no baseline, and the job says so
and passes. ctest still guards every commit.

    scripts/check_plugin_abi_manifest_diff.py [--repo DIR]
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = "cmake/plugin_abi_manifest.txt"
VERSION = "VERSION"

# A published release tag. `pre-*`, `develop-latest` and the older uppercase
# `V0.0.8` are not one, on purpose.
RELEASE_TAG = re.compile(r"^v\d+\.\d+\.\d+$")

STRUCT_LINE = re.compile(r"^struct (\S+) size (\d+) align (\d+)$")


class Report:
    """What a manifest diff does to the plugin ABI."""

    def __init__(self):
        self.added = []
        self.grown = []
        self.changed = []
        self.removed = []
        self.major_bumped = False

    @property
    def problems(self):
        """The lines a release with this ABI major may not remove or change."""
        if self.major_bumped:
            return []
        return self.removed + self.changed


def parse_abi(version_text):
    """The (major, minor, patch) of the abi line in a VERSION text."""
    for line in version_text.splitlines():
        key, _, value = line.partition(":")
        if key.strip() != "abi":
            continue
        parts = [part for part in value.strip().split(".") if part != ""]
        if len(parts) != 3 or not all(part.isdigit() for part in parts):
            raise ValueError("VERSION: 'abi: %s' is not MAJOR.MINOR.PATCH" % value.strip())
        return tuple(int(part) for part in parts)
    raise ValueError("VERSION has no 'abi:' line")


def line_key(line):
    """The (kind, name) a manifest line is about; a change keeps it, a removal loses it."""
    fields = line.split(" ")
    if len(fields) < 2:
        raise ValueError("not a manifest line: %r" % line)
    return (fields[0], fields[1])


def entries(text):
    """The manifest lines by key, blank lines and a missing final newline ignored."""
    found = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        key = line_key(line)
        if key in found:
            raise ValueError("two manifest lines for %s %s" % key)
        found[key] = line
    return found


def is_release_tag(name):
    """True for v<major>.<minor>.<patch> and for nothing else."""
    return RELEASE_TAG.match(name) is not None


def release_tags(names):
    """The names that are release tags, in the order given."""
    return [name for name in names if is_release_tag(name)]


def struct_parts(line):
    """(name, size, align) of a struct line."""
    match = STRUCT_LINE.match(line)
    if match is None:
        raise ValueError("not a struct line: %r" % line)
    return (match.group(1), int(match.group(2)), int(match.group(3)))


def is_compatible_growth(old_line, new_line):
    """A struct that grew and kept its alignment breaks no plugin."""
    old = struct_parts(old_line)
    new = struct_parts(new_line)
    return old[0] == new[0] and old[2] == new[2] and new[1] >= old[1]


def compare(old_text, new_text, old_abi, new_abi):
    """The report of new_text against old_text, given the abi versions of each."""
    report = Report()
    report.major_bumped = new_abi[0] > old_abi[0]
    old = entries(old_text)
    new = entries(new_text)
    grown = []
    for key, line in new.items():
        if key not in old:
            report.added.append(line)
        elif old[key] == line:
            continue
        elif key[0] == "struct" and is_compatible_growth(old[key], line):
            grown.append((key, old[key], line))
        else:
            report.changed.append((old[key], line))
    for key, line in old.items():
        if key not in new:
            report.removed.append(line)
    broken = [line_key(line)[1] for line in report.removed]
    broken.extend(line_key(line[0])[1] for line in report.changed)
    for key, old_line, new_line in grown:
        name = key[1]
        if any(other.startswith(name + ".") for other in broken):
            report.changed.append((old_line, new_line))
        else:
            report.grown.append((old_line, new_line))
    return report


def _git(repo, *args):
    result = subprocess.run(
        ["git"] + list(args),
        cwd=repo,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        check=False,
    )
    return result.returncode, result.stdout


def baseline(repo, path=MANIFEST):
    """(tag, text) of the newest release tag that carries path, or None."""
    code, tags = _git(repo, "tag", "--sort=-v:refname")
    if code != 0:
        raise SystemExit("check_plugin_abi_manifest_diff: `git tag` failed in %s" % repo)
    for tag in release_tags(tags.split()):
        code, text = _git(repo, "show", "%s:%s" % (tag, path))
        if code == 0:
            return tag, text
    return None


def run(repo=ROOT, path=MANIFEST):
    """Compare the working manifest with the newest tagged one; 0 when it passes."""
    found = baseline(repo, path)
    if found is None:
        print(
            "check_plugin_abi_manifest_diff: no baseline exists yet (no release tag "
            "carries %s), nothing to compare" % path
        )
        return 0
    tag, old_text = found
    with open(os.path.join(repo, path), "r", encoding="utf-8") as handle:
        new_text = handle.read()
    code, old_version = _git(repo, "show", "%s:%s" % (tag, VERSION))
    if code != 0:
        raise SystemExit("check_plugin_abi_manifest_diff: tag %s has no %s" % (tag, VERSION))
    with open(os.path.join(repo, VERSION), "r", encoding="utf-8") as handle:
        new_version = handle.read()
    old_abi = parse_abi(old_version)
    new_abi = parse_abi(new_version)
    report = compare(old_text, new_text, old_abi, new_abi)
    for line in report.added:
        print("added:   %s" % line)
    for old_line, new_line in report.grown:
        print("grown:   %s\n     to: %s" % (old_line, new_line))
    for old_line, new_line in report.changed:
        print("changed: %s\n     to: %s" % (old_line, new_line))
    for line in report.removed:
        print("removed: %s" % line)
    if not report.problems:
        print(
            "check_plugin_abi_manifest_diff: ok against %s (abi %d.%d.%d -> %d.%d.%d)"
            % ((tag,) + old_abi + new_abi)
        )
        return 0
    print(
        "check_plugin_abi_manifest_diff: %d line(s) of the plugin ABI manifest that %s "
        "published are removed or changed, and the abi major in %s is still %d."
        % (len(report.problems), tag, VERSION, new_abi[0])
    )
    print("A plugin built against %s reads those fields and callbacks." % tag)
    print("Either keep them, or raise the abi major to %d in %s." % (old_abi[0] + 1, VERSION))
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=ROOT, help="repository root (default: %(default)s)")
    parser.add_argument("--manifest", default=MANIFEST, help="manifest path (default: %(default)s)")
    args = parser.parse_args(argv)
    return run(args.repo, args.manifest)


if __name__ == "__main__":
    sys.exit(main())
