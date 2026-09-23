#!/usr/bin/env python3
"""
Guard the wheel payload: C++ development artifacts belong to `make install`.

Skipped unless a wheel is available. Point DFTRACER_UTILS_WHEEL at one, or
leave it in wheelhouse/ or dist/ at the repo root.
"""

import os
import zipfile
from glob import glob
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]

# Everything CMake installs must land under dftracer/; the rest is packaging.
ALLOWED_TOP_LEVEL = ("dftracer/",)
ALLOWED_TOP_LEVEL_SUFFIXES = (".dist-info", ".data")

FORBIDDEN_PREFIXES = (
    "dftracer/lib/cmake/",
    "dftracer/lib/pkgconfig/",
    "share/",
)
FORBIDDEN_SUFFIXES = (".a",)

REQUIRED_PATTERNS = (
    "dftracer/utils/__init__.py",
    "dftracer/utils/dftracer_utils_ext",
    "dftracer/lib/libdftracer_utils_core",
    "dftracer/lib/libdftracer_utils_utilities",
    "dftracer/bin/dftracer_index",
    "dftracer/bin/ldb",
    "dftracer/bin/sst_dump",
    # Plugin headers a pip-installed wheel needs to compile JIT plugins and the
    # dftracer_plugin CLI scaffold: prims.h is included by jit-generated code,
    # plugin.h by the CLI scaffold, abi.h by both.
    "dftracer/include/dftracer/utils/plugins/abi.h",
    "dftracer/include/dftracer/utils/plugins/prims.h",
    "dftracer/include/dftracer/utils/plugins/plugin.h",
    # The whole include/dftracer/utils tree ships (not a hand-picked subset) so
    # any public C/C++ ABI header compiles standalone from the wheel: spot
    # check headers from directories that were NOT part of the old subset.
    "dftracer/include/dftracer/utils/core/abi.h",
    "dftracer/include/dftracer/utils/core/coro/abi.h",
    "dftracer/include/dftracer/utils/dataframe/abi.h",
    "dftracer/include/dftracer/utils/query/abi.h",
    "dftracer/include/dftracer/utils/utilities/indexer/index_database.h",
)


def _include_tree_names(names):
    prefix = "dftracer/include/dftracer/utils/"
    return {n for n in names if n.startswith(prefix) and n.endswith(".h")}


def _source_tree_headers(repo_root):
    include_root = repo_root / "include" / "dftracer" / "utils"
    return {str(p.relative_to(include_root)) for p in include_root.rglob("*.h")}


def _find_wheel():
    explicit = os.environ.get("DFTRACER_UTILS_WHEEL")
    if explicit:
        return explicit
    for pattern in ("wheelhouse/*.whl", "dist/*.whl"):
        found = sorted(glob(str(REPO_ROOT / pattern)))
        if found:
            return found[-1]
    return None


@pytest.fixture(scope="module")
def wheel_names():
    wheel = _find_wheel()
    if wheel is None:
        pytest.skip("no wheel found (set DFTRACER_UTILS_WHEEL or build into wheelhouse/)")
    with zipfile.ZipFile(wheel) as zf:
        return wheel, zf.namelist()


def test_no_development_artifacts(wheel_names):
    wheel, names = wheel_names
    leaked = [
        n for n in names if n.startswith(FORBIDDEN_PREFIXES) or n.endswith(FORBIDDEN_SUFFIXES)
    ]
    assert not leaked, f"{Path(wheel).name} ships development artifacts: {leaked[:10]}"


def test_no_stray_top_level_entries(wheel_names):
    wheel, names = wheel_names
    stray = set()
    for name in names:
        top = name.split("/", 1)[0]
        if name.startswith(ALLOWED_TOP_LEVEL) or top.endswith(ALLOWED_TOP_LEVEL_SUFFIXES):
            continue
        stray.add(top)
    assert not stray, f"{Path(wheel).name} has unexpected top-level entries: {sorted(stray)}"


@pytest.mark.parametrize("required", REQUIRED_PATTERNS)
def test_runtime_payload_present(wheel_names, required):
    wheel, names = wheel_names
    assert any(n.startswith(required) for n in names), f"{Path(wheel).name} is missing {required}"


def test_full_include_tree_bundled(wheel_names):
    """The wheel ships the complete include/dftracer/utils tree, not a
    hand-picked subset: every source header must have a matching wheel
    entry."""
    wheel, names = wheel_names
    shipped = {n[len("dftracer/include/dftracer/utils/") :] for n in _include_tree_names(names)}
    expected = _source_tree_headers(REPO_ROOT)
    missing = expected - shipped
    assert not missing, f"{Path(wheel).name} is missing headers: {sorted(missing)[:20]}"
