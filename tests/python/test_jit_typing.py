# SPDX-License-Identifier: MIT
"""Static-typing regression guard for the typed @jit.plugin DSL.

Runs the type checker on two fixtures under tests/python/typing/: the sample
must check clean (inference works with no annotations), and the wrong fixture
must be flagged on every deliberate mistake. This locks the DSL signatures so a
future edit cannot silently loosen them.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

_TYPING_DIR = Path(__file__).parent / "typing"
_SAMPLE = _TYPING_DIR / "jit_typed_sample.py"
_WRONG = _TYPING_DIR / "jit_typed_wrong.py"

pytestmark = pytest.mark.skipif(
    shutil.which("uvx") is None, reason="uvx not available to run the ty type checker"
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
        "unresolved-attribute",
        "invalid-argument-type",
    ):
        assert rule in out, f"expected {rule} diagnostic, got:\n{out}"
