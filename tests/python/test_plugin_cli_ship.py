#!/usr/bin/env python3
"""End-to-end tests for the `dftracer_plugin build module:Class` ship path.

`build` routes a `module:Class` argument to the jit backend instead of
compiling a source file, and emits a C header of name constants for every
name the plugin provides alongside the .so.
"""

import ctypes
import gzip
import os
import shutil
import sys
import textwrap

import pytest

from dftracer.utils import plugin_cli

_HAS_CXX = bool(shutil.which("c++") or shutil.which("clang++") or shutil.which("g++"))


def _write_trace(path: str, n: int) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i in range(n):
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{1 + i % 2},"tid":1,'
                f'"ts":{1000 + i},"dur":{10 + i},"ph":"X",'
                f'"args":{{"fhash":"f{i % 3}"}}}}\n'
            )


def _write_wide_plugin(tmp_path) -> None:
    pkg = tmp_path / "acme" / "stats"
    pkg.mkdir(parents=True)
    (tmp_path / "acme" / "__init__.py").write_text("")
    (pkg / "__init__.py").write_text("")
    (pkg / "io.py").write_text(
        textwrap.dedent(
            """
            from dftracer.utils import jit


            @jit.plugin
            class Wide:
                edges = jit.map(key=(jit.i64, jit.str_), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if e.fhash != jit.NONE:
                        self.edges[(e.pid, e.name)] += 1
            """
        )
    )


def _write_not_a_plugin(tmp_path) -> None:
    (tmp_path / "notaplugin.py").write_text("class Foo:\n    pass\n")


def _write_main_authored_plugin(tmp_path) -> None:
    (tmp_path / "mainauthored.py").write_text(
        textwrap.dedent(
            """
            from dftracer.utils import jit


            class Local:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    self.hits[(e.pid,)] += 1

            Local.__module__ = "__main__"
            Local = jit.plugin(Local)
            """
        )
    )


def _forget(*names: str) -> None:
    for name in names:
        sys.modules.pop(name, None)


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available")
def test_build_python_plugin_spec_produces_loadable_so_and_header(tmp_path, monkeypatch):
    from dftracer.utils.plugins import Plugins

    _write_wide_plugin(tmp_path)
    monkeypatch.syspath_prepend(str(tmp_path))
    try:
        so = tmp_path / "wide.so"
        rc = plugin_cli.main(["build", "acme.stats.io:Wide", "-o", str(so)])
        assert rc == 0
        assert so.is_file()

        # Lazy binding: the .so's dftu_* calls resolve inside the host that
        # loads it; here only its exported factory symbol is the question.
        lib = ctypes.CDLL(str(so), mode=os.RTLD_LAZY)
        assert hasattr(lib, "dftracer_plugin")

        header = so.with_suffix(".h")
        assert header.is_file()
        text = header.read_text()
        assert "#ifndef ACME_STATS_IO_WIDE_H" in text
        assert "#define ACME_STATS_IO_WIDE_H" in text
        assert '#define ACME_STATS_IO_WIDE_EDGES "acme/stats.io.wide.edges"' in text
        assert "#endif" in text

        n = 30
        _write_trace(str(tmp_path / "trace.pfw.gz"), n)
        plugins = Plugins([str(so)])
        run = plugins.run(str(tmp_path / "trace.pfw.gz"))
        assert "acme/stats.io.wide.edges" in run.results
        assert run.stats["events_scanned"] == n
    finally:
        _forget("acme", "acme.stats", "acme.stats.io")


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available")
def test_build_python_plugin_no_header(tmp_path, monkeypatch):
    _write_wide_plugin(tmp_path)
    monkeypatch.syspath_prepend(str(tmp_path))
    try:
        so = tmp_path / "wide.so"
        rc = plugin_cli.main(["build", "acme.stats.io:Wide", "-o", str(so), "--no-header"])
        assert rc == 0
        assert so.is_file()
        assert not so.with_suffix(".h").is_file()
    finally:
        _forget("acme", "acme.stats", "acme.stats.io")


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available")
def test_build_python_plugin_explicit_header_path(tmp_path, monkeypatch):
    _write_wide_plugin(tmp_path)
    monkeypatch.syspath_prepend(str(tmp_path))
    try:
        so = tmp_path / "wide.so"
        header = tmp_path / "custom" / "names.h"
        rc = plugin_cli.main(
            ["build", "acme.stats.io:Wide", "-o", str(so), "--header", str(header)]
        )
        assert rc == 0
        assert header.is_file()
        assert not so.with_suffix(".h").is_file()
    finally:
        _forget("acme", "acme.stats", "acme.stats.io")


def test_build_spec_not_a_plugin_class_errors(tmp_path, monkeypatch, capsys):
    _write_not_a_plugin(tmp_path)
    monkeypatch.syspath_prepend(str(tmp_path))
    try:
        rc = plugin_cli.main(["build", "notaplugin:Foo", "-o", str(tmp_path / "out.so")])
        assert rc == 1
        err = capsys.readouterr().err
        assert "not a @jit.plugin class" in err
    finally:
        _forget("notaplugin")


def test_build_spec_missing_module_errors(capsys):
    rc = plugin_cli.main(["build", "no.such.module:Foo", "-o", "out.so"])
    assert rc == 1
    err = capsys.readouterr().err
    assert "cannot import module" in err


def test_build_spec_missing_attribute_errors(tmp_path, monkeypatch, capsys):
    _write_not_a_plugin(tmp_path)
    monkeypatch.syspath_prepend(str(tmp_path))
    try:
        rc = plugin_cli.main(["build", "notaplugin:Missing", "-o", str(tmp_path / "out.so")])
        assert rc == 1
        err = capsys.readouterr().err
        assert "no attribute 'Missing'" in err
    finally:
        _forget("notaplugin")


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available")
def test_build_main_authored_plugin_reports_not_shippable(tmp_path, monkeypatch, capsys):
    _write_main_authored_plugin(tmp_path)
    monkeypatch.syspath_prepend(str(tmp_path))
    try:
        rc = plugin_cli.main(["build", "mainauthored:Local", "-o", str(tmp_path / "out.so")])
        assert rc == 1
        err = capsys.readouterr().err
        assert "cannot build a shippable" in err
        assert "__main__" in err
        assert "Traceback" not in err
    finally:
        _forget("mainauthored")


def test_source_file_build_is_unchanged_regression(tmp_path):
    assert plugin_cli.main(["new", "mycount", "-o", str(tmp_path)]) == 0
    src = tmp_path / "mycount.c"
    assert "dftracer_plugin" in src.read_text()
