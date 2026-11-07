# SPDX-License-Identifier: MIT
"""Static-typing regression guard for the typed key-value store API.

The sample under tests/python/typing/ must check clean, which includes each
assert_type for the overloads. The wrong fixture must be flagged on every
deliberate mistake.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

_TYPING_DIR = Path(__file__).parent / "typing"
_SAMPLE = _TYPING_DIR / "store_typed_sample.py"
_WRONG = _TYPING_DIR / "store_typed_wrong.py"

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
    assert out.count("`Batch.put`") == 2, f"expected both put mistakes flagged, got:\n{out}"
    assert "no attribute `fold`" in out, f"expected the fold mistake, got:\n{out}"
    assert out.count("No overload of bound method `Snapshot.get`") == 2, out
    assert out.count("No overload of bound method `Store.namespace`") == 1, out
    assert "`register_type`" in out, f"expected the register_type mistake, got:\n{out}"
    assert "Found 8 diagnostics" in out, f"expected all 8 flagged, got:\n{out}"
