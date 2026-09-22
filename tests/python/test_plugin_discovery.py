#!/usr/bin/env python3
"""Unit tests for find_example_plugin's build-tree selection.

The plugin-host tests (test_plugin_host.py) rely on this to hand back a
plugin ABI-matched to the extension under test rather than the first .so that
happens to dlopen, which a stale build tree can satisfy just as well as a
current one.
"""

import os
import time

from .common import find_example_plugin


def test_find_example_plugin_env_override(tmp_path, monkeypatch):
    fake = tmp_path / "fake_plugin_env.so"
    fake.write_bytes(b"")
    monkeypatch.setenv("DFTRACER_TEST_PLUGIN_ENV_PATH", str(fake))
    result = find_example_plugin("fake_plugin_env", "DFTRACER_TEST_PLUGIN_ENV_PATH", tmp_path)
    assert result == str(fake)


def test_find_example_plugin_picks_newest_build_tree(tmp_path, monkeypatch):
    monkeypatch.delenv("DFTRACER_TEST_PLUGIN_MTIME_PATH", raising=False)

    old_dir = tmp_path / "build" / "build-old" / "examples" / "plugins"
    new_dir = tmp_path / "build" / "build-new" / "examples" / "plugins"
    old_dir.mkdir(parents=True)
    new_dir.mkdir(parents=True)
    old_so = old_dir / "fake_plugin_mtime.so"
    new_so = new_dir / "fake_plugin_mtime.so"
    old_so.write_bytes(b"old")
    new_so.write_bytes(b"new")
    now = time.time()
    os.utime(old_so, (now - 100, now - 100))
    os.utime(new_so, (now, now))

    result = find_example_plugin("fake_plugin_mtime", "DFTRACER_TEST_PLUGIN_MTIME_PATH", tmp_path)
    assert result == str(new_so)


def test_find_example_plugin_empty_when_absent(tmp_path, monkeypatch):
    monkeypatch.delenv("DFTRACER_TEST_PLUGIN_ABSENT_PATH", raising=False)
    result = find_example_plugin("no_such_plugin", "DFTRACER_TEST_PLUGIN_ABSENT_PATH", tmp_path)
    assert result == ""
