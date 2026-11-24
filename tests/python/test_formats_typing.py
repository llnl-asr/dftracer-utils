# SPDX-License-Identifier: MIT
"""Static-typing guard for dftracer.utils.formats.

Runs mypy --strict (the newest, and 1.14 on Python 3.8, the last that supports
it) and ty on the fixtures under tests/python/typing/: the sample must check
clean, and the wrong fixture must be flagged on every deliberate mistake.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_TYPING_DIR = Path(__file__).parent / "typing"
_SAMPLE = _TYPING_DIR / "formats_typed_sample.py"
_WRONG = _TYPING_DIR / "formats_typed_wrong.py"
_WRONG_LINES = len(
    [
        line
        for line in _WRONG.read_text(encoding="utf-8").splitlines()
        if "  # " in line and not line.startswith(("#", '"""'))
    ]
)

pytestmark = pytest.mark.skipif(
    shutil.which("uvx") is None, reason="uvx not available to run the type checkers"
)


def _mypy(tmp_path: Path, path: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            "uvx",
            *(extra or ("--python", sys.executable)),
            "mypy",
            "--strict",
            "--cache-dir",
            str(tmp_path / "mypy_cache"),
            str(path),
        ],
        capture_output=True,
        text=True,
        cwd=_ROOT,
    )


def _ty(path: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["uvx", "ty", "check", str(path)], capture_output=True, text=True, cwd=_ROOT
    )


def test_mypy_sample_checks_clean(tmp_path):
    r = _mypy(tmp_path, _SAMPLE)
    assert r.returncode == 0, f"expected clean, got:\n{r.stdout}\n{r.stderr}"


def test_mypy_sample_checks_clean_on_python_38(tmp_path):
    r = _mypy(tmp_path, _SAMPLE, "--python", "3.8", "--from", "mypy==1.14.1")
    assert r.returncode == 0, f"expected clean, got:\n{r.stdout}\n{r.stderr}"


def test_ty_sample_checks_clean():
    r = _ty(_SAMPLE)
    assert r.returncode == 0, f"expected clean, got:\n{r.stdout}\n{r.stderr}"


def test_mypy_flags_the_wrong_fixture(tmp_path):
    r = _mypy(tmp_path, _WRONG)
    assert r.returncode != 0, "mypy accepted code with known errors"
    out = r.stdout + r.stderr
    for code in ("arg-type", "attr-defined"):
        assert f"[{code}]" in out, f"expected [{code}] diagnostic, got:\n{out}"
    flagged = set(re.findall(r"formats_typed_wrong\.py:(\d+): error", out))
    assert len(flagged) >= _WRONG_LINES, f"expected a diagnostic per wrong line:\n{out}"


def test_ty_flags_the_wrong_fixture():
    r = _ty(_WRONG)
    assert r.returncode != 0, "ty accepted code with known errors"
    out = r.stdout + r.stderr
    flagged = set(re.findall(r"formats_typed_wrong\.py:(\d+)", out))
    assert len(flagged) >= _WRONG_LINES, f"expected a diagnostic per wrong line:\n{out}"
