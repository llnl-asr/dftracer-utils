# SPDX-License-Identifier: MIT
"""The rule scripts/check_plugin_abi_manifest_diff.py enforces, on manifests
held in memory, so the rule is tested without a git tag to compare against.

An added manifest line passes; a removed or changed one fails unless the abi
major in VERSION moved past the one the baseline tag published."""

import importlib.util
import pathlib

import pytest

_SCRIPT = (
    pathlib.Path(__file__).resolve().parents[2] / "scripts" / "check_plugin_abi_manifest_diff.py"
)


def _load_script():
    spec = importlib.util.spec_from_file_location("check_plugin_abi_manifest_diff", _SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


OLD_MANIFEST = """\
const DFTU_STR_NONE 4294967295
enum DFTU_T_STR 3
field dftu_bytes.len offset 8 size 8
fnptr dftu_bytes.free_fn void (*)(void* data, void* ud)
struct dftu_bytes size 16 align 8
"""

# A field past the end of dftu_bytes: an addition every plugin still reads.
ADDED_MANIFEST = OLD_MANIFEST + "field dftu_bytes.ud offset 16 size 8\n"

# The same field, recorded at a new offset: an existing plugin reads the wrong
# bytes, so the manifest alone cannot pass.
CHANGED_MANIFEST = OLD_MANIFEST.replace(
    "field dftu_bytes.len offset 8 size 8", "field dftu_bytes.len offset 4 size 8"
)

# A dropped field.
REMOVED_MANIFEST = OLD_MANIFEST.replace("field dftu_bytes.len offset 8 size 8\n", "")


# A field appended at the end of a struct, with the size line that comes with it:
# a plugin built against the old size still finds every field it reads.
APPENDED_MANIFEST = OLD_MANIFEST.replace(
    "field dftu_bytes.len offset 8 size 8\n",
    "field dftu_bytes.len offset 8 size 8\nfield dftu_bytes.ud offset 16 size 8\n",
).replace("struct dftu_bytes size 16 align 8", "struct dftu_bytes size 24 align 8")


def _version(abi):
    return f"api: 0.0.0\nabi: {abi}\nindex: 0\nduql: 0.0\n"


@pytest.fixture(name="module")
def fixture_module():
    return _load_script()


def test_added_line_passes(module):
    report = module.compare(
        OLD_MANIFEST, ADDED_MANIFEST, module.parse_abi(_version("0.1.0")), (0, 1, 0)
    )
    assert report.added == ["field dftu_bytes.ud offset 16 size 8"]
    assert report.problems == []


def test_removed_line_fails(module):
    report = module.compare(
        OLD_MANIFEST, REMOVED_MANIFEST, module.parse_abi(_version("0.1.0")), (0, 1, 0)
    )
    assert report.removed == ["field dftu_bytes.len offset 8 size 8"]
    assert report.problems == report.removed


def test_changed_line_fails(module):
    report = module.compare(
        OLD_MANIFEST, CHANGED_MANIFEST, module.parse_abi(_version("0.1.0")), (0, 1, 0)
    )
    assert report.changed == [
        ("field dftu_bytes.len offset 8 size 8", "field dftu_bytes.len offset 4 size 8")
    ]
    assert report.problems == report.changed


def test_a_minor_bump_does_not_excuse_a_removal(module):
    report = module.compare(
        OLD_MANIFEST, REMOVED_MANIFEST, module.parse_abi(_version("0.1.0")), (0, 2, 0)
    )
    assert report.problems


def test_a_major_bump_allows_a_removal(module):
    report = module.compare(
        OLD_MANIFEST, REMOVED_MANIFEST, module.parse_abi(_version("0.1.0")), (1, 0, 0)
    )
    assert report.removed
    assert report.problems == []


def test_a_patch_bump_does_not_excuse_a_change(module):
    report = module.compare(
        OLD_MANIFEST, CHANGED_MANIFEST, module.parse_abi(_version("0.1.0")), (0, 1, 1)
    )
    assert report.problems == report.changed


def test_appended_field_with_a_grown_size_line_passes(module):
    report = module.compare(OLD_MANIFEST, APPENDED_MANIFEST, (0, 1, 0), (0, 1, 0))
    assert report.added == ["field dftu_bytes.ud offset 16 size 8"]
    assert report.grown == [
        ("struct dftu_bytes size 16 align 8", "struct dftu_bytes size 24 align 8")
    ]
    assert report.problems == []


def test_shrunk_struct_fails(module):
    shrunk = OLD_MANIFEST.replace(
        "struct dftu_bytes size 16 align 8", "struct dftu_bytes size 12 align 8"
    )
    report = module.compare(OLD_MANIFEST, shrunk, (0, 1, 0), (0, 1, 0))
    assert report.grown == []
    assert report.changed == [
        ("struct dftu_bytes size 16 align 8", "struct dftu_bytes size 12 align 8")
    ]
    assert report.problems == report.changed


def test_a_growth_that_moves_a_field_fails(module):
    both = APPENDED_MANIFEST.replace(
        "field dftu_bytes.len offset 8 size 8", "field dftu_bytes.len offset 4 size 8"
    )
    report = module.compare(OLD_MANIFEST, both, (0, 1, 0), (0, 1, 0))
    assert report.grown == []
    assert len(report.changed) == 2
    assert report.problems


def test_a_growth_that_changes_the_alignment_fails(module):
    moved = OLD_MANIFEST.replace(
        "struct dftu_bytes size 16 align 8", "struct dftu_bytes size 32 align 4"
    )
    report = module.compare(OLD_MANIFEST, moved, (0, 1, 0), (0, 1, 0))
    assert report.grown == []
    assert report.problems


def test_release_tags_skip_pre_releases_and_develop(module):
    names = [
        "develop-latest",
        "pre-0.0.14.post2.dev0",
        "V0.0.8",
        "v0.0.8",
        "v1.2.3",
        "v1.2.3-rc1",
        "v1.2",
        "v1.2.3.4",
        "1.2.3",
        "v0.0.14",
    ]
    assert module.release_tags(names) == ["v0.0.8", "v1.2.3", "v0.0.14"]


def test_parse_abi_reads_the_abi_line_only(module):
    assert module.parse_abi(_version("2.3.4")) == (2, 3, 4)
    with pytest.raises(ValueError):
        module.parse_abi("api: 0.0.0\n")


def test_entries_rejects_two_lines_for_one_fact(module):
    doubled = OLD_MANIFEST + "field dftu_bytes.len offset 8 size 8\n"
    with pytest.raises(ValueError):
        module.entries(doubled)
