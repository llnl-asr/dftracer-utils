#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Native calls raise the typed DFTUtils*Error classes, not bare ValueError or
RuntimeError."""

import os

import pytest

import dftracer.utils as dft
from dftracer.utils import (
    DFTUtilsDuqlError,
    DFTUtilsError,
    DFTUtilsIndexerError,
    DFTUtilsIOError,
    DFTUtilsNotFoundError,
    DFTUtilsValueError,
)
from dftracer.utils import dftracer_utils_ext as ext


def _frame():
    return dft.DataFrame({"a": [1, 2, 3], "s": ["x", "y", "z"]})


def test_value_error_class_is_also_a_value_error():
    assert issubclass(DFTUtilsValueError, DFTUtilsError)
    assert issubclass(DFTUtilsValueError, ValueError)


def test_explode_of_a_non_list_column_raises_value_error_class():
    with pytest.raises(DFTUtilsValueError, match="not a List column"):
        ext.DataFrame.explode(_frame(), "a")


def test_group_by_unknown_aggregate_raises_value_error_class():
    with pytest.raises(DFTUtilsValueError, match="unknown aggregate op"):
        ext.DataFrame.group_by(_frame(), "a", "bogus:a")


def test_lazy_unknown_column_raises_not_found_class():
    with pytest.raises(DFTUtilsNotFoundError, match="no column named zz"):
        ext.DataFrame.lazy(_frame()).head_by("zz", 1)


def test_not_found_class_is_also_a_key_error():
    assert issubclass(DFTUtilsNotFoundError, DFTUtilsError)
    assert issubclass(DFTUtilsNotFoundError, KeyError)


def test_eager_and_lazy_unknown_column_agree():
    with pytest.raises(DFTUtilsNotFoundError) as eager:
        ext.DataFrame.select(_frame(), "zz")
    with pytest.raises(DFTUtilsNotFoundError) as lazy:
        ext.DataFrame.lazy(_frame()).head_by("zz", 1)
    assert isinstance(eager.value, KeyError) and isinstance(lazy.value, KeyError)
    assert str(eager.value) == "select: no column named zz"
    assert str(lazy.value) == "head_by: no column named zz"


def test_duql_parse_error_raises_duql_class():
    with pytest.raises(DFTUtilsDuqlError):
        ext.DataFrame.query(_frame(), "(((")


def test_missing_gz_path_raises_indexer_class(tmp_path):
    with pytest.raises(DFTUtilsIndexerError, match="does not exist"):
        ext.CheckpointIndexer(str(tmp_path / "missing.pfw.gz"), str(tmp_path / "missing.idx"))


def test_batch_index_of_a_non_gzip_file_raises_base_class(tmp_path):
    bad = tmp_path / "bad.pfw.gz"
    bad.write_bytes(b"not gzip")
    with pytest.raises(DFTUtilsError, match="not gz") as info:
        ext.build_index_batch(
            files=[str(bad)],
            index_dir=str(tmp_path / "idx"),
        )
    assert type(info.value) is DFTUtilsError


def test_missing_trace_raises_not_found_class(tmp_path):
    view = dft.TraceViewer([os.fspath(tmp_path / "missing.pfw.gz")])
    with pytest.raises(DFTUtilsNotFoundError):
        view.agg("count").collect()


def test_unreadable_trace_raises_io_class(tmp_path):
    bad = tmp_path / "bad.pfw.gz"
    bad.write_bytes(b"not gzip")
    with pytest.raises(DFTUtilsIOError):
        dft.TraceViewer([str(bad)]).agg("count").collect()
