# SPDX-License-Identifier: MIT
import gzip
import os

import pytest

from dftracer.utils.trace import discover_trace_files


def _touch(path, gz=True):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if gz:
        with gzip.open(path, "wt") as out:
            out.write("{}\n")
    else:
        with open(path, "w") as out:
            out.write("{}\n")
    return path


@pytest.fixture()
def tree(tmp_path):
    root = str(tmp_path)
    return {
        "top": _touch(os.path.join(root, "a.pfw.gz")),
        "plain": _touch(os.path.join(root, "notes.txt"), gz=False),
        "nested": _touch(os.path.join(root, "sub", "b.pfw.gz")),
        "index": _touch(os.path.join(root, ".dftindex", "c.pfw.gz")),
        "root": root,
    }


def test_directory_lists_only_top_level_input_files(tree):
    assert discover_trace_files(tree["root"]) == [tree["top"]]


def test_recursive_enters_subdirectories_but_not_index_folders(tree):
    assert discover_trace_files(tree["root"], recursive=True) == sorted(
        [tree["top"], tree["nested"]]
    )


def test_glob_keeps_only_input_names(tree):
    assert discover_trace_files(os.path.join(tree["root"], "*")) == [tree["top"]]
    assert discover_trace_files(os.path.join(tree["root"], "*.zip")) == []


def test_file_gives_itself_when_its_name_is_an_input(tree):
    assert discover_trace_files(tree["nested"]) == [tree["nested"]]
    assert discover_trace_files(tree["plain"]) == []


def test_file_with_glob_characters_in_its_name_is_a_file(tmp_path):
    odd = _touch(str(tmp_path / "run[1].pfw.gz"))
    _touch(str(tmp_path / "run1.pfw.gz"))
    assert discover_trace_files(odd) == [odd]


def test_recursive_glob_reaches_nested_files(tree):
    found = discover_trace_files(os.path.join(tree["root"], "**", "*.gz"), recursive=True)
    assert tree["top"] in found and tree["nested"] in found


def test_path_object_is_accepted(tree, tmp_path):
    assert discover_trace_files(tmp_path) == [tree["top"]]


def test_missing_path_raises(tmp_path):
    with pytest.raises(FileNotFoundError, match="no trace file or directory"):
        discover_trace_files(str(tmp_path / "gone.pfw.gz"))
