# SPDX-License-Identifier: MIT
"""duql ``explain`` and ``explain analyze``: the prefix, ``Pipe.explain(analyze=True)``
and ``TraceViewer.explain_duql`` give the plan, and the analyze form runs it and
adds the rows and time of each stage."""

import gzip
import json

import pytest

import dftracer.utils as dftu
from dftracer.utils.duql import c, source


def _trace(tmp_path):
    events = [
        {"ph": 1, "name": n, "cat": "POSIX", "pid": 1, "tid": 1, "ts": 10 * i, "dur": d}
        for i, (n, d) in enumerate([("read", 5), ("write", 50), ("read", 500), ("read", 900)])
    ]
    path = tmp_path / "t.pfw.gz"
    path.write_bytes(gzip.compress(("\n".join(json.dumps(e) for e in events) + "\n").encode()))
    return str(path)


def test_the_explain_prefix_gives_the_plan_as_rows(tmp_path):
    path = _trace(tmp_path)
    tv = dftu.trace.TraceViewer(path)
    plain = tv.explain_duql(f'from "{path}" | where dur > 100')
    got = tv.duql(f'explain from "{path}" | where dur > 100').collect().to_dict()
    assert list(got) == ["plan"]
    assert "\n".join(got["plan"]) + "\n" == plain


def test_explain_analyze_runs_the_plan_and_measures_each_stage(tmp_path):
    path = _trace(tmp_path)
    tv = dftu.trace.TraceViewer(path)
    text = tv.explain_duql(f'explain analyze from "{path}" | where dur > 100 | select name')
    assert "run:" in text
    assert "analyze:" in text
    assert "scan: rows out " in text
    assert ", time " in text
    # The plain explain of the same query runs nothing.
    assert "analyze:" not in tv.explain_duql(f'from "{path}" | where dur > 100 | select name')


def test_the_builder_asks_for_analyze(tmp_path):
    path = _trace(tmp_path)
    q = source(path).where(c("dur") > 100)
    assert "analyze:" not in q.explain()
    assert "analyze:" in q.explain(analyze=True)


def test_viewer_explain_analyze(tmp_path):
    path = _trace(tmp_path)
    text = dftu.trace.TraceViewer(path).filter(dftu.col("dur") > 100).explain(analyze=True)
    assert "analyze:" in text
    assert "rows out " in text


def test_a_field_named_explain_needs_backticks(tmp_path):
    path = _trace(tmp_path)
    with pytest.raises(Exception):
        dftu.trace.TraceViewer(path).duql("explain == 1").collect()
