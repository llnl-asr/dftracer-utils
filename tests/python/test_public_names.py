# SPDX-License-Identifier: MIT
"""The public names: every public module has an explicit __all__ that is its
public surface, each name resolves, none is a typing helper, a stdlib module or
a re-exported import, and each is in the API reference or listed here with a
reason."""

import importlib
import inspect
import pkgutil
import re
import types
import typing
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

# The same for the other public modules, by module. The top-level names above
# are the ones the package re-exports.
_NO_ENTRY = "no reference entry yet"
_SAME_AS_TOP_LEVEL = {name: reason for name, reason in _NOT_IN_API_REFERENCE.items()}
_NOT_IN_API_REFERENCE_BY_MODULE: dict = {
    "dftracer.utils": _NOT_IN_API_REFERENCE,
    "dftracer.utils.agg": {k: v for k, v in _SAME_AS_TOP_LEVEL.items() if k in ("Udaf", "udaf")},
    "dftracer.utils.dask": {
        name: _NO_ENTRY
        for name in (
            "DFTracerUtilsDaskWorkerPlugin",
            "DistributedIndexResult",
            "ProgressAggregator",
            "QueryPage",
            "distributed_write_trace",
        )
    },
    "dftracer.utils.dask.frame": {"DaskFrame": "in guides/scale/distributed-frame.rst"},
    "dftracer.utils.duql": {
        "Named": "the aliased column of the builder, in plugins.rst",
        "SortKey": _NO_ENTRY,
        "case_": "in api/duql.rst",
        "load_path": "in api/duql.rst",
    },
    "dftracer.utils.expr": {
        "F": _SAME_AS_TOP_LEVEL["F"],
        "Udf": _SAME_AS_TOP_LEVEL["Udf"],
        "udf": _SAME_AS_TOP_LEVEL["udf"],
        "call": _SAME_AS_TOP_LEVEL["call"],
        "Named": "the aliased expression, in plugins.rst",
        "Value": "the scalar type a literal takes (a type alias)",
        "when": "conditional expression, in guides/core/columnar-ops.rst",
        "if_else": _NO_ENTRY,
    },
    "dftracer.utils.index": {
        "ExplainEntry": "typed dict that Indexer.explain returns",
        "ExplainExtension": "typed dict that Indexer.explain returns",
        "ExplainSummary": "typed dict that Indexer.explain returns",
        "ManifestEntry": "typed dict that Indexer.manifest returns",
        "ManifestExtension": "typed dict that Indexer.manifest returns",
    },
    "dftracer.utils.jit.ops": {"names": _NO_ENTRY, "info": _NO_ENTRY},
    "dftracer.utils.jit_op": {
        "Op": _NO_ENTRY,
        "compile_op": _NO_ENTRY,
        "run_op": _NO_ENTRY,
        "run_op_array": _NO_ENTRY,
        "lower_to_expr": _NO_ENTRY,
        "can_fuse": "in guides/plugins/compose-ops.rst",
    },
    "dftracer.utils.lazyframe": {
        **{k: v for k, v in _SAME_AS_TOP_LEVEL.items() if k in ("LazyFrame", "lazy", "scan_file")},
        "LazyGroupBy": "returned by LazyFrame.group_by, in guides/data/pandas-polars.rst",
    },
    "dftracer.utils.pandas": {
        "merge": "pandas-named function, in guides/data/pandas-polars.rst",
        "read_parquet": "in guides/analysis/export.rst",
    },
    "dftracer.utils.plugin_cli": {"main": "the dftracer_plugin command, in plugins.rst"},
    "dftracer.utils.plugins": {"unnest": "in guides/dataframes.rst"},
    "dftracer.utils.polars": {
        "read_parquet": "in guides/analysis/export.rst",
        "scan_parquet": _NO_ENTRY,
    },
    "dftracer.utils.runtime": {
        "RuntimeProgress": "typed dict of Runtime.progress",
        "TaskProgress": "typed dict of Runtime.progress",
        "WorkerProgress": "typed dict of Runtime.progress",
    },
    "dftracer.utils.store": {
        name: "type of the Store API, in guides/runtime/key-value-store.rst"
        for name in ("Batch", "Namespace", "Scope")
    },
    "dftracer.utils.store.codec": {"MAX_DEPTH": _NO_ENTRY, "StoreAware": _NO_ENTRY},
}
for _name in ("BytesLike", "CopyInfo", "Encodable", "PackReport", "ScopeState"):
    _NOT_IN_API_REFERENCE_BY_MODULE["dftracer.utils.store"][_name] = _NO_ENTRY
for _name in ("Snapshot", "SourceStat", "StoreAware"):
    _NOT_IN_API_REFERENCE_BY_MODULE["dftracer.utils.store"][_name] = _NO_ENTRY

