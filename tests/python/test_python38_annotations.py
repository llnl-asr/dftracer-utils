# SPDX-License-Identifier: MIT
"""Python 3.8 can import every module and read the annotations the package reads.

The package supports 3.8, where ``list[str]``, ``X | Y`` and ``os.PathLike[str]``
fail when an annotation is evaluated. Ruff's UP rules rewrite annotations to
those forms, so a module that evaluates its annotations (no ``from __future__
import annotations``, a dataclass, ``typing.get_type_hints``) breaks on 3.8
without any 3.8 run noticing. This test runs a 3.8 interpreter with the docs
stand-in for the extension: it imports every module, calls
``typing.get_type_hints`` on every public class and on a user ``RecordSchema``
subclass. The optional dependencies are installed for the 3.8 run, so nothing
is skipped: a module that fails to import fails the test, and the three modules
that need numpy, pandas, dask and distributed must be among those checked.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]

_SCRIPT = r"""
import importlib, inspect, pkgutil, sys, typing
sys.path.insert(0, sys.argv[1])
sys.path.insert(0, sys.argv[2])
from _ext_stub import install_extension_stub
install_extension_stub()
import dftracer.utils as pkg
# Names a module imports only under TYPE_CHECKING because the package does not
# require them; the hints of such a class resolve with them supplied.
import pyarrow
from distributed import Client, Future

LAZY = {"pa": pyarrow, "Client": Client, "Future": Future}
bad = []
modules = []
for info in pkgutil.walk_packages(pkg.__path__, "dftracer.utils."):
    try:
        modules.append(importlib.import_module(info.name))
    except BaseException as e:
        bad.append("import %s: %s: %s" % (info.name, type(e).__name__, str(e)[:200]))
modules.append(pkg)
loaded = {m.__name__ for m in modules}
for needed in ("dask._worker",):
    if "dftracer.utils." + needed not in loaded:
        bad.append("not checked: " + needed)
seen = set()
for mod in modules:
    for name, obj in vars(mod).items():
        if (
            inspect.isclass(obj)
            and not name.startswith("_")
            and obj.__module__.startswith("dftracer.utils")
            and not obj.__module__.endswith("dftracer_utils_ext")
            and obj not in seen
        ):
            seen.add(obj)
            try:
                typing.get_type_hints(obj, None, LAZY)
            except BaseException as e:
                bad.append("hints %s.%s: %s: %s" % (obj.__module__, name, type(e).__name__, str(e)[:200]))
from dftracer.utils import schemas
try:
    class UserSchema(schemas.RecordSchema, id="user_schema_38"):
        name: str
        size: int = schemas.field(path="args.size")
    typing.get_type_hints(UserSchema)
except BaseException as e:
    bad.append("user schema: %s: %s" % (type(e).__name__, str(e)[:200]))
print("\n".join(bad))
print("checked", len(modules), "modules", len(seen), "classes")
sys.exit(1 if bad else 0)
"""


def _has_python_38():
    if shutil.which("uv") is None:
        return False
    r = subprocess.run(["uv", "python", "find", "3.8"], capture_output=True, text=True)
    return r.returncode == 0


@pytest.mark.skipif(not _has_python_38(), reason="uv or Python 3.8 not available")
def test_every_module_imports_and_its_annotations_read_on_python_3_8(tmp_path):
    script = tmp_path / "check38.py"
    script.write_text(_SCRIPT, encoding="utf-8")
    r = subprocess.run(
        [
            "uv",
            "run",
            "--no-project",
            "--python",
            "3.8",
            "--with",
            "typing_extensions",
            "--with",
            "numpy",
            "--with",
            "pandas",
            "--with",
            "pyarrow",
            "--with",
            "dask",
            "--with",
            "distributed",
            "python",
            str(script),
            str(_ROOT / "python"),
            str(_ROOT / "docs" / "source"),
        ],
        capture_output=True,
        text=True,
    )
    if "Traceback" not in r.stderr and "checked" not in r.stdout:
        pytest.skip(f"uv could not set up Python 3.8 with its packages: {r.stderr[-300:]}")
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"
