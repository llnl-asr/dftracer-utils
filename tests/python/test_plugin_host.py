#!/usr/bin/env python3
"""End-to-end tests for the compiled-plugin Plugins binding.

These load a real compiled example plugin (event_counter.so, C ABI) and run it
over a generated trace through the Python binding, asserting the fused scan
counted every event.
"""

import os
from pathlib import Path

import pytest

from dftracer.utils.plugins import Plugins

from .common import Environment, find_example_plugin

_REPO_ROOT = Path(__file__).resolve().parents[2]


def _find_plugin(name: str, env_var: str) -> str:
    return find_example_plugin(name, env_var, _REPO_ROOT)


_PLUGIN = _find_plugin("event_counter", "DFTRACER_EVENT_COUNTER_PLUGIN_PATH")
_RESULT_PLUGIN = _find_plugin("process_counts", "DFTRACER_PROCESS_COUNTS_PLUGIN_PATH")
_skip = pytest.mark.skipif(
    not _PLUGIN, reason="no compiled event_counter plugin found in any build tree"
)


@_skip
class TestPlugins:
    def test_load_run_counts_events(self):
        with Environment() as env:
            env.create_dft_trace_file("trace.pfw.gz", num_events=50)
            plugins = Plugins([_PLUGIN])
            run = plugins.run(env.temp_dir)
            assert run.stats["events_scanned"] == 50
            assert run.stats["events_matched"] == 50

    def test_run_single_file(self):
        with Environment() as env:
            gz = env.create_dft_trace_file("one.pfw.gz", num_events=17)
            plugins = Plugins([_PLUGIN])
            run = plugins.run(gz)
            assert run.stats["events_scanned"] == 17

    def test_load_with_config_dict(self):
        # event_counter ignores config; this exercises the dict -> JSON ->
        # ConfigTree path end to end without error.
        with Environment() as env:
            env.create_dft_trace_file("cfg.pfw.gz", num_events=8)
            plugins = Plugins([_PLUGIN], config={"event_counter": {"threshold": 5, "label": "x"}})
            run = plugins.run(env.temp_dir)
            assert run.stats["events_scanned"] == 8

    def test_run_missing_traces_raises(self):
        with Environment() as env:
            empty = os.path.join(env.temp_dir, "empty")
            os.makedirs(empty, exist_ok=True)
            plugins = Plugins([_PLUGIN])
            with pytest.raises(Exception):
                plugins.run(empty)

    def test_bad_path_raises_on_construction(self):
        # The set is built (dlopen included) at construction time.
        with pytest.raises(ImportError):
            Plugins(["/nonexistent/does_not_exist.so"])


@pytest.mark.skipif(
    not _RESULT_PLUGIN,
    reason="no compiled process_counts plugin found in any build tree",
)
class TestPluginResults:
    def test_emitted_blob_round_trips_to_python(self):
        with Environment() as env:
            env.create_dft_trace_file("trace.pfw.gz", num_events=40)
            plugins = Plugins([_RESULT_PLUGIN])
            run = plugins.run(env.temp_dir)

            assert "process_counts" in run.results
            blob = run.results["process_counts"]
            assert isinstance(blob, bytes)

            # Each TSV row is "<pid>\t<count>"; the counts sum to every event.
            total = 0
            for line in blob.decode().splitlines():
                pid, count = line.split("\t")
                total += int(count)
            assert total == 40
            assert run.stats["events_scanned"] == 40