# A module whose names are described in prose, not autodoc: the pages under
# docs/source that name each of them as a literal (``name`` or ``prefix.name``).
# The jit vocabulary (monoids, scalar types, channels) compiles a body and never
# runs, so autodoc has nothing to show; docs/source/jit.rst describes the
# decorators and its tables the prims, and each other name carries a docstring.
_DOCUMENTED_BY_DOCSTRING = frozenset({"dftracer.utils.jit"})

_DESCRIBED_IN: dict = {
    "dftracer.utils.jit": ("jit.rst",),
    "dftracer.utils.jit.ops": ("jit.rst",),
    "dftracer.utils.jit_op": ("jit.rst",),
    "dftracer.utils.store": ("guides/runtime/key-value-store.rst",),
    "dftracer.utils.store.codec": ("guides/runtime/key-value-store.rst",),
    "dftracer.utils.duql": ("guides/core/duql.rst", "api/duql.rst"),
    "dftracer.utils.pandas": ("guides/data/pandas-polars.rst",),
    "dftracer.utils.polars": ("guides/data/pandas-polars.rst",),
    "dftracer.utils.plugins": ("plugins.rst",),
    "dftracer.utils.dask": (
        "guides/scale/distributed-index.rst",
        "guides/scale/distributed-frame.rst",
        "guides/scale/distributed-aggregation.rst",
        "api/trace_viewer.rst",
    ),
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


# Submodules of a package that the package re-exports. The package's __all__ is
# the public surface; the submodule holds the implementation.
_REACHED_THROUGH_PACKAGE = frozenset(
    {
        "dftracer.utils.dask.index",
        "dftracer.utils.dask.progress",
        "dftracer.utils.dask.viewer",
        "dftracer.utils.dask.worker",
        "dftracer.utils.duql.col",
        "dftracer.utils.duql.pipe",
        "dftracer.utils.jit.compiler",
        "dftracer.utils.jit.maps",
        "dftracer.utils.jit.monoids",
        "dftracer.utils.jit.package",
        "dftracer.utils.jit.plugin",
        "dftracer.utils.jit.prims",
        "dftracer.utils.jit.types",
        "dftracer.utils.jit.vfold",
        "dftracer.utils.store.store",
        "dftracer.utils.trace.lazy",
        "dftracer.utils.trace.live",
        "dftracer.utils.trace.partials",
        "dftracer.utils.trace.viewer",
    }
)


def _public_modules():
    names = ["dftracer.utils"]
    for info in pkgutil.walk_packages(dft.__path__, "dftracer.utils."):
        parts = info.name.split(".")[2:]
        if any(part.startswith("_") for part in parts) or parts[-1] == "dftracer_utils_ext":
            continue
        names.append(info.name)
    return names


_PUBLIC_MODULES = _public_modules()
_MODULES_WITH_ALL = [m for m in _PUBLIC_MODULES if m not in _REACHED_THROUGH_PACKAGE]


def _import(name):
    try:
        return importlib.import_module(name)
    except ImportError as exc:
        pytest.skip(f"{name} needs an optional dependency: {exc}")


def test_the_modules_reached_through_a_package_are_real_and_have_no_all():
    assert _REACHED_THROUGH_PACKAGE <= set(_PUBLIC_MODULES)
    for name in sorted(_REACHED_THROUGH_PACKAGE):
        module = _import(name)
        assert not hasattr(module, "__all__"), f"{name} has an __all__, drop it from the set"


@pytest.mark.parametrize("name", _MODULES_WITH_ALL)
def test_every_public_module_has_an_all_with_no_duplicates(name):
    module = _import(name)
    assert isinstance(getattr(module, "__all__", None), list), f"{name} has no __all__ list"
    assert module.__all__, f"{name} has an empty __all__"
    assert len(module.__all__) == len(set(module.__all__))


@pytest.mark.parametrize("name", _MODULES_WITH_ALL)
def test_every_name_in_a_module_all_is_a_public_definition(name):
    module = _import(name)
    for attr in module.__all__:
        assert not attr.startswith("_"), f"{name}.{attr} is private"
        assert hasattr(module, attr), f"{name}.{attr} does not resolve"
        obj = getattr(module, attr)
        assert not isinstance(obj, types.ModuleType), f"{name}.{attr} is a module"
        assert not isinstance(
            obj, (typing.TypeVar, getattr(typing, "ParamSpec", typing.TypeVar))
        ), f"{name}.{attr} is a type variable"
        assert obj is not getattr(typing, attr, None), f"{name}.{attr} is a typing helper"
        origin = getattr(obj, "__module__", None)
        if isinstance(origin, str) and origin not in ("typing", "typing_extensions"):
            assert origin.startswith("dftracer"), f"{name}.{attr} is imported from {origin}"


def _literal(attr):
    return re.compile(r"``(?:[\w.]*\.)?" + re.escape(attr) + r"\b")


@pytest.mark.skipif(not _API_DOCS.is_dir(), reason="docs/source is not in this tree")
@pytest.mark.parametrize("name", _MODULES_WITH_ALL)
def test_every_name_in_a_module_all_is_documented_or_listed(name):
    module = _import(name)
    text = "\n".join(p.read_text(encoding="utf-8") for p in _API_DOCS.rglob("*.rst"))
    listed = _NOT_IN_API_REFERENCE_BY_MODULE.get(name, {})
    prose = "\n".join(
        (_API_DOCS / f).read_text(encoding="utf-8") for f in _DESCRIBED_IN.get(name, ())
    )
    missing = [
        attr
        for attr in module.__all__
        if attr not in listed
        and not re.search(_DIRECTIVE.format(name=re.escape(attr)), text, re.MULTILINE)
        and not _literal(attr).search(prose)
        and not (name in _DOCUMENTED_BY_DOCSTRING and inspect.getdoc(getattr(module, attr)))
    ]
    assert not missing, f"{name}: not in docs/source and not listed: {missing}"
    stale = [
        attr
        for attr in listed
        if attr not in module.__all__
        or re.search(_DIRECTIVE.format(name=re.escape(attr)), text, re.MULTILINE)
    ]
    assert not stale, f"{name}: drop from the not-in-API list: {stale}"


# Public classes and functions a module defines, or loads by name, that its
# __all__ leaves out, with the reason. Everything else a module defines is in
# its __all__.
_ENGINE_HELPER = "helper that other engine modules import, not user API"
_NOT_IN_ALL: dict = {
    "dftracer.utils.agg": {
        name: _ENGINE_HELPER
        for name in (
            "NamedAggLike",
            "UdafAgg",
            "agg_specs_from_pandas_forms",
            "flatten_agg_specs",
            "has_agg_leaf",
            "reduce_specs",
            "select_aggs",
            "split_aggs",
            "to_agg",
        )
    },
    "dftracer.utils.dataframe": {name: _ENGINE_HELPER for name in ("as_names", "single_index")},
    "dftracer.utils.enums": {
        name: _ENGINE_HELPER
        for name in ("coerce_bytes", "coerce_duration", "dt_unit", "rule_to_units")
    },
    "dftracer.utils.expr": {
        name: "expression node or helper that other engine modules import, not user API"
        for name in (
            "AggLeaf",
            "Bin",
            "Cast",
            "Clip",
            "Cmp",
            "Col",
            "ColumnOp",
            "ColumnOpNode",
            "Fillna",
            "IsNull",
            "Lit",
            "Logical",
            "Prim",
            "Select",
            "Unary",
            "Wildcard",
            "as_expr",
            "check_quantile",
            "collect_columns",
            "duql_string",
            "emit_ast",
            "has_column_op",
            "resolve_selectors",
            "split_column_ops",
            "wrap",
        )
    },
    "dftracer.utils.groupby": {
        name: _ENGINE_HELPER
        for name in ("GroupResample", "GroupRolling", "group_quantile", "trace_group")
    },
    "dftracer.utils.jit_op": {
        name: _ENGINE_HELPER for name in ("Stage", "c_type", "emit_inline", "is_float_type")
    },
    "dftracer.utils.lazyframe": {
        name: _ENGINE_HELPER for name in ("agg_spec_strings", "dynamic_aggs")
    },
    "dftracer.utils.series": {
        name: _ENGINE_HELPER
        for name in ("norm_form_code", "require_utf8", "translate_pairs", "wrap_flag_bits")
    },
}


def _defined_public_names(module):
    names = {
        attr
        for attr, obj in vars(module).items()
        if not attr.startswith("_")
        and (inspect.isclass(obj) or inspect.isfunction(obj))
        and getattr(obj, "__module__", None) == module.__name__
    }
    # The names a package loads from a submodule on first use.
    names |= {attr for attr in getattr(module, "_SUBMODULE", ()) if not attr.startswith("_")}
    return names


@pytest.mark.parametrize("name", _MODULES_WITH_ALL)
def test_every_public_definition_of_a_module_is_in_its_all_or_listed(name):
    module = _import(name)
    listed = _NOT_IN_ALL.get(name, {})
    missing = sorted(_defined_public_names(module) - set(module.__all__) - set(listed))
    assert not missing, f"{name}: public but not in __all__ and not listed: {missing}"
    stale = sorted(attr for attr in listed if attr in module.__all__)
    assert not stale, f"{name}: now in __all__, drop from _NOT_IN_ALL: {stale}"
