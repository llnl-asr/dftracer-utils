# SPDX-License-Identifier: MIT
"""Static-typing regression guard for @dft.udf.

Runs ty and mypy --strict on two fixtures under tests/python/typing/: the sample
must check clean and the wrong fixture must be flagged on every deliberate
mistake. mypy also checks the modules that compile and run the UDFs.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_TYPING_DIR = Path(__file__).parent / "typing"
_SAMPLE = _TYPING_DIR / "udf_typed_sample.py"
_WRONG = _TYPING_DIR / "udf_typed_wrong.py"
_SOURCES = [
    _ROOT / "python" / "dftracer" / "utils" / "jit" / "_udf.py",
    _ROOT / "python" / "dftracer" / "utils" / "_udf_lane.py",
]

pytestmark = pytest.mark.skipif(
    shutil.which("uvx") is None, reason="uvx not available to run the type checkers"
)


def _ty(path: Path) -> subprocess.CompletedProcess:
    # The fixtures import numpy, so ty reads the interpreter that runs the tests.
    return subprocess.run(
        ["uvx", "ty", "check", "--python", sys.prefix, str(path)], capture_output=True, text=True
    )


def _mypy(tmp_path: Path, *paths: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            "uvx",
            "--python",
            sys.executable,
            "mypy",
            "--strict",
            "--cache-dir",
            str(tmp_path / "mypy_cache"),
            *map(str, paths),
        ],
        capture_output=True,
        text=True,
        cwd=_ROOT,
    )


def test_ty_sample_checks_clean():
    r = _ty(_SAMPLE)
    assert r.returncode == 0, f"expected clean, got:\n{r.stdout}\n{r.stderr}"


def test_ty_flags_the_wrong_fixture():
    r = _ty(_WRONG)
    assert r.returncode != 0, "ty accepted code with known errors"
    out = r.stdout + r.stderr
    assert "invalid-argument-type" in out, out
    assert "unresolved-attribute" in out or "missing-attribute" in out, out
    assert len(re.findall(r"udf_typed_wrong\.py:\d+", out)) >= 8, out


def test_mypy_sample_and_udf_modules_check_clean(tmp_path):
    r = _mypy(tmp_path, _SAMPLE, *_SOURCES)
    assert r.returncode == 0, f"expected clean, got:\n{r.stdout}\n{r.stderr}"


def test_mypy_flags_the_wrong_fixture(tmp_path):
    r = _mypy(tmp_path, _WRONG)
    assert r.returncode != 0, "mypy accepted code with known errors"
    out = r.stdout + r.stderr
    for code in ("call-overload", "call-arg", "attr-defined", "index", "assignment"):
        assert f"[{code}]" in out, f"expected [{code}] diagnostic, got:\n{out}"
    wrong_lines = len(re.findall(r"udf_typed_wrong\.py:\d+: error:", out))
    assert wrong_lines >= 9, f"expected one diagnostic per wrong use, got {wrong_lines}:\n{out}"
