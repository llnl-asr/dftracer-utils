#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A LazyFrame sink: a plugin's writer takes every row the plan streams.

The example plugin ``csv_sink`` registers the sink ``csv.write``, whose option
``path`` names the file it writes. The summary the run returns is what the sink
took, not what a frame held, so the test reads the file back.
"""

import pytest

pa = pytest.importorskip("pyarrow")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import DataFrame, SinkSummary, col  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402
from dftracer.utils.plugins import Plugins  # noqa: E402

from .common import find_installed_example_plugin  # noqa: E402

_PLUGIN = find_installed_example_plugin("csv")

pytestmark = [
    pytest.mark.skipif(
        not hasattr(_ext, "_LazyFrame"), reason="extension built without Arrow support"
    ),
    pytest.mark.skipif(not _PLUGIN, reason="no csv plugin installed next to the extension"),
]


@pytest.fixture(scope="module")
def plugin():
    # One load for the module: the library registers its sinks on load, and a
    # second load would register them twice.
    return Plugins([_PLUGIN])


def _df():
    return DataFrame.from_arrow(pa.table({"id": [1, 2, 3], "name": ["a", "b", "c"]}))


def test_sink_writes_every_row_and_reports_the_totals(tmp_path, plugin):
    out = tmp_path / "out.csv"
    summary = _df().lazy().sink("csv.write", path=str(out))
    assert isinstance(summary, SinkSummary)
    assert summary == SinkSummary(rows=3, batches=1)
    assert (summary.rows, summary.batches) == (3, 1)
    assert out.read_text() == "id,name\n1,a\n2,b\n3,c\n"
    assert plugin is not None


def test_sink_runs_the_pipeline_and_writes_what_it_keeps(tmp_path, plugin):
    out = tmp_path / "filtered.csv"
    summary = _df().lazy().filter(col("id") > 1).sink("csv.write", path=str(out))
    assert summary.rows == 2
    assert out.read_text().splitlines() == ["id,name", "2,b", "3,c"]
    assert plugin is not None


def test_an_unknown_sink_names_the_registered_ones(tmp_path, plugin):
    with pytest.raises(dft.DFTUtilsNotFoundError) as failed:
        _df().lazy().sink("no_such.sink", path=str(tmp_path / "x.csv"))
    assert "no sink named 'no_such.sink'" in str(failed.value)
    assert "csv.write" in str(failed.value)
    assert plugin is not None


def test_a_non_str_option_value_is_a_type_error(tmp_path, plugin):
    with pytest.raises(TypeError) as failed:
        _df().lazy().sink("csv.write", path=1)
    assert "path" in str(failed.value)
    assert "int" in str(failed.value)
    assert plugin is not None


def test_a_sink_without_its_required_option_fails(tmp_path, plugin):
    with pytest.raises(dft.DFTUtilsError) as failed:
        _df().lazy().sink("csv.write")
    assert "path" in str(failed.value)
    assert plugin is not None
