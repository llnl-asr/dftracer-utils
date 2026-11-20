# SPDX-License-Identifier: MIT
"""The package type-checks under ``mypy --strict`` with no error, and carries
almost no ignore comments.

The checkers also run against the interpreter that runs the tests, so the
optional libraries installed there are seen with their own types.

The settings live in [tool.mypy] in pyproject.toml. The optional dependencies
(numpy, pandas, polars, pyarrow, dask, distributed) are not installed in the
isolated environment ``uvx`` makes, so they are untyped there by configuration,
not by a comment on each import.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_PACKAGE = _ROOT / "python" / "dftracer"
_MAX_IGNORE_COMMENTS = 9


@pytest.mark.skipif(
    shutil.which("uvx") is None, reason="uvx not available to run the type checkers"
)
def test_mypy_strict_package_is_clean(tmp_path):
    r = subprocess.run(
        [
            "uvx",
            "mypy",
            "--strict",
            "--cache-dir",
            str(tmp_path / "mypy_cache"),
            "python/dftracer",
        ],
        capture_output=True,
        text=True,
        cwd=_ROOT,
    )
    assert r.returncode == 0, f"mypy --strict found errors:\n{r.stdout}\n{r.stderr}"


@pytest.mark.skipif(
    shutil.which("uvx") is None, reason="uvx not available to run the type checkers"
)
def test_mypy_strict_package_is_clean_with_installed_libraries(tmp_path):
    r = subprocess.run(
        [
            "uvx",
            "mypy",
            "--strict",
            "--python-executable",
            sys.executable,
            "--cache-dir",
            str(tmp_path / "mypy_cache_installed"),
            "python/dftracer",
        ],
        capture_output=True,
        text=True,
        cwd=_ROOT,
    )
    assert r.returncode == 0, f"mypy --strict found errors:\n{r.stdout}\n{r.stderr}"


@pytest.mark.skipif(
    shutil.which("uvx") is None, reason="uvx not available to run the type checkers"
)
def test_ty_package_is_clean_with_installed_libraries():
    r = subprocess.run(
        ["uvx", "ty", "check", "--python", sys.prefix, "python/"],
        capture_output=True,
        text=True,
        cwd=_ROOT,
    )
    assert r.returncode == 0, f"ty found errors:\n{r.stdout}\n{r.stderr}"


def test_ignore_comments_stay_under_ten():
    found = []
    for path in sorted([*_PACKAGE.rglob("*.py"), *_PACKAGE.rglob("*.pyi")]):
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            found += [
                f"{path.relative_to(_ROOT)}:{number}"
                for _ in re.findall(r"#\s*(?:type|ty|mypy):\s*ignore", line)
            ]
    assert len(found) <= _MAX_IGNORE_COMMENTS, f"{len(found)} ignore comments:\n" + "\n".join(found)
