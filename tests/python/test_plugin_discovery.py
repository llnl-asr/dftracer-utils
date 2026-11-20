#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unit tests for find_example_plugin's build-tree selection.

The plugin-host tests (test_plugin_host.py) rely on this to hand back a
plugin ABI-matched to the extension under test rather than the first .so that
happens to dlopen, which a stale build tree can satisfy just as well as a
current one.
"""

import os
import time

from .common import _NATIVE_MAGIC, find_example_plugin, newest_test_plugin

_FOREIGN_MAGIC = b"\xcf\xfa\xed\xfe" if _NATIVE_MAGIC == (b"\x7fELF",) else b"\x7fELF"


def test_find_example_plugin_env_override(tmp_path, monkeypatch):
    fake = tmp_path / "fake_plugin_env.so"
    fake.write_bytes(b"")
    monkeypatch.setenv("DFTRACER_UTILS_TEST_PLUGIN_ENV_PATH", str(fake))
    result = find_example_plugin("fake_plugin_env", "DFTRACER_UTILS_TEST_PLUGIN_ENV_PATH", tmp_path)
    assert result == str(fake)


def test_find_example_plugin_picks_newest_build_tree(tmp_path, monkeypatch):
    monkeypatch.delenv("DFTRACER_UTILS_TEST_PLUGIN_MTIME_PATH", raising=False)

    old_dir = tmp_path / "build" / "build-old" / "examples" / "plugins"
    new_dir = tmp_path / "build" / "build-new" / "examples" / "plugins"
    old_dir.mkdir(parents=True)
    new_dir.mkdir(parents=True)
    old_so = old_dir / "fake_plugin_mtime.so"
    new_so = new_dir / "fake_plugin_mtime.so"
    old_so.write_bytes(_NATIVE_MAGIC[0] + b"old")
    new_so.write_bytes(_NATIVE_MAGIC[0] + b"new")
    now = time.time()
    os.utime(old_so, (now - 100, now - 100))
    os.utime(new_so, (now, now))

    result = find_example_plugin(
        "fake_plugin_mtime", "DFTRACER_UTILS_TEST_PLUGIN_MTIME_PATH", tmp_path
    )
    assert result == str(new_so)


def test_plugin_finders_skip_a_library_of_another_platform(tmp_path, monkeypatch):
    monkeypatch.delenv("DFTRACER_UTILS_TEST_PLUGIN_FOREIGN_PATH", raising=False)
    native = tmp_path / "build" / "build-tests" / "examples" / "plugins" / "fake_plugin_foreign.so"
    foreign = (
        tmp_path / "build" / "build-container" / "examples" / "plugins" / "fake_plugin_foreign.so"
    )
    for path in (native, foreign):
        path.parent.mkdir(parents=True)
    native.write_bytes(_NATIVE_MAGIC[0] + b"here")
    foreign.write_bytes(_FOREIGN_MAGIC + b"there")
    now = time.time()
    os.utime(native, (now - 100, now - 100))
    os.utime(foreign, (now, now))
    assert find_example_plugin(
        "fake_plugin_foreign", "DFTRACER_UTILS_TEST_PLUGIN_FOREIGN_PATH", tmp_path
    ) == str(native)

    test_native = tmp_path / "build" / "build-tests" / "tests" / "plugins" / "fake_test_plugin.so"
    test_foreign = (
        tmp_path / "build" / "build-container" / "tests" / "plugins" / "fake_test_plugin.so"
    )
    for path in (test_native, test_foreign):
        path.parent.mkdir(parents=True)
    test_native.write_bytes(_NATIVE_MAGIC[0] + b"here")
    test_foreign.write_bytes(_FOREIGN_MAGIC + b"there")
    os.utime(test_native, (now - 100, now - 100))
    os.utime(test_foreign, (now, now))
    assert newest_test_plugin("fake_test_plugin", tmp_path) == str(test_native)
    test_native.unlink()
    assert newest_test_plugin("fake_test_plugin", tmp_path) == ""


def test_find_example_plugin_empty_when_absent(tmp_path, monkeypatch):
    monkeypatch.delenv("DFTRACER_UTILS_TEST_PLUGIN_ABSENT_PATH", raising=False)
    result = find_example_plugin(
        "no_such_plugin", "DFTRACER_UTILS_TEST_PLUGIN_ABSENT_PATH", tmp_path
    )
    assert result == ""
