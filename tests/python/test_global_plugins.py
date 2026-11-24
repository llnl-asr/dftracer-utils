#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Plugins loaded for the process, and one reader probe.

``DFTRACER_UTILS_PLUGINS`` loads plugins once, at the first call that reads a
trace. ``dft.plugins.load_plugin(path)`` loads one for a ``with`` block. The built-in
trace reader scores in the same probe as a plugin file format, the highest score
wins and a tie is refused. ``format=`` names the reader.

The formats are ``tests/plugins/toy_format_plugin`` (``toy_format.rows``, claims
a ``TOY1`` file at 90), ``toy_format_tie_plugin`` (``toy_format_tie.rows``, the
same score on the same file) and ``chunked_format_plugin``
(``chunked_format.rows``, claims a ``CHK1`` file at 90).
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

import dftracer.utils as dft
from dftracer.utils import dftracer_utils_ext as _ext

from .common import Environment, newest_test_plugin

_REPO_ROOT = Path(__file__).resolve().parents[2]


_TOY = newest_test_plugin("toy_format_plugin", _REPO_ROOT)
_TIE = newest_test_plugin("toy_format_tie_plugin", _REPO_ROOT)
_CHUNKED = newest_test_plugin("chunked_format_plugin", _REPO_ROOT)

pytestmark = [
    pytest.mark.skipif(
        not hasattr(_ext, "LazyFrame"), reason="extension built without Arrow support"
    ),
    pytest.mark.skipif(
        not (_TOY and _TIE and _CHUNKED), reason="the test format plugins are not built"
    ),
]

_TOY_ROWS = "TOY1\n1 alpha\n2 beta\n3 gamma\n"


def _run(code: str, plugins: str | None) -> subprocess.CompletedProcess[str]:
    """``code`` in a fresh interpreter, with ``DFTRACER_UTILS_PLUGINS`` set to
    ``plugins`` (unset for None)."""
    env = dict(os.environ)
    env.pop("DFTRACER_UTILS_PLUGINS", None)
    if plugins is not None:
        env["DFTRACER_UTILS_PLUGINS"] = plugins
    return subprocess.run(
        [sys.executable, "-I", "-c", code],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )


_SCAN = """
import dftracer.utils as dft
print(dft.scan_file({path!r}).collect().to_arrow().to_pydict())
"""


def test_the_environment_loads_a_plugin_for_every_reader(tmp_path):
    rows = tmp_path / "rows.toy"
    rows.write_text(_TOY_ROWS)
    code = _SCAN.format(path=str(rows))

    without = _run(code, None)
    assert without.returncode != 0
    assert "no format claims" in without.stderr

    loaded = _run(code, _TOY)
    assert loaded.returncode == 0, loaded.stderr
    assert "'id': [1, 2, 3]" in loaded.stdout


def test_a_plugin_path_that_does_not_load_is_an_error_that_names_it(tmp_path):
    missing = str(tmp_path / "no_such_plugin.so")
    code = _SCAN.format(path=str(tmp_path / "x.toy"))
    result = _run(code, missing)
    assert result.returncode != 0
    assert missing in result.stderr


def test_a_path_named_twice_and_empty_items_load_once(tmp_path):
    rows = tmp_path / "rows.toy"
    rows.write_text(_TOY_ROWS)
    result = _run(_SCAN.format(path=str(rows)), f"{_TOY}::{_TOY}:")
    assert result.returncode == 0, result.stderr


def test_the_environment_plugin_reads_through_the_indexer_and_the_viewer_refuses(tmp_path):
    rows = tmp_path / "rows.pfw"
    rows.write_text("CHK1\n" + "".join(f"{i} n{i}\n" for i in range(1, 9)))
    index = tmp_path / "index"
    index.mkdir()
    code = f"""
import dftracer.utils as dft
with dft.index.Indexer([{str(rows)!r}], index_dir={str(index)!r}) as indexer:
    indexer.build()
try:
    dft.trace.TraceViewer([{str(rows)!r}], index_dir={str(index)!r}).collect()
except Exception as exc:
    print(type(exc).__name__, exc)
"""
    result = _run(code, _CHUNKED)
    assert result.returncode == 0, result.stderr
    assert "chunked_format.rows" in result.stdout
    assert "scan_file" in result.stdout


def test_a_tie_is_refused_with_both_names(tmp_path):
    rows = tmp_path / "rows.toy"
    rows.write_text(_TOY_ROWS)
    with dft.plugins.load_plugin(_TOY), dft.plugins.load_plugin(_TIE):
        with pytest.raises(dft.errors.DFTUtilsValueError) as caught:
            dft.scan_file(str(rows)).collect()
        message = str(caught.value)
        assert "toy_format.rows" in message
        assert "toy_format_tie.rows" in message
        assert str(rows) in message


def test_format_forces_a_reader(tmp_path):
    rows = tmp_path / "rows.toy"
    rows.write_text(_TOY_ROWS)
    with dft.plugins.load_plugin(_TOY), dft.plugins.load_plugin(_TIE):
        got = dft.scan_file(str(rows), format="toy_format.rows").collect()
        assert got.to_arrow().to_pydict()["id"] == [1, 2, 3]


