#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A LazyFrame sink: a plugin's writer takes every row the plan streams.

The example plugin ``csv`` (examples/plugins/csv_sink.cpp) registers the sink
``csv.write``, whose option ``path`` names the file it writes. The summary the run returns is what the sink
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
        not hasattr(_ext, "LazyFrame"), reason="extension built without Arrow support"
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
    summary = _df().lazy().sink(out, format="csv.write")
    assert isinstance(summary, SinkSummary)
    assert summary == SinkSummary(root=str(out), rows=3, kind="csv.write", part_count=1)
    assert out.read_text() == "id,name\n1,a\n2,b\n3,c\n"
    assert plugin is not None


def test_sink_runs_the_pipeline_and_writes_what_it_keeps(tmp_path, plugin):
    out = tmp_path / "filtered.csv"
    summary = _df().lazy().filter(col("id") > 1).sink(out, format="csv.write")
    assert summary.rows == 2
    assert out.read_text().splitlines() == ["id,name", "2,b", "3,c"]
    assert plugin is not None


def test_a_lazy_plugin_sink_writes_when_it_runs(tmp_path, plugin):
    out = tmp_path / "lazy.csv"
    pending = _df().lazy().sink(out, format="csv.write", lazy=True)
    assert not out.exists()
    (summary,) = dft.collect_all([pending])
    assert summary.rows == 3 and out.exists()
    assert plugin is not None


def test_an_unknown_sink_names_the_registered_ones(tmp_path, plugin):
    with pytest.raises(dft.errors.DFTUtilsNotFoundError) as failed:
        _df().lazy().sink(tmp_path / "x.csv", format="no_such.sink")
    assert "no sink named 'no_such.sink'" in str(failed.value)
    assert "csv.write" in str(failed.value)
    assert plugin is not None


def test_a_bad_option_value_is_a_type_error(tmp_path, plugin):
    with pytest.raises(TypeError) as failed:
        _df().lazy().sink(tmp_path / "x.csv", format="csv.write", options={"delimiter": [1]})
    assert "delimiter" in str(failed.value)
    assert "list" in str(failed.value)
    assert plugin is not None


def test_the_path_is_not_an_option(tmp_path, plugin):
    with pytest.raises(ValueError) as failed:
        _df().lazy().sink(tmp_path / "x.csv", format="csv.write", options={"path": "y.csv"})
    assert "path" in str(failed.value)
    assert plugin is not None


def test_shared_arguments_reach_the_plugin_sink_as_options(tmp_path, plugin, monkeypatch):
    from dftracer.utils import _sink

    seen = {}

    class Recorder:
        @staticmethod
        def sink(plan, format, options, batch_size, runtime):
            seen.update(options)
            return 0, 0

    monkeypatch.setattr(_sink._ext, "LazyFrame", Recorder)
    _df().lazy().sink(
        tmp_path / "x.csv",
        format="csv.write",
        options={"delimiter": ";"},
        mode="overwrite",
        name="n",
        meta={"k": 1},
        rows="records",
        part_size=3,
        max_open_files=8,
    )
    assert seen == {
        "path": str(tmp_path / "x.csv"),
        "delimiter": ";",
        "mode": "overwrite",
        "name": "n",
        "meta": '{"k": 1}',
        "rows": "records",
        "part_size": "3",
        "max_open_files": "8",
    }
    with pytest.raises(ValueError, match="mode"):
        _df().lazy().sink(
            tmp_path / "x.csv", format="csv.write", options={"mode": "error"}, mode="overwrite"
        )
    assert plugin is not None


def test_a_shared_argument_equal_to_its_default_still_reaches_the_plugin(
    tmp_path, plugin, monkeypatch
):
    from dftracer.utils import _sink

    seen = []

    class Recorder:
        @staticmethod
        def sink(plan, format, options, batch_size, runtime):
            seen.append(dict(options))
            return 0, 0

    monkeypatch.setattr(_sink._ext, "LazyFrame", Recorder)
    _df().lazy().sink(
        tmp_path / "x.csv", format="csv.write", mode="error", part_size=0, max_open_files=64
    )
    assert seen[-1] == {
        "path": str(tmp_path / "x.csv"),
        "mode": "error",
        "part_size": "0",
        "max_open_files": "64",
    }
    _df().lazy().sink(tmp_path / "x.csv", format="csv.write")
    assert seen[-1] == {"path": str(tmp_path / "x.csv")}
    _df().write(tmp_path / "y.csv", format="csv.write", mode="error")
    assert seen[-1]["mode"] == "error"
    assert plugin is not None


def test_session_sink_takes_every_shared_argument_of_the_viewer_sink():
    import inspect

    from dftracer.utils.trace import Session, TraceViewer

    viewer = set(inspect.signature(TraceViewer.sink).parameters) - {"lazy"}
    session = set(inspect.signature(Session.sink).parameters) - {"viewer"}
    assert session == viewer
