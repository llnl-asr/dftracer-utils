# SPDX-License-Identifier: MIT
"""Static-typing regression guard for the typed @jit.plugin DSL.

Runs ty and mypy --strict on two fixtures under tests/python/typing/: the
sample must check clean and cover every name in ``jit.__all__``, and the wrong
fixture must be flagged on every deliberate mistake. mypy also checks the jit
modules themselves. This locks the DSL signatures so a future edit cannot
silently loosen them.
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

from dftracer.utils import jit

_ROOT = Path(__file__).resolve().parents[2]
_TYPING_DIR = Path(__file__).parent / "typing"
_SAMPLE = _TYPING_DIR / "jit_typed_sample.py"
_WRONG = _TYPING_DIR / "jit_typed_wrong.py"
_JIT_SOURCES = [
    _ROOT / "python" / "dftracer" / "utils" / "jit" / "__init__.py",
    _ROOT / "python" / "dftracer" / "utils" / "jit" / "ops.py",
    _ROOT / "python" / "dftracer" / "utils" / "jit" / "_batch.py",
]

pytestmark = pytest.mark.skipif(
    shutil.which("uvx") is None, reason="uvx not available to run the type checkers"
)


def _ty(path: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["uvx", "ty", "check", str(path)],
        capture_output=True,
        text=True,
    )


def test_typed_sample_checks_clean():
    r = _ty(_SAMPLE)
    assert r.returncode == 0, f"expected clean, got:\n{r.stdout}\n{r.stderr}"


def test_wrong_fixture_is_flagged():
    r = _ty(_WRONG)
    assert r.returncode != 0, "type checker accepted code with known errors"
    out = r.stdout + r.stderr
    for rule in (
        "unsupported-operator",
        "missing-argument",
        "missing-attribute",
        "invalid-argument-type",
    ):
        assert rule in out, f"expected {rule} diagnostic, got:\n{out}"


def _mypy(tmp_path: Path, *paths: Path) -> subprocess.CompletedProcess:
    # Settings live in [tool.mypy] in pyproject.toml, read from the repo root.
    return subprocess.run(
        [
            "uvx",
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


def test_mypy_sample_and_jit_modules_check_clean(tmp_path):
    r = _mypy(tmp_path, _SAMPLE, *_JIT_SOURCES)
    assert r.returncode == 0, f"expected clean, got:\n{r.stdout}\n{r.stderr}"


def test_mypy_flags_the_wrong_fixture(tmp_path):
    r = _mypy(tmp_path, _WRONG)
    assert r.returncode != 0, "mypy accepted code with known errors"
    out = r.stdout + r.stderr
    for code in ("arg-type", "call-arg", "attr-defined", "call-overload", "operator", "misc"):
        assert f"[{code}]" in out, f"expected [{code}] diagnostic, got:\n{out}"
    wrong_lines = len(re.findall(r"jit_typed_wrong\.py:\d+: error:", out))
    assert wrong_lines >= 20, f"expected one diagnostic per wrong use, got {wrong_lines}:\n{out}"


def test_sample_covers_every_public_jit_name():
    src = _SAMPLE.read_text(encoding="utf-8")

    def used(name: str) -> bool:
        n = re.escape(name)
        return bool(re.search(rf"\bjit\.{n}\b", src) or re.search(rf"^\s+{n},$", src, re.M))

    missing = [n for n in jit.__all__ if not used(n)]
    assert not missing, f"jit_typed_sample.py never uses: {missing}"
