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
}

# The same for the other public modules, by module.
_NO_ENTRY = "no reference entry yet"
_NOT_IN_API_REFERENCE_BY_MODULE: dict = {
    "dftracer.utils": _NOT_IN_API_REFERENCE,
    "dftracer.utils.dask": {
        **{
            name: _NO_ENTRY
            for name in (
                "DFTracerUtilsDaskWorkerPlugin",
                "DistributedIndexResult",
                "ProgressAggregator",
                "QueryPage",
                "distributed_write_trace",
            )
        },
        "DaskFrame": "in guides/scale/distributed-frame.rst",
    },
    "dftracer.utils.duql": {
        "Named": "the aliased column of the builder, in plugins.rst",
        "SortKey": _NO_ENTRY,
        "case_": "in api/duql.rst",
        "load_path": "in api/duql.rst",
    },
    "dftracer.utils.index": {
        "ExplainEntry": "typed dict that Indexer.explain returns",
        "ExplainExtension": "typed dict that Indexer.explain returns",
        "ExplainSummary": "typed dict that Indexer.explain returns",
        "ManifestEntry": "typed dict that Indexer.manifest returns",
        "ManifestExtension": "typed dict that Indexer.manifest returns",
    },
    "dftracer.utils.jit.ops": {"names": _NO_ENTRY, "info": _NO_ENTRY},
    "dftracer.utils.typing": {
        "LazyGroupBy": "returned by LazyFrame.group_by, in guides/data/pandas-polars.rst",
        "Named": "the aliased expression, in plugins.rst",
        "Value": "the scalar type a literal takes (a type alias)",
    },
    "dftracer.utils.schemas": {
        "JsonValue": "type alias of a JSON value tree, in schema signatures"
    },
    "dftracer.utils.pandas": {
        "merge": "pandas-named function, in guides/data/pandas-polars.rst",
        "read_parquet": "in guides/analysis/export.rst",
    },
    "dftracer.utils.plugin_cli": {"main": "the dftracer_plugin command, in plugins.rst"},
    "dftracer.utils.plugins": {
        "unnest": "in guides/dataframes.rst",
        "JSONValue": "type alias of a plugin config value (a JSON value tree)",
    },
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
        for name in ("Batch", "MAX_DEPTH", "Namespace", "Scope")
    },
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
    "dftracer.utils.store": ("guides/runtime/key-value-store.rst",),
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


def test_all_names_dtype_and_not_modules_or_other_areas():
    assert "DType" in dft.__all__
    for name in ("schemas", "Session", "Handle", "DFTUtilsError", "Phase", "Store", "Indexer"):
        assert name not in dft.__all__


def test_every_error_derives_from_dftutils_error():
    errors = [n for n in dft.errors.__all__ if n.startswith("DFTUtils") and n.endswith("Error")]
    assert len(errors) == 11
    assert len(errors) == len(dft.errors.__all__)
    for name in errors:
        assert issubclass(getattr(dft.errors, name), dft.errors.DFTUtilsError)


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


def _public_modules():
    names = ["dftracer.utils"]
    for info in pkgutil.walk_packages(dft.__path__, "dftracer.utils."):
        parts = info.name.split(".")[2:]
        if any(part.startswith("_") for part in parts) or parts[-1] == "dftracer_utils_ext":
            continue
        names.append(info.name)
    return names


_PUBLIC_MODULES = _public_modules()
_MODULES_WITH_ALL = list(_PUBLIC_MODULES)


def _import(name):
    try:
        return importlib.import_module(name)
    except ImportError as exc:
        pytest.skip(f"{name} needs an optional dependency: {exc}")


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
_NOT_IN_ALL: dict = {}


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


# The top-level names, as numpy keeps its main namespace: the DataFrame engine
# and the table functions. Every other name lives only in its area module.
_TOP_LEVEL = frozenset(
    {
        "DataFrame",
        "Series",
        "LazyFrame",
        "GroupBy",
        "Expr",
        "F",
        "col",
        "lit",
        "call",
        "where",
        "when",
        "if_else",
        "eval_many",
        "count",
        "Agg",
        "AggOp",
        "Udf",
        "udf",
        "Udaf",
        "udaf",
        "lazy",
        "collect_all",
        "LazyResult",
        "LazyScalar",
        "BoundColumn",
        "SinkSummary",
        "TraceWriteOptions",
        "DType",
        "TimeUnit",
        "GroupKey",
        "scan_file",
    }
)

# The compatibility namespaces re-export the engine names by purpose.
_COMPATIBILITY_NAMESPACES = frozenset({"dftracer.utils.pandas", "dftracer.utils.polars"})


def test_the_top_level_all_is_the_exact_allow_list():
    assert sorted(dft.__all__) == sorted(_TOP_LEVEL)


def test_no_public_name_is_exported_from_two_public_modules():
    homes: dict = {}
    for name in _MODULES_WITH_ALL:
        if name in _COMPATIBILITY_NAMESPACES:
            continue
        module = _import(name)
        for attr in module.__all__:
            homes.setdefault(id(getattr(module, attr)), {}).setdefault(name, []).append(attr)
    twice = sorted(
        sorted(f"{mod}.{attrs[0]}" for mod, attrs in modules.items())
        for modules in homes.values()
        if len(modules) > 1
    )
    assert not twice, f"one object, two import paths: {twice}"


