#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""One input rule for TraceViewer and Indexer, and a forced build next to a
trace folder that shares its index_dir."""

import os
import pathlib

import pytest

import dftracer.utils as dftu

from .common import Environment


def _viewer(files, index_dir):
    return dftu.TraceViewer(files, index_dir=index_dir)


def _indexer(files, index_dir):
    return dftu.Indexer(files, index_dir=index_dir)


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

    @pytest.mark.parametrize("make", [_viewer, _indexer], ids=["TraceViewer", "Indexer"])
    def test_a_directory_in_a_list_is_a_value_error(self, make):
        with Environment() as env:
            env.create_test_gzip_file()
            with pytest.raises(ValueError, match="directory"):
                make([env.temp_dir], os.path.join(env.temp_dir, "idx"))

    def test_an_empty_list_is_a_value_error_for_the_indexer_only(self):
        with Environment() as env:
            index_dir = os.path.join(env.temp_dir, "idx")
            with pytest.raises(ValueError, match="at least one path"):
                _indexer([], index_dir)
            # A shard with no files is a view of no rows.
            assert _rows([], index_dir) == 0

    def test_a_missing_path_and_a_glob_are_not_found(self):
        with Environment() as env:
            trace = env.create_test_gzip_file()
            index_dir = os.path.join(env.temp_dir, "idx")
            glob_text = os.path.join(os.path.dirname(trace), "*.pfw.gz")
            for spec in (
                os.path.join(env.temp_dir, "nope.pfw.gz"),
                [trace, os.path.join(env.temp_dir, "nope.pfw.gz")],
                glob_text,
                [glob_text],
            ):
                # Indexer stats its files on the first call, TraceViewer reads
                # them when the plan runs.
                with pytest.raises(dftu.DFTUtilsNotFoundError):
                    _indexer(spec, index_dir).status()
                with pytest.raises(dftu.DFTUtilsError):
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
            with pytest.raises(dftu.DFTUtilsIOError):
                viewer.lazy().collect()
            indexer = _indexer(junk, index_dir)
            assert indexer.status().needs_work == [junk]
            with pytest.raises(dftu.DFTUtilsIOError):
                indexer.ensure()


class TestForcedBuildKeepsOtherFolders:
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
