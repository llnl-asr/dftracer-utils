# SPDX-License-Identifier: MIT
"""Static-typing regression guard for the generic ``Series``.

Runs ty and mypy --strict on the fixtures under tests/python/typing/: the
sample must check clean with every public Series method group asserted, the
constructor sample must check clean under mypy, and the wrong fixture must be
flagged on every deliberate mistake. This locks the element type, the ``Self``
returns, the promotion rules and the dtype-literal ``cast``.
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_TYPING_DIR = Path(__file__).parent / "typing"
_SAMPLE = _TYPING_DIR / "series_typed_sample.py"
_CTOR = _TYPING_DIR / "series_ctor_sample.py"
_WRONG = _TYPING_DIR / "series_typed_wrong.py"
_WRONG_LINES = len(
    [
        line
        for line in _WRONG.read_text(encoding="utf-8").splitlines()
        if "#" in line and not line.startswith(("#", '"""'))
    ]
)

pytestmark = pytest.mark.skipif(
    shutil.which("uvx") is None, reason="uvx not available to run the type checkers"
)


def _ty(path: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["uvx", "ty", "check", str(path)], capture_output=True, text=True, cwd=_ROOT
    )


def _mypy(tmp_path: Path, path: Path) -> subprocess.CompletedProcess:
    # Settings live in [tool.mypy] in pyproject.toml, read from the repo root.
    return subprocess.run(
        ["uvx", "mypy", "--strict", "--cache-dir", str(tmp_path / "mypy_cache"), str(path)],
        capture_output=True,
        text=True,
        cwd=_ROOT,
    )


def test_ty_sample_checks_clean():
    r = _ty(_SAMPLE)
    assert r.returncode == 0, f"expected clean, got:\n{r.stdout}\n{r.stderr}"


def test_mypy_sample_checks_clean(tmp_path):
    r = _mypy(tmp_path, _SAMPLE)
    assert r.returncode == 0, f"expected clean, got:\n{r.stdout}\n{r.stderr}"


def test_mypy_constructor_sample_checks_clean(tmp_path):
    r = _mypy(tmp_path, _CTOR)
    assert r.returncode == 0, f"expected clean, got:\n{r.stdout}\n{r.stderr}"


def test_ty_flags_the_wrong_fixture():
    r = _ty(_WRONG)
    assert r.returncode != 0, "ty accepted code with known errors"
    out = r.stdout + r.stderr
    for rule in (
        "invalid-attribute-access",
        "missing-attribute",
        "invalid-argument-type",
        "unsupported-operator",
        "no-matching-overload",
        "invalid-assignment",
        "type-assertion-failure",
    ):
        assert rule in out, f"expected {rule} diagnostic, got:\n{out}"
    flagged = len(set(re.findall(r"series_typed_wrong\.py:(\d+)", out)))
    assert flagged >= _WRONG_LINES, f"expected a diagnostic per wrong line:\n{out}"


def test_mypy_flags_the_wrong_fixture(tmp_path):
    r = _mypy(tmp_path, _WRONG)
    assert r.returncode != 0, "mypy accepted code with known errors"
    out = r.stdout + r.stderr
    for code in (
        "misc",
        "attr-defined",
        "call-overload",
        "arg-type",
        "operator",
        "assignment",
        "assert-type",
    ):
        assert f"[{code}]" in out, f"expected [{code}] diagnostic, got:\n{out}"
    flagged = len(set(re.findall(r"series_typed_wrong\.py:(\d+): error", out)))
    assert flagged >= _WRONG_LINES, f"expected a diagnostic per wrong line:\n{out}"
