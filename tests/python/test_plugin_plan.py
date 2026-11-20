#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""``LazyFrame.explain`` shows the plan a plugin file format gives before it scans.

The format is ``tests/plugins/sdk_plan_plugin.cpp``: a file that starts with the
line ``SDKP`` and then one integer per line, in the column ``id``. Its plan hook
reads the filters as one duql text, and its explain hook prints that text.
"""

from pathlib import Path

import pytest

import dftracer.utils as dft
from dftracer.utils import dftracer_utils_ext as _ext
from dftracer.utils.plugins import Plugins

from .common import newest_test_plugin

_REPO_ROOT = Path(__file__).resolve().parents[2]


_PLUGIN = newest_test_plugin("sdk_plan_plugin", _REPO_ROOT)

pytestmark = [
    pytest.mark.skipif(
        not hasattr(_ext, "LazyFrame"), reason="extension built without Arrow support"
    ),
    pytest.mark.skipif(not _PLUGIN, reason="no built sdk_plan_plugin"),
]


def _rows(tmp_path: Path) -> Path:
    data = tmp_path / "data"
    data.mkdir()
    rows = data / "rows.sdkp"
    rows.write_text("SDKP\n" + "".join(f"{i}\n" for i in range(1, 6)))
    return rows


def test_explain_shows_the_plan_and_the_duql_the_plugin_got(tmp_path):
    rows = _rows(tmp_path)
    index_dir = tmp_path / "index"
    index_dir.mkdir()
    plugins = Plugins([_PLUGIN])
    try:
        plan = dft.scan_file(str(rows), str(index_dir)).filter(dft.col("id") > 2).explain()
        assert "file format 'sdk_plan.fmt'" in plan
        assert "duql=id > 2; plan_votes=no; plan_cost=unknown; plan_est_rows=1001" in plan
    finally:
        del plugins


def test_explain_analyze_hands_the_token_to_the_scan(tmp_path):
    rows = _rows(tmp_path)
    index_dir = tmp_path / "index"
    index_dir.mkdir()
    plugins = Plugins([_PLUGIN])
    try:
        lf = dft.scan_file(str(rows), str(index_dir)).filter(dft.col("id") > 2)
        assert "counter sdk_plan.fmt.token: 7" in lf.explain(analyze=True)
    finally:
        del plugins
