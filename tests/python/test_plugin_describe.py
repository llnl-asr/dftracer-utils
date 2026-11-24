#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""``Plugins.describe()`` and ``dft.scan_file``, the Python side of the plugin
developer tools.

``describe()`` reports what a plugin registered per kind without running it.
``scan_file(path)`` plans a read of a file with the plugin format that claims
it, so a format a plugin adds is readable from Python without the trace index.
"""

from pathlib import Path

import pytest

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402
from dftracer.utils.plugins import PluginInfo, Plugins  # noqa: E402

from .common import find_example_plugin, newest_test_plugin  # noqa: E402

_REPO_ROOT = Path(__file__).resolve().parents[2]
_UDFS_PLUGIN = find_example_plugin(
    "duration_udfs", "DFTRACER_UTILS_DURATION_UDFS_PLUGIN", _REPO_ROOT
)


def test_a_file_no_format_claims_is_a_notfound_error(tmp_path):
    if not hasattr(_ext, "LazyFrame"):
        pytest.skip("extension built without Arrow support")
    plain = tmp_path / "plain.dft"
    plain.write_text("nothing here claims this file\n")
    with pytest.raises(dft.errors.DFTUtilsNotFoundError, match="no format claims"):
        dft.scan_file(str(plain)).collect()


@pytest.mark.skipif(not _UDFS_PLUGIN, reason="no duration_udfs plugin installed")
def test_describe_of_an_example_plugin_lists_its_udfs():
    plugins = Plugins([_UDFS_PLUGIN])
    infos = plugins.describe()

    assert len(infos) == 1
    info = infos[0]
    assert isinstance(info, PluginInfo)
    assert info.path == _UDFS_PLUGIN
    assert info.abi_version == _ext.PLUGIN_ABI_VERSION
    assert "to_ms" in info.udfs
    assert "is_slow" in info.udfs
    # Every list field is a list of str, and every kind has its own list.
    for field in (
        "provides",
        "consumes",
        "ops",
        "udfs",
        "udafs",
        "states",
        "providers",
        "nodes",
        "index_extensions",
        "file_formats",
        "sinks",
        "reads",
        "config_keys",
    ):
        value = getattr(info, field)
        assert isinstance(value, list), field
        assert all(isinstance(item, str) for item in value), field
    # Two scalar UDFs and nothing else.
    assert info.udafs == []
    assert info.states == []
    assert info.file_formats == []


_TOY_FORMAT_PLUGIN = newest_test_plugin("toy_format_plugin", _REPO_ROOT)


@pytest.mark.skipif(not hasattr(_ext, "LazyFrame"), reason="extension built without Arrow support")
@pytest.mark.skipif(not _TOY_FORMAT_PLUGIN, reason="no built toy_format_plugin")
def test_describe_lists_a_file_format_and_scan_file_reads_through_it(tmp_path):
    plugins = Plugins([_TOY_FORMAT_PLUGIN])
    info = plugins.describe()[0]
    assert "toy_format.rows" in info.file_formats

    toy = tmp_path / "rows.toy"
    toy.write_text("TOY1\n1 alpha\n2 beta\n")

    got = dft.scan_file(str(toy)).collect().to_arrow().to_pydict()
    assert got == {"id": [1, 2], "name": ["alpha", "beta"]}
