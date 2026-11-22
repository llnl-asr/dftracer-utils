#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""One input rule for TraceViewer and Indexer, and a forced build next to a
trace folder that shares its index_dir."""

import gzip
import os
import pathlib
import time

import pytest

import dftracer.utils as dftu

from .common import Environment


def _viewer(files, index_dir):
    return dftu.trace.TraceViewer(files, index_dir=index_dir)


def _indexer(files, index_dir):
    return dftu.index.Indexer(files, index_dir=index_dir)


def _rows(files, index_dir):
    return _viewer(files, index_dir).lazy().collect().num_rows


class TestOneInputRule:
    @pytest.mark.parametrize("make", [_viewer, _indexer], ids=["TraceViewer", "Indexer"])
    def test_a_pathlike_works_alone_and_in_a_list(self, make):
        with Environment() as env:
            trace = env.create_test_gzip_file()
            index_dir = os.path.join(env.temp_dir, "idx")
            make(pathlib.Path(trace), index_dir)
            make([pathlib.Path(trace)], index_dir)
            make((pathlib.Path(trace), trace), index_dir)

    def test_pathlike_reads_the_same_rows_as_str(self):
        with Environment() as env:
            trace = env.create_test_gzip_file()
            index_dir = os.path.join(env.temp_dir, "idx")
            expected = _rows(trace, index_dir)
            assert expected > 0
            assert _rows(pathlib.Path(trace), index_dir) == expected
            assert _rows([pathlib.Path(trace)], index_dir) == expected

    @pytest.mark.parametrize("make", [_viewer, _indexer], ids=["TraceViewer", "Indexer"])
    @pytest.mark.parametrize("bad", [b"x.pfw.gz", [b"x.pfw.gz"], [3], 3])
    def test_bytes_and_other_types_are_type_errors(self, make, bad):
        with Environment() as env:
            with pytest.raises(TypeError):
                make(bad, os.path.join(env.temp_dir, "idx"))

    def test_a_repeated_path_is_read_once(self):
        with Environment() as env:
            trace = env.create_test_gzip_file()
            index_dir = os.path.join(env.temp_dir, "idx")
            once = _rows([trace], index_dir)
            assert _rows([trace, trace, pathlib.Path(trace)], index_dir) == once
            status = _indexer([trace, trace, pathlib.Path(trace)], index_dir).status()
            assert status.total_files == 1
            assert status.needs_work == [trace]

    def test_duplicates_keep_the_first_occurrence_order(self):
        with Environment() as env:
            first = env.create_dft_trace_file_with_pid("a.pfw.gz", 1)
            second = env.create_dft_trace_file_with_pid("b.pfw.gz", 2)
            status = _indexer(
                [second, first, second, first], os.path.join(env.temp_dir, "idx")
            ).status()
            assert status.needs_work == [second, first]

    def test_an_empty_list_is_a_value_error_for_the_indexer_only(self):
        with Environment() as env:
            index_dir = os.path.join(env.temp_dir, "idx")
            with pytest.raises(ValueError, match="at least one path"):
                _indexer([], index_dir)
            # A shard with no files is a view of no rows.
            assert _rows([], index_dir) == 0

    def test_a_missing_path_is_not_found(self):
        with Environment() as env:
            trace = env.create_test_gzip_file()
            index_dir = os.path.join(env.temp_dir, "idx")
            for spec in (
                os.path.join(env.temp_dir, "nope.pfw.gz"),
                [trace, os.path.join(env.temp_dir, "nope.pfw.gz")],
            ):
                # Indexer stats its files on the first call, TraceViewer reads
                # them when the plan runs.
                with pytest.raises(dftu.errors.DFTUtilsNotFoundError):
                    _indexer(spec, index_dir).status()
                with pytest.raises(dftu.errors.DFTUtilsError):
                    _viewer(spec, index_dir).lazy().collect()

    def test_an_empty_directory_has_no_files(self):
        with Environment() as env:
            empty = os.path.join(env.temp_dir, "empty")
            os.makedirs(empty)
            index_dir = os.path.join(env.temp_dir, "idx")
            assert _rows(empty, index_dir) == 0
            status = _indexer(empty, index_dir).status()
            assert status.total_files == 0
            assert status.needs_work == []

    def test_a_non_trace_file_fails_when_it_is_read(self):
        with Environment() as env:
            junk = os.path.join(env.temp_dir, "notes.txt")
            pathlib.Path(junk).write_text("hello\n")
            index_dir = os.path.join(env.temp_dir, "idx")
            viewer = _viewer(junk, index_dir)
            with pytest.raises(dftu.errors.DFTUtilsIOError):
                viewer.lazy().collect()
            indexer = _indexer(junk, index_dir)
            assert indexer.status().needs_work == [junk]
            with pytest.raises(dftu.errors.DFTUtilsIOError):
                indexer.ensure()


def _tree(env):
    """root/a.pfw.gz, b.pfw.gz, x1.pfw.gz, x[1].pfw.gz, sub/c.pfw.gz,
    sub/deep/d.pfw.gz, other/e.pfw.gz, notes.txt; one pid per file."""
    names = [
        "a.pfw.gz",
        "b.pfw.gz",
        "x1.pfw.gz",
        "x[1].pfw.gz",
        "sub/c.pfw.gz",
        "sub/deep/d.pfw.gz",
        "other/e.pfw.gz",
    ]
    paths = {}
    for pid, name in enumerate(names, start=1):
        paths[name] = env.create_dft_trace_file_with_pid(name, pid, num_events=5)
    notes = os.path.join(env.temp_dir, "notes.txt")
    pathlib.Path(notes).write_text("not a trace\n")
    paths["notes.txt"] = notes
    return paths


def _pids(files, index_dir):
    return sorted(set(_viewer(files, index_dir).lazy().collect().to_dict()["pid"]))


def _listed(files, index_dir):
    return _indexer(files, index_dir).status().needs_work


@pytest.mark.parametrize("make", [_viewer, _indexer], ids=["TraceViewer", "Indexer"])
class TestEveryInputForm:
    def test_a_directory_is_scanned_recursively(self, make):
        with Environment() as env:
            paths = _tree(env)
            idx = os.path.join(env.temp_dir, "idx")
            expected = sorted(v for k, v in paths.items() if k.endswith(".pfw.gz"))
            if make is _indexer:
                assert sorted(_listed(env.temp_dir, idx)) == expected
            else:
                assert _pids(env.temp_dir, idx) == [1, 2, 3, 4, 5, 6, 7]

    def test_a_list_of_directories_and_a_mixed_list(self, make):
        with Environment() as env:
            paths = _tree(env)
            idx = os.path.join(env.temp_dir, "idx")
            sub = os.path.join(env.temp_dir, "sub")
            other = os.path.join(env.temp_dir, "other")
            make([sub, other], idx)
            if make is _indexer:
                assert _listed([sub, other], idx) == [
                    paths["sub/c.pfw.gz"],
                    paths["sub/deep/d.pfw.gz"],
                    paths["other/e.pfw.gz"],
                ]
                assert _listed([paths["a.pfw.gz"], sub, paths["b.pfw.gz"]], idx) == [
                    paths["a.pfw.gz"],
                    paths["sub/c.pfw.gz"],
                    paths["sub/deep/d.pfw.gz"],
                    paths["b.pfw.gz"],
                ]
            else:
                assert _pids([sub, other], idx) == [5, 6, 7]
                assert _pids([paths["a.pfw.gz"], sub, paths["b.pfw.gz"]], idx) == [1, 2, 5, 6]

    def test_overlapping_inputs_and_symlinks_name_a_file_once(self, make):
        with Environment() as env:
            paths = _tree(env)
            idx = os.path.join(env.temp_dir, "idx")
            sub = os.path.join(env.temp_dir, "sub")
            link = os.path.join(env.temp_dir, "linked")
            os.symlink(sub, link)
            alias = os.path.join(env.temp_dir, "alias.pfw.gz")
            os.symlink(paths["a.pfw.gz"], alias)
            spec = [sub, link, paths["sub/c.pfw.gz"], env.temp_dir, alias]
            if make is _indexer:
                listed = _listed(spec, idx)
                assert len(listed) == 7
                assert len({os.path.realpath(p) for p in listed}) == 7
                assert listed[:2] == [paths["sub/c.pfw.gz"], paths["sub/deep/d.pfw.gz"]]
            else:
                rows = _viewer(spec, idx).lazy().collect().num_rows
                real = [v for k, v in paths.items() if k.endswith(".pfw.gz")]
                assert rows == _rows(real, idx)

    def test_glob_star_question_and_classes(self, make):
        with Environment() as env:
            paths = _tree(env)
            idx = os.path.join(env.temp_dir, "idx")
            root = env.temp_dir
            if make is _indexer:
                assert _listed(os.path.join(root, "*.pfw.gz"), idx) == [
                    paths["a.pfw.gz"],
                    paths["b.pfw.gz"],
                    paths["x1.pfw.gz"],
                    paths["x[1].pfw.gz"],
                ]
                assert _listed(os.path.join(root, "x?.pfw.gz"), idx) == [paths["x1.pfw.gz"]]
                assert _listed(os.path.join(root, "[ab].pfw.gz"), idx) == [
                    paths["a.pfw.gz"],
                    paths["b.pfw.gz"],
                ]
                assert _listed(os.path.join(root, "[!a]*.pfw.gz"), idx) == [
                    paths["b.pfw.gz"],
                    paths["x1.pfw.gz"],
                    paths["x[1].pfw.gz"],
                ]
                assert _listed(os.path.join(root, "[^ab]?.pfw.gz"), idx) == [paths["x1.pfw.gz"]]
                assert _listed(os.path.join(root, "*", "*.pfw.gz"), idx) == [
                    paths["other/e.pfw.gz"],
                    paths["sub/c.pfw.gz"],
                ]
            else:
                assert _pids(os.path.join(root, "*.pfw.gz"), idx) == [1, 2, 3, 4]
                assert _pids(os.path.join(root, "x?.pfw.gz"), idx) == [3]
                assert _pids(os.path.join(root, "[ab].pfw.gz"), idx) == [1, 2]
                assert _pids(os.path.join(root, "[!a]*.pfw.gz"), idx) == [2, 3, 4]
                assert _pids(os.path.join(root, "*", "*.pfw.gz"), idx) == [5, 7]

    def test_glob_double_star_alone_and_in_a_list(self, make):
        with Environment() as env:
            paths = _tree(env)
            idx = os.path.join(env.temp_dir, "idx")
            root = env.temp_dir
            if make is _indexer:
                assert _listed(os.path.join(root, "**", "*.pfw.gz"), idx) == sorted(
                    v for k, v in paths.items() if k.endswith(".pfw.gz")
                )
                assert _listed(os.path.join(root, "**", "d.pfw.gz"), idx) == [
                    paths["sub/deep/d.pfw.gz"]
                ]
                assert _listed(
                    [paths["a.pfw.gz"], os.path.join(root, "sub", "**", "*.pfw.gz")], idx
                ) == [paths["a.pfw.gz"], paths["sub/c.pfw.gz"], paths["sub/deep/d.pfw.gz"]]
            else:
                assert _pids(os.path.join(root, "**", "*.pfw.gz"), idx) == [1, 2, 3, 4, 5, 6, 7]
                assert _pids(os.path.join(root, "**", "d.pfw.gz"), idx) == [6]
                assert _pids(
                    [paths["a.pfw.gz"], os.path.join(root, "sub", "**", "*.pfw.gz")], idx
                ) == [1, 5, 6]

    def test_an_existing_file_with_a_wildcard_in_its_name_is_taken_literally(self, make):
        with Environment() as env:
            paths = _tree(env)
            idx = os.path.join(env.temp_dir, "idx")
            if make is _indexer:
                assert _listed(paths["x[1].pfw.gz"], idx) == [paths["x[1].pfw.gz"]]
            else:
                assert _pids(paths["x[1].pfw.gz"], idx) == [4]

    def test_a_glob_that_matches_nothing_is_an_empty_input(self, make):
        with Environment() as env:
            _tree(env)
            idx = os.path.join(env.temp_dir, "idx")
            nothing = os.path.join(env.temp_dir, "*.nothing")
            if make is _indexer:
                for spec in (nothing, [nothing]):
                    status = _indexer(spec, idx).status()
                    assert status.total_files == 0
                    assert status.needs_work == []
            else:
                assert _rows(nothing, idx) == 0
                assert _rows([nothing], idx) == 0

    def test_a_file_that_is_named_is_taken_whatever_its_name(self, make):
        with Environment() as env:
            paths = _tree(env)
            idx = os.path.join(env.temp_dir, "idx")
            notes = paths["notes.txt"]
            if make is _indexer:
                assert _listed([notes], idx) == [notes]
            else:
                with pytest.raises(dftu.errors.DFTUtilsIOError):
                    _viewer(notes, idx).lazy().collect()


class TestHardLinksAndStagingNames:
    def test_a_hard_link_names_its_file_once(self):
        with Environment() as env:
            a = env.create_dft_trace_file_with_pid("a.pfw.gz", 1, num_events=5)
            os.link(a, os.path.join(env.temp_dir, "b.pfw.gz"))
            idx = os.path.join(env.temp_dir, "idx")
            assert _listed([env.temp_dir, a], idx) == [a]
            assert _listed([os.path.join(env.temp_dir, "b.pfw.gz"), a], idx) == [
                os.path.join(env.temp_dir, "b.pfw.gz")
            ]
            assert _rows([env.temp_dir, a], idx) == _rows(a, idx)

    def test_staging_folders_and_index_folders_are_not_read(self):
        with Environment() as env:
            a = env.create_dft_trace_file_with_pid("a.pfw.gz", 1, num_events=5)
            env.create_dft_trace_file_with_pid(".out.tmp-1/p.pfw.gz", 2, num_events=5)
            env.create_dft_trace_file_with_pid(".out.old-2/p.pfw.gz", 3, num_events=5)
            env.create_dft_trace_file_with_pid(".dftindex-x/p.pfw.gz", 4, num_events=5)
            idx = os.path.join(env.temp_dir, "idx")
            root = env.temp_dir
            assert _rows(os.path.join(root, "*", "*.gz"), idx) == 0
            for spec in (root, os.path.join(root, "**", "*.pfw.gz")):
                assert _pids(spec, idx) == [1]
            assert _listed(root, idx) == [a]
            assert _listed(os.path.join(root, "**", "*.pfw.gz"), idx) == [a]
            assert _pids(root, idx) == [1]


class TestTwoFoldersOneIndexRoot:
    def test_two_folders_in_one_list_share_an_index_root(self):
        with Environment() as env:
            a = env.create_dft_trace_file_with_pid("A/a.pfw.gz", 1, num_events=5)
            b = env.create_dft_trace_file_with_pid("B/b.pfw.gz", 2, num_events=5)
            shared = os.path.join(env.temp_dir, "shared_index")
            folders = [os.path.dirname(a), os.path.dirname(b)]
            both = _indexer(folders, shared)
            assert both.status().needs_work == [a, b]
            assert both.ensure().needs_work == []
            assert _pids(folders, shared) == [1, 2]
            assert _indexer(folders, shared).status().needs_work == []

    def test_a_rebuild_of_one_folder_keeps_the_data_of_the_other(self):
        with Environment() as env:
            a = env.create_dft_trace_file_with_pid("A/a.pfw.gz", 1, num_events=5)
            b = env.create_dft_trace_file_with_pid("B/b.pfw.gz", 2, num_events=5)
            shared = os.path.join(env.temp_dir, "shared_index")
            folders = [os.path.dirname(a), os.path.dirname(b)]
            assert _indexer(folders, shared).ensure().needs_work == []
            before = _counts(folders, shared)

            only_a = _indexer(os.path.dirname(a), shared)
            assert only_a.build(force=True).needs_work == []

            assert _indexer(folders, shared).status().needs_work == []
            assert _counts(folders, shared) == before
            assert _pids(folders, shared) == [1, 2]

    def test_a_changed_file_in_one_folder_rebuilds_only_that_folder(self):
        with Environment() as env:
            a = env.create_dft_trace_file_with_pid("A/a.pfw.gz", 1, num_events=5)
            b = env.create_dft_trace_file_with_pid("B/b.pfw.gz", 2, num_events=5)
            shared = os.path.join(env.temp_dir, "shared_index")
            folders = [os.path.dirname(a), os.path.dirname(b)]
            assert _indexer(folders, shared).ensure().needs_work == []
            env.create_dft_trace_file_with_pid("A/a.pfw.gz", 1, num_events=9)
            assert _indexer(folders, shared).status().needs_work == [a]
            assert _indexer(folders, shared).ensure().needs_work == []
            assert _rows(folders, shared) == _rows(a, shared) + _rows(b, shared)


class TestPathParsing:
    @pytest.mark.skipif(
        bool(os.environ.get("DFTRACER_UTILS_VALGRIND")),
        reason="a time limit cannot hold under valgrind",
    )
    def test_a_hundred_thousand_distinct_paths_construct_quickly(self):
        with Environment() as env:
            paths = [f"{env.temp_dir}/some/long/dir/trace-{i:08d}.pfw.gz" for i in range(100_000)]
            start = time.monotonic()
            _indexer(paths, os.path.join(env.temp_dir, "idx"))
            assert time.monotonic() - start < 2.0

    def test_a_fspath_that_shrinks_the_list_is_safe(self):
        with Environment() as env:
            trace = env.create_test_gzip_file()
            items = []

            class Shrinks:
                def __fspath__(self):
                    items.clear()
                    return trace

            items.extend([Shrinks(), trace, trace, trace])
            status = _indexer(items, os.path.join(env.temp_dir, "idx")).status()
            assert status.needs_work == [trace]


def _write_named(path, name, count=20):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with gzip.open(path, "wt") as f:
        for i in range(count):
            f.write(
                f'{{"name":"{name}","cat":"POSIX","pid":1,"tid":1,"ts":{1000000 + i * 1000},'
                f'"dur":100,"ph":1,"args":{{}}}}\n'
            )


def _counts(files, index_dir):
    df = _viewer(files, index_dir).group_by("name").agg("count").collect().to_dict()
    return dict(zip(df["name"], df[next(k for k in df if k != "name")]))


class TestForcedBuildKeepsOtherFolders:
    def test_the_forced_folder_drops_its_old_names_and_matches_a_fresh_index(self):
        with Environment() as env:
            a = os.path.join(env.temp_dir, "A", "a.pfw.gz")
            b = os.path.join(env.temp_dir, "B", "b.pfw.gz")
            _write_named(a, "old_a")
            _write_named(b, "name_b")
            shared = os.path.join(env.temp_dir, "shared_index")
            ia = _indexer(a, shared)
            ib = _indexer(b, shared)
            ia.ensure()
            ib.ensure()
            _write_named(a, "new_a", 7)
            assert ia.build(force=True).needs_work == []
            assert ib.status().needs_work == []
            both = _counts([a, b], shared)
            assert set(both) == {"new_a", "name_b"}
            assert both == _counts([a, b], os.path.join(env.temp_dir, "fresh_index"))
            assert both["new_a"] == 7

    def test_a_forced_build_of_one_folder_leaves_the_other_clean(self):
        with Environment() as env:
            a = env.create_dft_trace_file_with_pid("A/a.pfw.gz", 1)
            b = env.create_dft_trace_file_with_pid("B/b.pfw.gz", 2)
            index_dir = os.path.join(env.temp_dir, "shared_index")
            ia = _indexer(os.path.dirname(a), index_dir)
            ib = _indexer(os.path.dirname(b), index_dir)
            assert ia.ensure().needs_work == []
            assert ib.ensure().needs_work == []

            def counts():
                view = _viewer(os.path.dirname(b), index_dir)
                return view.group_by("name").agg("count").collect().to_dict()

            before = counts()
            assert before

            forced = ia.build(force=True)
            assert forced.needs_work == []
            assert ib.status().needs_work == []
            assert ib.build().needs_work == []
            assert counts() == before
