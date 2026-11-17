# SPDX-License-Identifier: MIT
"""The four native types take weak references, and the stub in
dftracer_utils_ext.pyi lists the same members as the compiled classes."""

import ast
import gc
import weakref
from pathlib import Path

import pytest

import dftracer.utils as d
from dftracer.utils import dftracer_utils_ext as _ext
from dftracer.utils.series import Series

_PYI = Path(__file__).resolve().parents[2] / "python/dftracer/utils/dftracer_utils_ext.pyi"
_TYPES = ("Series", "DataFrame", "LazyFrame", "TraceViewer")


def _instances():
    frame = d.DataFrame({"x": [1, 2, 3]})
    return {
        "Series": Series([1, 2, 3]),
        "DataFrame": frame,
        "LazyFrame": frame.lazy(),
    }


@pytest.mark.parametrize("name", _TYPES)
def test_native_type_has_a_weaklist_slot(name):
    assert getattr(_ext, name).__weakrefoffset__ > 0


@pytest.mark.parametrize("name", ["Series", "DataFrame", "LazyFrame"])
def test_weak_reference_dies_with_the_object(name):
    obj = _instances()[name]
    calls = []
    ref = weakref.ref(obj, calls.append)
    assert ref() is obj
    del obj
    gc.collect()
    assert ref() is None and len(calls) == 1


def test_weak_reference_to_a_series_and_a_derived_series():
    s = Series([1, 2, 3])
    t = s.abs()
    assert weakref.ref(s)() is s and weakref.ref(t)() is t
    wd = weakref.WeakValueDictionary()
    wd["s"] = s
    assert wd["s"] is s
    del s
    gc.collect()
    assert "s" not in wd


def test_native_subclass_of_a_type_has_weakrefs_too():
    class Sub(_ext.Series):
        __slots__ = ()

    assert Sub.__weakrefoffset__ == _ext.Series.__weakrefoffset__


def _stub_members(name):
    tree = ast.parse(_PYI.read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == name:
            out = set()
            for item in node.body:
                if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    out.add(item.name)
                elif isinstance(item, ast.AnnAssign) and isinstance(item.target, ast.Name):
                    out.add(item.target.id)
                elif isinstance(item, ast.Assign):
                    out.update(t.id for t in item.targets if isinstance(t, ast.Name))
            return out
    raise AssertionError(f"{name} is not in the stub")


@pytest.mark.parametrize("name", _TYPES)
def test_stub_and_runtime_list_the_same_members(name):
    stub = _stub_members(name)
    runtime = set(dir(getattr(_ext, name)))
    assert sorted(stub - runtime) == [], f"{name}: in the stub, not in the class"
    public = {m for m in runtime - stub if not (m.startswith("__") and m.endswith("__"))}
    assert sorted(public) == [], f"{name}: in the class, not in the stub"
