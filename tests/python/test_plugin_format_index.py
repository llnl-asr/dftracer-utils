#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""An index of a file a plugin file format reads: ``Indexer`` builds it into an
``index_dir`` and ``scan_file(path, index_dir)`` skips the chunks it rules out.

The format is ``tests/plugins/sdk_chunked_format_plugin.cpp``: 4 rows per chunk,
one ``id`` column. It appends each chunk it scans to the file named by
``SDK_CHUNKED_LOG``, which is how the test sees the chunks that were skipped.
"""

from pathlib import Path

import pytest

import dftracer.utils as dft
from dftracer.utils import dftracer_utils_ext as _ext
from dftracer.utils.plugins import Plugins

from .common import newest_test_plugin

_REPO_ROOT = Path(__file__).resolve().parents[2]


_PLUGIN = newest_test_plugin("sdk_chunked_format_plugin", _REPO_ROOT)

pytestmark = [
    pytest.mark.skipif(
        not hasattr(_ext, "LazyFrame"), reason="extension built without Arrow support"
    ),
    pytest.mark.skipif(not _PLUGIN, reason="no built sdk_chunked_format_plugin"),
]


def _scanned(log: Path):
    return [int(line) for line in log.read_text().split()] if log.exists() else []


def test_scan_file_with_an_index_dir_skips_chunks(tmp_path, monkeypatch):
    data = tmp_path / "data"
    data.mkdir()
    rows = data / "rows.sdk"
    rows.write_text("SDKC\n" + "".join(f"{i}\n" for i in range(1, 17)))
    index_dir = tmp_path / "index"
    index_dir.mkdir()
    log = tmp_path / "scanned.log"
    monkeypatch.setenv("SDK_CHUNKED_LOG", str(log))

    plugins = Plugins([_PLUGIN])
    try:
        with dft.index.Indexer([str(rows)], index_dir=str(index_dir)) as indexer:
            indexer.build()
        log.unlink(missing_ok=True)

        got = dft.scan_file(str(rows), str(index_dir)).filter(dft.col("id") > 12).collect()
        assert got.to_arrow().to_pydict() == {"id": [13, 14, 15, 16]}
        assert _scanned(log) == [3]

        log.unlink()
        other = tmp_path / "no-index"
        other.mkdir()
        got = dft.scan_file(str(rows), str(other)).filter(dft.col("id") > 12).collect()
        assert got.to_arrow().to_pydict() == {"id": [13, 14, 15, 16]}
        assert sorted(_scanned(log)) == [0, 1, 2, 3]
    finally:
        del plugins
