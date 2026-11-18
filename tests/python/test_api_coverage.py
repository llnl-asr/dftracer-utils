# SPDX-License-Identifier: MIT
"""The pandas and Polars name-coverage gate (scripts/api_coverage.py): every
public name of the installed library is ours or is listed with a reason in
scripts/api_coverage_exclude.txt, and a listed name that is now ours is a
stale line."""

import importlib.util
import sys
from pathlib import Path

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

_ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location(
    "dftu_api_coverage", _ROOT / "scripts" / "api_coverage.py"
)
assert _spec is not None and _spec.loader is not None
cov = importlib.util.module_from_spec(_spec)
sys.modules["dftu_api_coverage"] = cov
_spec.loader.exec_module(cov)


def _need(lib):
    if lib not in ("pandas", "polars"):
        raise AssertionError(lib)
    pytest.importorskip(lib, reason=f"{lib} is not installed: its coverage is not checked")


@pytest.mark.parametrize("lib", ["pandas", "polars"])
def test_every_public_name_is_ours_or_excluded(lib):
    _need(lib)
    rows, errors, skipped = cov.measure([lib])
    assert not skipped
    assert errors == []
    assert [(r.cls, sorted(r.missing)) for r in rows if r.missing] == []


@pytest.mark.parametrize("lib", ["pandas", "polars"])
def test_every_class_has_upstream_names(lib):
    _need(lib)
    rows, _, _ = cov.measure([lib])
    assert rows
    for r in rows:
        assert r.upstream, f"{lib} {r.cls}: no public names read from the library"
        if (lib, r.cls) != ("polars", "Expr.list"):  # Expr has no list namespace yet
            assert r.have, f"{lib} {r.cls}: none of them is ours"


def _with_exclusions(monkeypatch, tmp_path, text):
    path = tmp_path / "exclude.txt"
    path.write_text(text, encoding="utf-8")
    monkeypatch.setattr(cov, "EXCLUDE", path)


def test_a_name_neither_ours_nor_excluded_fails(monkeypatch, tmp_path):
    _need("polars")
    kept = [
        line
        for line in cov.EXCLUDE.read_text(encoding="utf-8").splitlines()
        if not line.startswith("polars Series.sin |")
    ]
    _with_exclusions(monkeypatch, tmp_path, "\n".join(kept) + "\n")
    rows, errors, _ = cov.measure(["polars"])
    series = next(r for r in rows if r.cls == "Series")
    assert series.missing == {"sin"}


def test_a_line_for_a_name_we_have_is_stale(monkeypatch, tmp_path):
    _need("pandas")
    text = cov.EXCLUDE.read_text(encoding="utf-8") + "pandas Series.sum | out-of-scope | x\n"
    _with_exclusions(monkeypatch, tmp_path, text)
    _, errors, _ = cov.measure(["pandas"])
    assert any("stale exclusion: pandas Series.sum" in e for e in errors)


def test_a_needs_kernel_line_that_points_to_a_method_we_have_is_an_error(monkeypatch, tmp_path):
    _need("polars")
    kept = [
        line
        for line in cov.EXCLUDE.read_text(encoding="utf-8").splitlines()
        if not line.startswith("polars Series.sin |")
    ]
    kept.append("polars Series.sin | needs-kernel | no kernel; use floor for it")
    _with_exclusions(monkeypatch, tmp_path, "\n".join(kept) + "\n")
    _, errors, _ = cov.measure(["polars"])
    assert any("needs-kernel polars Series.sin" in e and "existing floor" in e for e in errors)


@pytest.mark.parametrize(
    "line,message",
    [
        ("pandas Series.sum | nonsense-kind | x", "unknown reason kind"),
        ("pandas Series.sum | out-of-scope", "want 'lib Class.name | kind | detail'"),
        ("pandas Series.sum | out-of-scope | ", "empty detail"),
        ("numpy Series.sum | out-of-scope | x", "unknown library"),
    ],
)
def test_a_bad_exclusion_line_is_an_error(monkeypatch, tmp_path, line, message):
    _with_exclusions(monkeypatch, tmp_path, line + "\n")
    _, errors = cov.load_exclusions(cov.EXCLUDE)
    assert any(message in e for e in errors), errors


def test_a_duplicate_exclusion_is_an_error(monkeypatch, tmp_path):
    line = "pandas Series.nope | out-of-scope | x\n"
    _with_exclusions(monkeypatch, tmp_path, line + line)
    _, errors = cov.load_exclusions(cov.EXCLUDE)
    assert any("duplicate" in e for e in errors)


def test_the_report_has_a_row_per_class():
    _need("pandas")
    rows, _, _ = cov.measure(["pandas"])
    text = cov.report(rows)
    for r in rows:
        assert r.cls in text
    assert "missing" in text
