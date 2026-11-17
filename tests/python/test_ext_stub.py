# SPDX-License-Identifier: MIT
"""docs/source/_ext_stub.py, the stand-in for the native extension that the
docs build installs, is generated from dftracer_utils_ext.pyi; the extension
declares what the .pyi declares and nothing more."""

import ast
import os
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")
from dftracer.utils import dftracer_utils_ext as ext  # noqa: E402

_ROOT = Path(__file__).resolve().parents[2]
_PYI = _ROOT / "python/dftracer/utils/dftracer_utils_ext.pyi"
# Opaque handles that are capsules at run time; the .pyi names them for the
# checkers.
_TYPING_ONLY = {"_UdfHandle", "_UdafHandle"}
_DEAD_EXPORTS = [
    "TraceReaderIterator",
    "_ArrowStreamingIterator",
    "_MemoryViewBatch",
    "read_arrow_files_parallel",
    "MetadataCollectorUtility",
    "IndexerCheckpoint",
]

_CHILD = """
import sys
sys.path[:0] = {paths!r}
from _ext_stub import install_extension_stub
install_extension_stub()
import dftracer.utils as d
ext = sys.modules["dftracer.utils.dftracer_utils_ext"]
assert not hasattr(ext, "__file__"), ext.__file__
assert d.Series.__mro__[-3:-1] and issubclass(d.Series, ext.Series)
for name in {declared!r}:
    assert hasattr(ext, name), name
assert not hasattr(ext, "no_such_symbol")
assert not hasattr(ext.Series, "no_such_method")
assert ext.Series.__module__ == "dftracer.utils.dftracer_utils_ext"
print("stub ok")
"""


def _declared():
    """The classes, functions and constants the .pyi declares, minus the type
    aliases and typed dicts that exist for the checkers only."""
    tree = ast.parse(_PYI.read_text(encoding="utf-8"))
    names = []
    for node in tree.body:
        if isinstance(node, ast.ClassDef):
            typed = any(isinstance(b, ast.Name) and b.id == "TypedDict" for b in node.bases)
            if not typed:
                names.append(node.name)
        elif isinstance(node, ast.FunctionDef):
            if node.name not in names:
                names.append(node.name)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            if node.value is None:
                names.append(node.target.id)
    return names


def test_the_committed_stub_equals_the_generated_one():
    run = subprocess.run(
        [sys.executable, str(_ROOT / "scripts" / "gen_ext_stub.py"), "--check"],
        capture_output=True,
        text=True,
        cwd=_ROOT,
    )
    assert run.returncode == 0, run.stderr


def test_the_stub_installs_and_the_package_imports_without_the_extension():
    paths = [str(_ROOT / "docs" / "source"), str(_ROOT / "python")]
    if sys.version_info < (3, 11):
        import typing_extensions

        paths.append(str(Path(typing_extensions.__file__).parent))
    env = {k: v for k, v in os.environ.items() if k != "PYTHONPATH"}
    run = subprocess.run(
        [sys.executable, "-I", "-S", "-c", _CHILD.format(paths=paths, declared=_declared())],
        capture_output=True,
        text=True,
        cwd=_ROOT,
        env=env,
    )
    assert run.returncode == 0 and "stub ok" in run.stdout, run.stderr + run.stdout


def test_the_extension_declares_what_the_stub_file_declares():
    names = set(_declared())
    runtime = {n for n in dir(ext) if not (n.startswith("__") and n.endswith("__"))}
    assert sorted(names - runtime - _TYPING_ONLY) == [], (
        "declared in the .pyi, not in the extension"
    )
    undeclared = runtime - names - {"Scalar", "T_co"}
    assert sorted(undeclared) == [], "in the extension, not in the .pyi"


@pytest.mark.parametrize("name", _DEAD_EXPORTS)
def test_a_dead_export_is_gone(name):
    assert not hasattr(ext, name)
    assert name not in _declared()
    assert name not in _STUB_TEXT.read_text(encoding="utf-8")


_STUB_TEXT = _ROOT / "docs" / "source" / "_ext_stub.py"


def test_the_stub_keeps_every_overload_and_the_docstrings():
    text = _STUB_TEXT.read_text(encoding="utf-8")
    assert '    @overload\n    def rolling(self, window: int) -> "Rolling": ...' in text
    assert "    def rolling(\n        self, window: int, op: Literal" in text
    assert "Exponentially weighted window" in text
    assert "class Rolling:" in text and "class Ewm:" in text
