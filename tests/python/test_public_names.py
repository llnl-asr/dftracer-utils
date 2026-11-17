# SPDX-License-Identifier: MIT
"""The names of the top-level package: __all__ is the public surface, each name
resolves, and each is in the API reference or listed here with a reason."""

import re
import types
from pathlib import Path

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as dft  # noqa: E402

_API_DOCS = Path(__file__).resolve().parents[2] / "docs" / "source"

# Public names with no autodoc entry in docs/source, and where they are
# described instead.
_NOT_IN_API_REFERENCE = {
    "F": "field builder, in guides/analysis and columnar-engine.rst",
    "Udf": "user-defined functions, in plugins.rst and guides/plugins/python-udfs.rst",
    "Udaf": "user-defined functions, in plugins.rst and guides/plugins/python-udfs.rst",
    "udf": "user-defined functions, in plugins.rst and guides/plugins/python-udfs.rst",
    "udaf": "user-defined functions, in plugins.rst and guides/plugins/python-udfs.rst",
    "call": "UDF call builder, in plugins.rst",
    "Store": "key-value store, in guides/runtime/key-value-store.rst",
    "LazyFrame": "lazy frame, in columnar-engine.rst and quickstart.rst",
    "lazy": "lazy frame constructor, in columnar-engine.rst and quickstart.rst",
    "scan_file": "lazy file scan, in plugins.rst",
    "write_shard_set": "distributed index merge, in guides/scale/distributed-index.rst",
    "consolidate_shard_set": "distributed index merge, in guides/scale/distributed-index.rst",
}

_DIRECTIVE = (
    r"^\s*\.\. (?:py:)?(?:auto)?(?:class|function|exception|data|attribute|decorator)"
    r"::\s*(?:dftracer\.utils\.)?(?:\w+\.)*{name}\b"
)


def _public_names():
    return {
        name
        for name in dir(dft)
        if not name.startswith("_") and not isinstance(getattr(dft, name), types.ModuleType)
    }


def test_all_is_the_public_names():
    assert len(dft.__all__) == len(set(dft.__all__))
    assert set(dft.__all__) == _public_names()


def test_every_name_in_all_resolves():
    for name in dft.__all__:
        assert hasattr(dft, name), name


def test_all_names_the_errors_and_dtype_and_not_modules():
    for name in (
        "DFTUtilsError",
        "DFTUtilsValueError",
        "DFTUtilsNotFoundError",
        "DFTUtilsIOError",
        "DFTUtilsParseError",
        "DFTUtilsCompressionError",
        "DFTUtilsDuqlError",
        "DFTUtilsReaderError",
        "DFTUtilsIndexerError",
        "DFTUtilsPipelineError",
        "DFTUtilsAggregationError",
        "DType",
    ):
        assert name in dft.__all__
    for name in ("schemas", "Session", "Handle"):
        assert name not in dft.__all__


def test_every_error_derives_from_dftutils_error():
    errors = [n for n in dft.__all__ if n.startswith("DFTUtils") and n.endswith("Error")]
    assert len(errors) == 11
    for name in errors:
        assert issubclass(getattr(dft, name), dft.DFTUtilsError)


@pytest.mark.skipif(not _API_DOCS.is_dir(), reason="docs/source is not in this tree")
def test_every_name_is_documented_or_listed_with_a_reason():
    text = "\n".join(p.read_text(encoding="utf-8") for p in _API_DOCS.glob("api/*.rst"))
    missing = [
        name
        for name in dft.__all__
        if name not in _NOT_IN_API_REFERENCE
        and not re.search(_DIRECTIVE.format(name=re.escape(name)), text, re.MULTILINE)
    ]
    assert not missing, f"not in docs/source/api: {missing}"
    stale = [
        name
        for name in _NOT_IN_API_REFERENCE
        if name in dft.__all__
        and re.search(_DIRECTIVE.format(name=re.escape(name)), text, re.MULTILINE)
    ]
    assert not stale, f"documented now, drop from _NOT_IN_API_REFERENCE: {stale}"
