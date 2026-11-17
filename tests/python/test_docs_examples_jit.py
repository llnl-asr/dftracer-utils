# SPDX-License-Identifier: MIT
"""The runnable files of examples/jit that the JIT tutorial and docs/source/jit.rst
include: each tutorial step prints what its expected file holds, and each reference
file runs and checks its own results."""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("pandas")
pytest.importorskip("pyarrow")

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)

EXAMPLES = Path(__file__).resolve().parents[2] / "examples" / "jit"
STEPS = sorted(p.stem for p in EXAMPLES.glob("step*.py"))
REFS = sorted(p.stem for p in EXAMPLES.glob("ref_*.py"))


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    out = tmp_path_factory.mktemp("docs_trace")
    subprocess.run([sys.executable, str(EXAMPLES / "make_trace.py"), str(out)], check=True)
    return str(out)


def _run(name, trace):
    return subprocess.run(
        [sys.executable, str(EXAMPLES / f"{name}.py"), trace],
        capture_output=True,
        text=True,
        timeout=600,
    )


def test_every_example_is_listed():
    assert len(STEPS) == 6 and len(REFS) == 5
    for name in STEPS:
        assert (EXAMPLES / "expected" / f"{name}.txt").is_file()


@pytest.mark.parametrize("name", STEPS)
def test_a_tutorial_step_prints_its_expected_output(name, trace):
    done = _run(name, trace)
    assert done.returncode == 0, done.stderr
    assert done.stdout == (EXAMPLES / "expected" / f"{name}.txt").read_text(encoding="utf-8")


@pytest.mark.parametrize("name", REFS)
def test_a_reference_example_runs_and_its_checks_hold(name, trace):
    done = _run(name, trace)
    assert done.returncode == 0, done.stderr
