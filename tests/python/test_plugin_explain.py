#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""``LazyFrame.explain`` shows what a plugin file format says about its plan, and
``explain(analyze=True)`` runs the plan and adds the rows and time of each stage,
the filters the format took and the counters it added.

The format is ``tests/plugins/sdk_chunked_format_plugin.cpp``: 4 rows per chunk,
one ``id`` column, an explain hook and a ``chunks`` counter.
"""

from pathlib import Path

import pytest

import dftracer.utils as dft
from dftracer.utils import dftracer_utils_ext as _ext
from dftracer.utils.plugins import Plugins

_REPO_ROOT = Path(__file__).resolve().parents[2]


def _plugin() -> str:
    found = []
    for suffix in ("dylib", "so"):
        found += _REPO_ROOT.glob(f"build/**/tests/plugins/sdk_chunked_format_plugin.{suffix}")
    return str(max(found, key=lambda p: p.stat().st_mtime)) if found else ""


_PLUGIN = _plugin()

pytestmark = [
    pytest.mark.skipif(
        not hasattr(_ext, "LazyFrame"), reason="extension built without Arrow support"
    ),
    pytest.mark.skipif(not _PLUGIN, reason="no built sdk_chunked_format_plugin"),
]


def _rows(tmp_path: Path) -> Path:
    data = tmp_path / "data"
    data.mkdir()
    rows = data / "rows.sdk"
    rows.write_text("SDKC\n" + "".join(f"{i}\n" for i in range(1, 17)))
    return rows


def test_explain_shows_the_format_lines(tmp_path):
    rows = _rows(tmp_path)
    index_dir = tmp_path / "index"
    index_dir.mkdir()
    plugins = Plugins([_PLUGIN])
    try:
        with dft.Indexer([str(rows)], index_dir=str(index_dir)) as indexer:
            indexer.build()
        plan = dft.scan_file(str(rows), str(index_dir)).filter(dft.col("id") > 12).explain()
        assert "file format 'sdk_chunked': chunks=1 of 4 read; rows_per_chunk=4" in plan
    finally:
        del plugins


def test_explain_analyze_counts_what_the_run_did(tmp_path):
    rows = _rows(tmp_path)
    index_dir = tmp_path / "index"
    index_dir.mkdir()
    plugins = Plugins([_PLUGIN])
    try:
        with dft.Indexer([str(rows)], index_dir=str(index_dir)) as indexer:
            indexer.build()
        lf = dft.scan_file(str(rows), str(index_dir)).filter(dft.col("id") > 12)
        text = lf.explain(analyze=True)
        assert "pushed filter 0: no" in text
        assert "scan: rows out 4, time (incl. inputs) " in text
        assert "filter: rows in 4, rows out 4, time (incl. inputs) " in text
        assert "counter sdk_chunked.chunks: 1" in text
        # A plain explain runs nothing and has no run lines.
        assert "analyze:" not in lf.explain()
    finally:
        del plugins
