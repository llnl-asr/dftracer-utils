#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""End-to-end test for the dftracer_plugin CLI.

`new` scaffolds a template plugin, `build` compiles it against the bundled ABI
headers to a loadable .so, and Plugins loads and runs it over a fake trace.
The C template folds each batch into a per-process count accumulator whose
value column sums to the scanned event count.
"""

import ctypes
import gzip
import os
import shutil

import pytest

from dftracer.utils import plugin_cli
from dftracer.utils.plugins import Plugins

_HAS_CXX = bool(shutil.which("c++") or shutil.which("clang++") or shutil.which("g++"))


def _write_trace(path: str, n: int) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i in range(n):
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{1 + i % 2},"tid":1,'
                f'"ts":{1000 + i},"dur":{10 + i},"ph":1,"args":{{}}}}\n'
            )


def test_ops_lists_host_ops_with_signatures(capsys):
    assert plugin_cli.main(["ops"]) == 0
    out = capsys.readouterr().out
    assert "dftu.series.add" in out
    # the signature and kind ride along, which is the point of the command
    for line in out.splitlines():
        if line.startswith("dftu.series.add "):
            assert "[series," in line
            break
    else:
        raise AssertionError("dftu.series.add not listed")


def test_ops_prefix_filters_and_reports_frame_kind(capsys):
    assert plugin_cli.main(["ops", "dftu.frame."]) == 0
    out = capsys.readouterr().out
    lines = [ln for ln in out.splitlines() if ln.strip()]
    assert lines, "expected registered dftu.frame.* ops"
    for ln in lines:
        assert ln.startswith("dftu.frame.")
        # The bracketed kind is the op's RETURN token, so a frame -> series op
        # (mask, is_unique) reports series under a dftu.frame. name.
        assert "[frame," in ln or "[series," in ln
    assert any("[frame," in ln for ln in lines)


def test_ops_unknown_prefix_errors(capsys):
    assert plugin_cli.main(["ops", "nope.no.such."]) == 1
    assert "no ops match" in capsys.readouterr().err


def test_cflags_prints_include_dir(capsys):
    assert plugin_cli.main(["cflags"]) == 0
    out = capsys.readouterr().out.strip()
    assert "-std=c++20" in out
    assert "-shared" in out
    assert "-I" in out
    assert "include" in out


def test_new_scaffolds_c_source(tmp_path):
    assert plugin_cli.main(["new", "mycount", "-o", str(tmp_path)]) == 0
    src = tmp_path / "mycount.c"
    assert src.is_file()
    assert "dftracer_plugin" in src.read_text()


def test_new_scaffolds_cpp_source(tmp_path):
    assert plugin_cli.main(["new", "mycount", "--cpp", "-o", str(tmp_path)]) == 0
    src = tmp_path / "mycount.cpp"
    assert src.is_file()
    assert 'DFTU_PLUGIN("mycount"' in src.read_text()
    assert "p.fold<mycount>()" in src.read_text()


def test_generated_abi_version_header_carries_the_host_version(tmp_path, monkeypatch):
    from dftracer.utils import _build
    from dftracer.utils.dftracer_utils_ext import PLUGIN_ABI_VERSION

    monkeypatch.setenv("DFTRACER_UTILS_JIT_CACHE", str(tmp_path / "cache"))
    overlay = _build._ensure_abi_version_header(str(tmp_path / "no_include"))
    assert overlay is not None
    text = (
        tmp_path
        / "cache"
        / "abi_version_include"
        / "dftracer"
        / "utils"
        / "plugins"
        / "abi_version.h"
    ).read_text()
    assert f"#define DFTRACER_UTILS_PLUGIN_ABI_VERSION 0x{PLUGIN_ABI_VERSION:X}u" in text


def test_build_missing_source_errors(tmp_path):
    assert plugin_cli.main(["build", str(tmp_path / "nope.c")]) == 1


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available")
def test_new_build_load_run_c(tmp_path):
    pa = pytest.importorskip("pyarrow")
    assert plugin_cli.main(["new", "mycount", "-o", str(tmp_path)]) == 0
    src = tmp_path / "mycount.c"
    so = tmp_path / "mycount.so"
    assert plugin_cli.main(["build", str(src), "-o", str(so)]) == 0
    assert so.is_file()

    # It is a real loadable shared object with the ABI entry symbol.
    lib = ctypes.CDLL(str(so), mode=os.RTLD_LAZY)
    assert hasattr(lib, "dftracer_plugin")

    n = 40
    _write_trace(str(tmp_path / "trace.pfw.gz"), n)

    plugins = Plugins([str(so)])
    run = plugins.run(str(tmp_path))

    assert "mycount" in run.results
    tbl = pa.table(run.results["mycount"])
    assert tbl.column_names == ["pid", "value"]
    val = tbl.column("value").to_numpy(zero_copy_only=False)
    assert int(val.sum()) == n
    assert run.stats["events_scanned"] == n


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available")
def test_new_build_load_run_cpp(tmp_path):
    pa = pytest.importorskip("pyarrow")
    assert plugin_cli.main(["new", "mycount", "--cpp", "-o", str(tmp_path)]) == 0
    src = tmp_path / "mycount.cpp"
    so = tmp_path / "mycount_cpp.so"
    assert plugin_cli.main(["build", str(src), "-o", str(so)]) == 0
    assert so.is_file()

    n = 40
    _write_trace(str(tmp_path / "trace.pfw.gz"), n)

    plugins = Plugins([str(so)])
    results = plugins.run(str(tmp_path)).results

    tbl = pa.table(results["mycount"])
    val = tbl.column("value").to_numpy(zero_copy_only=False)
    assert int(val.sum()) == n


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available")
def test_plugin_out_of_memory_raises_memory_error(tmp_path):
    assert plugin_cli.main(["new", "oom", "-o", str(tmp_path)]) == 0
    src = tmp_path / "oom.c"
    text = src.read_text()
    refuse = '(void)slice;\n    return dftu_fail(ctx, DFTU_E_NO_MEMORY, "heap refused");'
    assert "(void)slice;\n    if (!agg" in text
    src.write_text(text.replace("(void)slice;", refuse, 1))
    so = tmp_path / "oom.so"
    assert plugin_cli.main(["build", str(src), "-o", str(so)]) == 0

    _write_trace(str(tmp_path / "trace.pfw.gz"), 10)
    with pytest.raises(MemoryError, match="heap refused"):
        Plugins([str(so)]).run(str(tmp_path))


@pytest.mark.skipif(os.name != "posix", reason="fake compiler is a shell script")
@pytest.mark.parametrize(
    ("banner", "wanted"),
    [
        ("g++ (GCC) 12.1.1 20220628\nCopyright (C) 2022 Free Software Foundation, Inc.", True),
        ("Apple clang version 17.0.0 (clang-1700.0.13.5)", False),
    ],
)
def test_cflags_drop_gnu_unique_only_for_gcc(tmp_path, monkeypatch, banner, wanted):
    from dftracer.utils import _build

    fake = tmp_path / "fake-cxx"
    fake.write_text(f"#!/bin/sh\ncat <<'EOF'\n{banner}\nEOF\n")
    fake.chmod(0o755)
    monkeypatch.setenv("CXX", str(fake))
    _build._is_gcc.cache_clear()
    try:
        assert ("-fno-gnu-unique" in _build.cflags()) is wanted
    finally:
        _build._is_gcc.cache_clear()


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available")
def test_threads_building_one_source_all_get_the_library(tmp_path):
    import threading

    from dftracer.utils import _build

    source = 'extern "C" int answer() { return 42; }\n'
    paths, errors = [], []

    def build():
        try:
            paths.append(_build.compile_source(source, "race", cache=tmp_path, group="g"))
        except Exception as exc:  # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=build) for _ in range(6)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert errors == []
    assert len(set(paths)) == 1
    assert ctypes.CDLL(paths[0]).answer() == 42
