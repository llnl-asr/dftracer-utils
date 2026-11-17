# SPDX-License-Identifier: MIT
"""The Python API table, src/dftracer/utils/python/core/api.def: the native
module, the Expr methods and the type declarations all come from it, and the
public methods of Series and Expr that it does not give are listed with a
reason in api_allowlist.txt."""

import importlib.util
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import dftracer_utils_ext as ext  # noqa: E402
from dftracer.utils.columnar import Expr  # noqa: E402

_ROOT = Path(__file__).resolve().parents[2]
_ALLOWLIST = Path(__file__).parent / "api_allowlist.txt"
# Duplicate-code findings of `pylint --enable=duplicate-code python/` today. It
# only goes down.
_DUPLICATE_CODE_FINDINGS = 19


def _api_def():
    spec = importlib.util.spec_from_file_location("api_def", _ROOT / "scripts" / "api_def.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["api_def"] = module
    spec.loader.exec_module(module)
    return module


ROWS = _api_def().rows()
SERIES_ROWS = [r for r in ROWS if r.cls == "Series"]


def _public(cls):
    return {n for n in dir(cls) if not n.startswith("_")}


def test_native_rows_equal_the_rows_of_the_file():
    native = [(*r[:6], r[6], tuple(r[7]), r[8]) for r in ext._api_rows()]
    assert native == [tuple(r) for r in ROWS]


@pytest.mark.parametrize("row", SERIES_ROWS, ids=[r.name for r in SERIES_ROWS])
def test_series_row_is_a_native_method_with_the_row_doc(row):
    method = getattr(ext.Series, row.name)
    assert method.__doc__ == row.doc
    assert row.name in ext.Series.__dict__


def _float_series():
    return dft.Series([1.5, -2.0, None, 4.0, -0.5])


_REJECTS_FLOAT = {"is_unique", "is_duplicated"}


@pytest.mark.parametrize("row", SERIES_ROWS, ids=[r.name for r in SERIES_ROWS])
def test_series_row_runs_the_op_and_keeps_the_name(row):
    s = _float_series()
    s.name = "x"
    method = getattr(ext.Series, row.name)
    if row.args == "NONE":
        out = method(s)
    elif row.args == "SCALAR":
        out = method(s, **{row.keywords[0]: 0.0})
    else:
        out = method(s, **dict(zip(row.keywords, (-1, 2))))
    assert type(out) is dft.Series
    if row.name not in {"unique", "drop_nulls"}:
        assert out.name == "x" or row.name == "cum_count"


def test_scalar_rows_take_their_keywords_and_refuse_others():
    s = _float_series()
    assert s.fillna(value=0.0).to_pylist() == s.fillna(0.0).to_pylist()
    assert ext.Series.clip(s, lo=0, hi=1).to_pylist() == ext.Series.clip(s, 0, 1).to_pylist()
    with pytest.raises(TypeError):
        ext.Series.clip(s, lo=0, hi=1, extra=2)
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
        assert got.to_pylist() == want.to_pylist(), row.name


def test_committed_declarations_equal_the_generated_ones():
    run = subprocess.run(
        [sys.executable, str(_ROOT / "scripts" / "gen_pyi.py"), "--check"],
        capture_output=True,
        text=True,
        cwd=_ROOT,
    )
    assert run.returncode == 0, run.stderr


def _allowlist():
    out = {}
    for line in _ALLOWLIST.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        name, sep, reason = line.partition(" | ")
        assert sep and reason.strip(), f"allowlist line needs a reason: {line!r}"
        assert name not in out, f"listed twice: {name}"
        out[name] = reason
    return out


def _generated(cls_name):
    return {r.name for r in ROWS if r.cls == cls_name or (cls_name == "Expr" and r.on_expr)}


@pytest.mark.parametrize("cls_name,cls", [("Series", dft.Series), ("Expr", Expr)])
def test_every_public_method_is_generated_or_listed(cls_name, cls):
    listed = {k.split(".", 1)[1] for k in _allowlist() if k.startswith(cls_name + ".")}
    public = _public(cls)
    generated = _generated(cls_name)
    assert not (public - generated - listed), (
        f"{cls_name}: add a row to api.def or a reasoned line to api_allowlist.txt: "
        f"{sorted(public - generated - listed)}"
    )
    assert not (listed - public), f"{cls_name}: listed but gone: {sorted(listed - public)}"
    assert not (listed & generated), (
        f"{cls_name}: listed and generated: {sorted(listed & generated)}"
    )


@pytest.mark.skipif(shutil.which("uvx") is None, reason="uvx not available to run pylint")
def test_duplicate_code_does_not_grow():
    run = subprocess.run(
        ["uvx", "pylint", "--disable=all", "--enable=duplicate-code", "python/"],
        capture_output=True,
        text=True,
        cwd=_ROOT,
    )
    found = len(re.findall(r"R0801", run.stdout))
    assert found <= _DUPLICATE_CODE_FINDINGS, (
        f"{found} duplicate-code findings, up from {_DUPLICATE_CODE_FINDINGS}:\n{run.stdout}"
    )
