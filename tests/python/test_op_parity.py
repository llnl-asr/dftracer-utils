"""The Python surface of Series / DataFrame / LazyFrame against the op
registry, in both directions (scripts/check_op_parity_doxygen.py --python).
A Python method with no op behind it, or a registered op no Python method
reaches, fails here; the Doxygen half of the same script covers the C++
methods and runs from the docs build instead."""

import importlib.util
import pathlib

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

_SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "scripts" / "check_op_parity_doxygen.py"


def _load_script():
    spec = importlib.util.spec_from_file_location("check_op_parity_doxygen", _SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_python_surface_matches_registry():
    problems = _load_script().check_python()
    assert problems == [], "\n".join(problems)


def test_python_only_debt_is_measured():
    # The debt list is the number section 19 part 3 asked for; it only shrinks.
    module = _load_script()
    total = sum(len(v) for v in module.PYTHON_ONLY_DEBT.values())
    assert total == 0, "PYTHON_ONLY_DEBT grew; land the op instead of listing it"
