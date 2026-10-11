"""The choice of PyPI prereleases that scripts/ci/pypi_cleanup_prereleases.py
deletes after a release."""

import importlib.util
import pathlib
import re

_SCRIPT = (
    pathlib.Path(__file__).resolve().parents[2] / "scripts" / "ci" / "pypi_cleanup_prereleases.py"
)


def _load_script():
    spec = importlib.util.spec_from_file_location("pypi_cleanup_prereleases", _SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cleanup = _load_script()


def test_keeps_highest_post_number_not_newest_string():
    versions = ["0.0.14.post9.dev0", "0.0.14.post10.dev0", "0.0.14.post2.dev0"]
    keep, delete = cleanup.plan(versions, "0.0.14")
    assert keep == "0.0.14.post10.dev0"
    assert delete == ["0.0.14.post2.dev0", "0.0.14.post9.dev0"]


def test_dev_number_breaks_a_tie_on_post_number():
    keep, delete = cleanup.plan(["0.0.14.post5.dev1", "0.0.14.post5.dev0"], "0.0.14")
    assert keep == "0.0.14.post5.dev1"
    assert delete == ["0.0.14.post5.dev0"]


def test_leaves_releases_and_other_versions_alone():
    versions = [
        "0.0.13",
        "0.0.13.post27.dev0",
        "0.0.14",
        "0.0.14.post3",
        "0.0.14.post3.dev0",
        "0.0.14.post4.dev0",
        "0.0.140.post1.dev0",
        "10.0.14.post1.dev0",
        "0.0.14.post1.dev0.extra",
        "0.0.15",
        "0.0.15.post1.dev0",
    ]
    keep, delete = cleanup.plan(versions, "0.0.14")
    assert keep == "0.0.14.post4.dev0"
    assert delete == ["0.0.14.post3.dev0"]


def test_single_prerelease_is_kept():
    assert cleanup.plan(["0.0.13", "0.0.13.post27.dev0"], "0.0.13") == ("0.0.13.post27.dev0", [])


def test_no_prerelease_deletes_nothing():
    assert cleanup.plan(["0.0.13", "0.0.14"], "0.0.14") == (None, [])


def test_version_dots_are_literal():
    assert cleanup.plan(["0x0y14.post1.dev0", "0.0.14.post2.dev0"], "0.0.14") == (
        "0.0.14.post2.dev0",
        [],
    )


def test_delete_regex_matches_exactly_the_delete_list():
    versions = [
        "0.0.14",
        "0.0.14.post1.dev0",
        "0.0.14.post10.dev0",
        "0.0.14.post11.dev0",
        "0.0.14.post1.dev01",
    ]
    keep, delete = cleanup.plan(versions, "0.0.14")
    regex = re.compile(cleanup.delete_regex(delete))
    # pypi-cleanup selects a version when the regex matches it with re.match.
    assert [v for v in versions if regex.match(v)] == [v for v in versions if v in delete]
    assert not regex.match(keep)