def test_every_public_module_has_a_non_empty_all():
    # A module is public when no component of its path starts with an underscore;
    # one that is not a home of public names is private by name.
    bare = []
    for name in _PUBLIC_MODULES:
        module = _import(name)
        if not getattr(module, "__all__", None):
            bare.append(name)
    assert not bare, f"public modules with no __all__ (make them private): {bare}"


_DOC_TARGET = re.compile(
    r"(?:^\s*\.\. (?:py:)?(?:auto)?(?:class|function|exception|data|attribute|decorator|module"
    r"|method)::\s*|:(?:mod|class|func|meth|attr|exc|data|obj):`[~!.]*)"
    r"(dftracer\.utils(?:\.\w+)*)",
    re.MULTILINE,
)


def _resolve_doc_target(path):
    """The module that holds ``path`` and the first name taken from it."""
    parts = path.split(".")
    for end in range(len(parts), 1, -1):
        prefix = ".".join(parts[:end])
        try:
            module = importlib.import_module(prefix)
        except ImportError:
            continue
        return module, parts[end] if end < len(parts) else None
    return None, None


@pytest.mark.skipif(not _API_DOCS.is_dir(), reason="docs/source is not in this tree")
def test_docs_name_only_public_homes():
    bad = []
    for page in sorted(_API_DOCS.rglob("*.rst")):
        for match in _DOC_TARGET.finditer(page.read_text(encoding="utf-8")):
            path = match.group(1)
            module, first = _resolve_doc_target(path)
            where = f"{page.relative_to(_API_DOCS)}: {path}"
            if module is None:
                continue
            if any(part.startswith("_") for part in module.__name__.split(".")):
                bad.append(f"{where} (private module)")
            elif first is not None and first not in getattr(module, "__all__", ()):
                if not hasattr(module, first) or not isinstance(
                    getattr(module, first), types.ModuleType
                ):
                    bad.append(f"{where} ({first} is not in {module.__name__}.__all__)")
    assert not bad, "docs point at a path that is not a public home:\n" + "\n".join(bad[:60])


# The one native module. Its classes are static types whose __module__ cannot
# change, and no page tells a user to import from it.
_NATIVE_MODULE = "dftracer_utils_ext"


# What a public module may hold that is neither in its __all__ nor a submodule:
# objects from the standard library and typing, whose __module__ is not ours.
_OTHER_ATTRIBUTES: dict = {}


def _foreign(obj):
    module = getattr(obj, "__module__", None) or type(obj).__module__
    return not module.startswith("dftracer")


@pytest.mark.parametrize("name", _MODULES_WITH_ALL)
def test_a_public_module_hands_out_only_its_all_and_submodules(name):
    module = _import(name)
    extra = sorted(
        attr
        for attr, obj in vars(module).items()
        if not attr.startswith("_")
        and attr not in module.__all__
        and not isinstance(obj, types.ModuleType)
        and not _foreign(obj)
        and attr not in _OTHER_ATTRIBUTES.get(name, ())
    )
    assert not extra, f"{name}: public attributes outside __all__ (make them private): {extra}"


@pytest.mark.parametrize(
    "name", [m for m in _MODULES_WITH_ALL if m not in _COMPATIBILITY_NAMESPACES]
)
def test_every_public_function_reports_its_home_and_a_class_its_own_module(name):
    module = _import(name)
    wrong = []
    for attr in module.__all__:
        obj = getattr(module, attr)
        origin = getattr(obj, "__module__", "") or ""
        if isinstance(obj, (types.FunctionType, types.BuiltinFunctionType)):
            if origin.startswith("dftracer.utils") and origin != name:
                wrong.append(f"{attr}: function reports {origin}")
        elif isinstance(obj, type) and origin.startswith("dftracer"):
            # A class keeps its defining module so typing.get_type_hints reads
            # its annotations there; that module must be ours and must hold it.
            owner = importlib.import_module(
                "dftracer.utils.dftracer_utils_ext" if origin == "dftracer_utils_ext" else origin
            )
            if getattr(owner, obj.__name__, None) is not obj:
                wrong.append(f"{attr}: class is not in {origin}")
    assert not wrong, f"{name}: {wrong}"


def test_get_type_hints_works_on_the_public_classes_and_functions():
    from dftracer.utils.index import Indexer
    from dftracer.utils.trace import TraceViewer

    lazy_names = {"pa": None, "Client": None, "Future": None}
    for obj in (dft.DataFrame, dft.Series, dft.LazyFrame, dft.Expr, TraceViewer, Indexer):
        assert isinstance(typing.get_type_hints(obj, localns=lazy_names), dict), obj
    for fn in (dft.col, dft.lit, dft.scan_file, dft.count, dft.lazy):
        assert isinstance(typing.get_type_hints(fn, localns=lazy_names), dict), fn


@pytest.mark.skipif(not _API_DOCS.is_dir(), reason="docs/source is not in this tree")
def test_no_page_tells_a_user_to_import_from_the_native_module():
    pattern = re.compile(
        r"(?:from|import) dftracer\.utils\.dftracer_utils_ext|dftracer\.utils\.dftracer_utils_ext\.\w+"
    )
    hits = [
        str(page.relative_to(_API_DOCS))
        for page in _API_DOCS.rglob("*.rst")
        if pattern.search(page.read_text(encoding="utf-8"))
    ]
    assert not hits, f"{_NATIVE_MODULE} is not a public home: {hits}"