def test_an_unknown_format_lists_the_known_names(tmp_path):
    rows = tmp_path / "rows.toy"
    rows.write_text(_TOY_ROWS)
    with dft.plugins.load_plugin(_TOY):
        with pytest.raises(dft.errors.DFTUtilsValueError) as caught:
            dft.scan_file(str(rows), format="nope").collect()
        assert "unknown format 'nope'" in str(caught.value)
        assert "jsonl, toy_format.low, toy_format.rows" in str(caught.value)
        with pytest.raises(dft.errors.DFTUtilsValueError, match="unknown format 'nope'"):
            dft.index.Indexer([str(rows)], format="nope").status()
        with pytest.raises(dft.errors.DFTUtilsValueError, match="unknown format 'nope'"):
            dft.trace.TraceViewer([str(rows)], format="nope").lazy().collect()


def test_a_trace_file_is_read_by_the_built_in_reader_with_plugins_loaded():
    with Environment() as env:
        trace = env.create_dft_trace_file(num_events=64)
        index = Path(env.temp_dir) / "index"
        index.mkdir()
        with dft.index.Indexer([trace], index_dir=str(index)) as indexer:
            indexer.build()
        before = {p: p.stat().st_mtime_ns for p in index.rglob("*") if p.is_file()}
        assert before

        with dft.plugins.load_plugin(_TOY), dft.plugins.load_plugin(_CHUNKED):
            viewer = dft.trace.TraceViewer([trace], index_dir=str(index))
            assert viewer.lazy().collect().num_rows == 64
            assert dft.scan_file(trace, str(index)).collect().num_rows == 64
            assert (
                dft.trace.TraceViewer([trace], index_dir=str(index), format="jsonl")
                .lazy()
                .collect()
                .num_rows
                == 64
            )
            with dft.index.Indexer([trace], index_dir=str(index)) as indexer:
                assert indexer.status().needs_work == []
                indexer.build()

        after = {p: p.stat().st_mtime_ns for p in index.rglob("*") if p.is_file()}
        assert after == before


def test_a_loaded_plugin_is_unloaded_at_the_end_of_the_block(tmp_path):
    rows = tmp_path / "rows.toy"
    rows.write_text(_TOY_ROWS)
    with dft.plugins.load_plugin(_TOY) as handle:
        assert handle.path == _TOY
        assert dft.scan_file(str(rows)).collect().num_rows == 3
    assert handle.closed
    with pytest.raises(dft.errors.DFTUtilsNotFoundError, match="no format claims"):
        dft.scan_file(str(rows)).collect()


def test_loading_the_same_path_twice_keeps_it_until_the_last_close(tmp_path):
    rows = tmp_path / "rows.toy"
    rows.write_text(_TOY_ROWS)
    first = dft.plugins.load_plugin(_TOY)
    second = dft.plugins.load_plugin(_TOY)
    first.close()
    assert dft.scan_file(str(rows)).collect().num_rows == 3
    second.close()
    second.close()
    with pytest.raises(dft.errors.DFTUtilsNotFoundError):
        dft.scan_file(str(rows)).collect()


def test_unloading_while_a_scan_is_open_is_an_error_not_a_crash(tmp_path):
    rows = tmp_path / "rows.toy"
    rows.write_text(_TOY_ROWS)
    handle = dft.plugins.load_plugin(_TOY)
    plan = dft.scan_file(str(rows))
    with pytest.raises(dft.errors.DFTUtilsValueError, match="still open"):
        handle.close()
    assert not handle.closed
    assert plan.collect().num_rows == 3
    del plan
    handle.close()
    assert handle.closed


def test_a_plugin_that_does_not_load_raises_and_names_the_path(tmp_path):
    missing = str(tmp_path / "no_such_plugin.so")
    with pytest.raises(dft.errors.DFTUtilsError, match="no_such_plugin"):
        dft.plugins.load_plugin(missing)


def test_a_plugins_set_shares_a_plugin_that_is_already_loaded(tmp_path):
    from dftracer.utils.plugins import Plugins

    rows = tmp_path / "rows.toy"
    rows.write_text(_TOY_ROWS)
    with dft.plugins.load_plugin(_TOY):
        built = Plugins([_TOY])
        assert built.describe()[0].file_formats
        del built
        assert dft.scan_file(str(rows)).collect().num_rows == 3


def test_a_plugins_set_shares_a_plugin_the_environment_holds(tmp_path):
    rows = tmp_path / "rows.toy"
    rows.write_text(_TOY_ROWS)
    code = f"""
import dftracer.utils as dft
from dftracer.utils.plugins import Plugins
print(dft.scan_file({str(rows)!r}).collect().num_rows)
built = Plugins([{_TOY!r}])
del built
print(dft.scan_file({str(rows)!r}).collect().num_rows)
"""
    result = _run(code, _TOY)
    assert result.returncode == 0, result.stderr
    assert result.stdout.split() == ["3", "3"]


def test_a_failed_environment_load_is_retried_by_the_next_call(tmp_path):
    rows = tmp_path / "rows.toy"
    rows.write_text(_TOY_ROWS)
    late = tmp_path / ("late" + Path(_TOY).suffix)
    code = f"""
import shutil
import dftracer.utils as dft
try:
    dft.scan_file({str(rows)!r})
except Exception as exc:
    print("failed", {str(late)!r} in str(exc))
shutil.copy({_TOY!r}, {str(late)!r})
print(dft.scan_file({str(rows)!r}).collect().num_rows)
"""
    result = _run(code, str(late))
    assert result.returncode == 0, result.stderr
    assert result.stdout.split() == ["failed", "True", "3"]
