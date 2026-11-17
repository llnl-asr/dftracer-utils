# SPDX-License-Identifier: MIT
"""The Python API table, src/dftracer/utils/python/core/api.def: the native
module, the Expr methods and the type declarations all come from it, and the
public methods of Series, Expr, the accessors, DataFrame, LazyFrame and both
GroupBy classes that it does not give are listed with a reason in
api_allowlist.txt."""

import ast
import importlib
import importlib.util
import inspect
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import dftracer_utils_ext as ext  # noqa: E402
from dftracer.utils.columnar import Expr, GroupBy  # noqa: E402
from dftracer.utils.lazyframe import LazyGroupBy  # noqa: E402
from dftracer.utils.series import _DtAccessor, _ListAccessor, _StrAccessor  # noqa: E402

_ROOT = Path(__file__).resolve().parents[2]
_ALLOWLIST = Path(__file__).parent / "api_allowlist.txt"
# Duplicate-code findings of `pylint --enable=duplicate-code python/` today. It
# only goes down.
_DUPLICATE_CODE_FINDINGS = 18
_PYLINT_VERSION = "4.1.4"


def _alias_table():
    spec = importlib.util.spec_from_file_location(
        "dftu_aliases", _ROOT / "python" / "dftracer" / "utils" / "_aliases.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _api_def():
    spec = importlib.util.spec_from_file_location("api_def", _ROOT / "scripts" / "api_def.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["api_def"] = module
    spec.loader.exec_module(module)
    return module


ROWS = _api_def().rows()
SERIES_ROWS = [r for r in ROWS if r.cls == "Series"]
ACCESSOR_ROWS = [r for r in ROWS if r.cls in {"Str", "Dt", "List"}]
ACCESSOR_TYPES = {"Str": ext.StrAccessor, "Dt": ext.DtAccessor, "List": ext.ListAccessor}
PUBLIC_ACCESSORS = {"Str": _StrAccessor, "Dt": _DtAccessor, "List": _ListAccessor}


def _public(cls):
    return {n for n in dir(cls) if not n.startswith("_")}


def test_native_rows_equal_the_rows_of_the_file():
    native = [(*r[:6], r[6], tuple(r[7]), r[8], r[9]) for r in ext._api_rows()]
    assert native == [tuple(r) for r in ROWS]


@pytest.mark.parametrize("row", SERIES_ROWS, ids=[r.name for r in SERIES_ROWS])
def test_series_row_is_a_native_method_with_the_row_doc(row):
    method = getattr(ext.Series, row.name)
    assert method.__doc__ == row.doc
    assert row.name in ext.Series.__dict__


def _float_series():
    return dft.Series([1.5, -2.0, None, 4.0, -0.5])


def _input_for(row):
    """A named column the row's op accepts."""
    if row.name == "list_len":
        s = dft.Series(["a b", "c", None]).str.split(" ")
    elif row.name == "logical_not":
        s = dft.Series([True, False, None])
    elif row.name.startswith(("str_", "to_", "hex64_parse")) or row.name == "fnv1a":
        s = dft.Series(["a", " b ", None])
    elif row.name == "hex64_format":
        s = dft.Series([1, 255, None])
    else:
        s = dft.Series([1.5, -2.0, None, 4.0, -0.5])
    s.name = "x"
    return s


@pytest.mark.parametrize("row", SERIES_ROWS, ids=[r.name for r in SERIES_ROWS])
def test_series_row_runs_the_op_and_keeps_the_name(row):
    s = _input_for(row)
    method = getattr(ext.Series, row.name)
    if row.args == "NONE":
        out = method(s)
    elif row.args == "SCALAR":
        out = method(s, **{row.keywords[0]: 0.0})
    else:
        out = method(s, **dict(zip(row.keywords, (-1, 2))))
    if row.result in {"I64", "F64", "SCALAR", "ELEMENT"}:
        assert not isinstance(out, dft.Series)
        return
    assert type(out) is dft.Series
    if row.name not in {"unique", "drop_nulls"}:
        assert out.name == "x" or row.name == "cum_count"


def _accessor_input(row):
    if row.cls == "List" or row.name == "join":
        s = dft.Series(["a b", "c", None]).str.split(" ")
    else:
        s = dft.Series(["a", " Ab ", None, "12"])
    s.name = "x"
    return s


_ACCESSOR_ARGS = {"NONE": (), "STR": ("a",), "STR2": ("a", "b"), "INT": (1,)}


@pytest.mark.parametrize("row", ACCESSOR_ROWS, ids=[f"{r.cls}.{r.name}" for r in ACCESSOR_ROWS])
def test_accessor_row_is_a_native_method_with_the_row_doc(row):
    cls = ACCESSOR_TYPES[row.cls]
    assert getattr(cls, row.name).__doc__ == row.doc
    assert row.name in cls.__dict__
    assert row.name not in PUBLIC_ACCESSORS[row.cls].__dict__


@pytest.mark.parametrize("row", ACCESSOR_ROWS, ids=[f"{r.cls}.{r.name}" for r in ACCESSOR_ROWS])
def test_accessor_row_runs_its_op_like_the_series_method_of_the_op(row):
    s = _accessor_input(row)
    args = _ACCESSOR_ARGS[row.args]
    acc = getattr(s, row.cls.lower())
    out = getattr(acc, row.name)(*args)
    flat = getattr(s, row.op.rsplit(".", 1)[1])(*args)
    assert type(out) is dft.Series
    assert out.name == "x" and out.to_list() == flat.to_list()
    assert str(out.dtype) == str(flat.dtype)
    if row.keywords:
        by_name = getattr(acc, row.name)(**dict(zip(row.keywords, args)))
        assert by_name.to_list() == out.to_list()
        with pytest.raises(TypeError):
            getattr(acc, row.name)(**{"nope": args[0]})


def test_an_accessor_row_the_column_does_not_fit_raises_its_error():
    with pytest.raises(dft.DFTUtilsValueError, match="list_len: not a List column"):
        dft.Series([1, 2]).list.len()
    with pytest.raises(dft.DFTUtilsValueError, match="list_get: not a List column"):
        dft.Series([1, 2]).list.get(0)
    with pytest.raises(dft.DFTUtilsValueError, match="str_count: not a String column"):
        dft.Series([1, 2]).str.count("a")
    with pytest.raises(TypeError):
        dft.Series(["a"]).str.zfill("x")
    with pytest.raises(TypeError):
        dft.Series(["a"]).str.startswith(1)


def test_a_row_the_column_does_not_fit_raises_its_error():
    with pytest.raises(dft.DFTUtilsValueError, match="list_len: not a List column"):
        ext.Series.list_len(dft.Series([1, 2]))
    with pytest.raises(dft.DFTUtilsValueError, match="null column"):
        ext.Series.abs(dft.Series(["a"]))


def test_scalar_rows_take_their_keywords_and_refuse_others():
    s = _float_series()
    assert s.fill_null(value=0.0).to_list() == s.fill_null(0.0).to_list()
    assert ext.Series.clip(s, lower=0, upper=1).to_list() == ext.Series.clip(s, 0, 1).to_list()
    with pytest.raises(TypeError):
        ext.Series.clip(s, lower=0, upper=1, extra=2)
    with pytest.raises(TypeError):
        ext.Series.abs(s, 1)
    with pytest.raises(TypeError):
        ext.Series.is_between(s, 0)


def test_expr_methods_come_from_the_rows():
    for row in ROWS:
        if row.on_expr:
            fn = Expr.__dict__[row.name]
            assert fn.__doc__ == row.doc and fn.__name__ == row.name
            node = getattr(dft.col("x"), row.name)()
            assert node.spec.op == row.op


def test_the_expr_rows_evaluate_like_the_series_methods():
    s = dft.Series([1.0, 4.0, 4.0, 9.0], name="x")
    for row in (r for r in ROWS if r.on_expr and r.args == "NONE"):
        if row.name in {"is_unique", "is_duplicated", "is_nan", "is_finite", "is_infinite"}:
            continue
        got = getattr(dft.col("x"), row.name)().apply(dft.DataFrame({"x": s}))
        want = getattr(s, row.name)()
        assert got.to_list() == want.to_list(), row.name


def test_committed_declarations_equal_the_generated_ones():
    run = subprocess.run(
        [sys.executable, str(_ROOT / "scripts" / "gen_pyi.py"), "--check"],
        capture_output=True,
        text=True,
        cwd=_ROOT,
    )
    assert run.returncode == 0, run.stderr


_KINDS = {
    "alias-of",
    "needs-token",
    "python-semantics",
    "expr-node",
    "construction",
    "conversion",
    "display",
}
_WITH_ARGUMENT = {"alias-of", "needs-token", "expr-node"}
_FRAME_CLASSES = {
    "DataFrame": dft.DataFrame,
    "LazyFrame": dft.LazyFrame,
    "GroupBy": GroupBy,
    "LazyGroupBy": LazyGroupBy,
}
_CLASSES = {"Series": dft.Series, "Expr": Expr, **PUBLIC_ACCESSORS, **_FRAME_CLASSES}


def _parse_line(line):
    parts = [p.strip() for p in line.split(" | ")]
    assert len(parts) == 3 and all(parts), f"allowlist line is `name | kind | detail`: {line!r}"
    name, kind, detail = parts
    base, _, arg = kind.partition(":")
    assert base in _KINDS, f"unknown kind in {line!r}"
    assert (base in _WITH_ARGUMENT) == bool(arg), (
        f"only alias-of, needs-token and expr-node take an argument: {line!r}"
    )
    assert (detail == "-") == (base in _WITH_ARGUMENT), (
        f"detail is `-` exactly for the kinds with an argument: {line!r}"
    )
    return name, base, arg, detail


def _allowlist(path=_ALLOWLIST):
    """`Class.name | kind[:argument] | detail` lines, by name."""
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        name, base, arg, detail = _parse_line(line)
        assert name not in out, f"listed twice: {name}"
        out[name] = (base, arg, detail)
    return out


def _generated(cls_name):
    aliases = {a.alias for a in _alias_table().ALIASES if a.cls == cls_name}
    return {
        r.name for r in ROWS if r.cls == cls_name or (cls_name == "Expr" and r.on_expr)
    } | aliases


def _row_tokens():
    return {r.args for r in ROWS} | {r.result for r in ROWS}


def _expr_node_of(method):
    """The node class that Expr.<method> builds, read from its source: the class
    a `return Class(...)` calls, `_ColumnOpNode` for a `return self._op(...)`, else
    None."""
    tree = ast.parse(Path(importlib.import_module("dftracer.utils.columnar").__file__).read_text())
    classes = {n.name for n in tree.body if isinstance(n, ast.ClassDef)}
    expr = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "Expr")
    for fn in expr.body:
        if isinstance(fn, ast.FunctionDef) and fn.name == method:
            for node in ast.walk(fn):
                if isinstance(node, ast.Return) and isinstance(node.value, ast.Call):
                    call = node.value.func
                    if isinstance(call, ast.Name) and call.id in classes:
                        return call.id
                    if isinstance(call, ast.Attribute) and call.attr == "_op":
                        return "_ColumnOpNode"
    return None


def _entry_problem(name, base, arg):
    """Why an alias-of, needs-token or expr-node entry is wrong, or None."""
    cls_name, _, attr = name.partition(".")
    cls = _CLASSES.get(cls_name)
    if cls is None or not hasattr(cls, attr):
        return f"{name}: no such method"
    if base == "alias-of":
        if getattr(cls, attr) is not getattr(cls, arg, object()):
            return f"{name} is not {arg}"
    elif base == "needs-token":
        api = _api_def()
        if arg not in api.ARG_TOKENS | api.RESULT_TOKENS:
            return f"{name}: {arg} is not a token api_def.py knows"
        if arg in _row_tokens():
            return f"{name}: the token {arg} is in api.def; make the method a row"
    elif base == "expr-node":
        if cls_name != "Expr":
            return f"{name}: only Expr methods build nodes"
        if _expr_node_of(attr) != arg:
            return f"{name}: its body builds {_expr_node_of(attr)}, not {arg}"
    return None


def _entries(kind):
    return [(n, arg) for n, (base, arg, _) in _allowlist().items() if base == kind]


@pytest.mark.parametrize("name,target", _entries("alias-of"))
def test_alias_of_is_the_same_attribute(name, target):
    assert _entry_problem(name, "alias-of", target) is None
    cls_name = name.split(".")[0]
    assert f"{cls_name}.{target}" in _allowlist() or target in _generated(cls_name), (
        f"{name}: the target {target} is neither generated nor listed"
    )


@pytest.mark.parametrize("name,token", _entries("needs-token"))
def test_needs_token_names_a_known_token_no_row_uses(name, token):
    assert _entry_problem(name, "needs-token", token) is None


@pytest.mark.parametrize("name,node", _entries("expr-node"))
def test_expr_node_is_the_class_the_method_builds(name, node):
    assert _entry_problem(name, "expr-node", node) is None


def _construction_problem(name):
    """Why a construction entry is not a class or static method, or None."""
    cls_name, _, attr = name.partition(".")
    cls = _CLASSES.get(cls_name)
    if cls is None or not hasattr(cls, attr):
        return f"{name}: no such method"
    if not isinstance(inspect.getattr_static(cls, attr), (classmethod, staticmethod)):
        return f"{name}: a construction builds the object from the class"
    return None


@pytest.mark.parametrize("name", [n for n, _ in _entries("construction")])
def test_construction_is_a_class_or_static_method(name):
    assert _construction_problem(name) is None


@pytest.mark.parametrize("name", ["Series.abs", "DataFrame.head", "Series.nope"])
def test_the_construction_check_fails_on_a_method_that_builds_nothing(name):
    assert _construction_problem(name) is not None


@pytest.mark.parametrize(
    "name,base,arg",
    [
        ("Series.pad", "alias-of", "abs"),
        ("Series.nope", "alias-of", "abs"),
        ("Series.head", "needs-token", "NO_SUCH_TOKEN"),
        ("Series.head", "needs-token", "NONE"),
        ("Series.head", "needs-token", "I64"),
        ("Expr.cum_sum", "expr-node", "Agg"),
        ("Expr.sum", "expr-node", "_ColumnOpNode"),
        ("Series.head", "expr-node", "Agg"),
    ],
)
def test_the_entry_checks_fail_on_a_bad_entry(name, base, arg):
    assert _entry_problem(name, base, arg) is not None


@pytest.mark.parametrize(
    "line",
    [
        "Series.head | made-up | reason",
        "Series.head | needs-token | -",
        "Series.head | python-semantics:X | reason",
        "Series.head | python-semantics | -",
        "Series.head | needs-token:I64_ARG | reason",
        "Series.head | python-semantics",
    ],
)
def test_the_line_parser_fails_on_a_bad_line(line):
    with pytest.raises(AssertionError):
        _parse_line(line)


def test_a_duplicate_line_in_a_copy_of_the_allowlist_is_refused(tmp_path):
    copy = tmp_path / "list.txt"
    copy.write_text(
        "Series.head | needs-token:I64_ARG | -\nSeries.head | needs-token:I64_ARG | -\n",
        encoding="utf-8",
    )
    with pytest.raises(AssertionError, match="listed twice"):
        _allowlist(copy)


@pytest.mark.parametrize("cls_name", list(_CLASSES))
def test_every_public_method_is_generated_or_listed(cls_name):
    listed = {k.split(".", 1)[1] for k in _allowlist() if k.startswith(cls_name + ".")}
    public = _public(_CLASSES[cls_name])
    generated = _generated(cls_name)
    assert not (public - generated - listed), (
        f"{cls_name}: add a row to api.def or a reasoned line to api_allowlist.txt: "
        f"{sorted(public - generated - listed)}"
    )
    assert not (listed - public), f"{cls_name}: listed but gone: {sorted(listed - public)}"
    assert not (listed & generated), (
        f"{cls_name}: listed and generated: {sorted(listed & generated)}"
    )


def test_duplicate_code_does_not_grow():
    """Runs pylint through uvx, pinned. Without uvx this test fails; set
    DFTRACER_UTILS_SKIP_PYLINT=1 (a variable of this test only) to skip it on a
    machine that cannot run uvx."""
    if os.environ.get("DFTRACER_UTILS_SKIP_PYLINT") == "1":
        pytest.skip("DFTRACER_UTILS_SKIP_PYLINT=1")
    assert shutil.which("uvx") is not None, (
        "uvx is needed to run pylint; set DFTRACER_UTILS_SKIP_PYLINT=1 to skip this test"
    )
    run = subprocess.run(
        [
            "uvx",
            f"pylint=={_PYLINT_VERSION}",
            "--disable=all",
            "--enable=duplicate-code",
            "python/",
        ],
        capture_output=True,
        text=True,
        cwd=_ROOT,
    )
    # pylint exits with a bit mask of the message classes it reported; 32 is a
    # usage error, so it did not run.
    assert run.returncode != 32, f"pylint usage error:\n{run.stderr}"
    assert re.search(r"rated at \d+\.\d+/10", run.stdout), (
        f"pylint printed no rating:\n{run.stdout}"
    )
    found = len(re.findall(r"R0801", run.stdout))
    assert found <= _DUPLICATE_CODE_FINDINGS, (
        f"{found} duplicate-code findings, up from {_DUPLICATE_CODE_FINDINGS}:\n{run.stdout}"
    )


_FIXTURE = """\
/* PY_METHOD(Series, in_block_comment, "x", NONE, SAME, KEEP, NO, "", "", "", "doc") */
#ifndef PY_METHOD
#define PY_METHOD(cls, name, op, args, result, name_rule, on_expr, kw0, kw1, err, doc)
#endif

// PY_METHOD(Series, in_line_comment, "x", NONE, SAME, KEEP, NO, "", "", "", "doc")
#if 0
PY_METHOD(Series, in_if_zero, "x", NONE, SAME, KEEP, NO, "", "", "", "doc")
#if 1
PY_METHOD(Series, in_nested_if, "x", NONE, SAME, KEEP, NO, "", "", "", "doc")
#endif
#else
    PY_METHOD(Series, in_else, "dftu.series.else", NONE, SAME, KEEP, NO, "", "", "", "else")
#endif
#if 0
PY_METHOD(Series, in_second_if_zero, "x", NONE, SAME, KEEP, NO, "", "", "", "doc")
#elif 1
PY_METHOD(Series, in_elif, "dftu.series.elif", NONE, SAME, KEEP, NO, "", "", "", "elif")
#endif
    PY_METHOD(Series, indented, "dftu.series.a", SCALAR, BOOL, DROP, YES, "value", "", "boom",
              "has // and /* inside, (parens) and, commas")  // trailing comment
PY_METHOD(Series, plain, "dftu.series.b", NUMBER2, INT, KEEP, NO, "lo", "hi", "",
          "two "
          "pieces") /* trailing block */
#undef PY_METHOD
"""


def test_the_reader_skips_comments_and_inactive_blocks(tmp_path):
    path = tmp_path / "fixture.def"
    path.write_text(_FIXTURE, encoding="utf-8")
    rows = _api_def().rows(path)
    assert [r.name for r in rows] == ["in_else", "in_elif", "indented", "plain"]
    indented = rows[2]
    assert indented.args == "SCALAR" and indented.on_expr and indented.err == "boom"
    assert indented.keywords == ("value",)
    assert indented.doc == "has // and /* inside, (parens) and, commas"
    assert rows[3].doc == "two pieces" and rows[3].keywords == ("lo", "hi")


def test_the_reader_refuses_a_row_of_the_wrong_width(tmp_path):
    path = tmp_path / "bad.def"
    path.write_text('PY_METHOD(Series, short, "op", NONE)\n', encoding="utf-8")
    with pytest.raises(ValueError):
        _api_def().rows(path)
