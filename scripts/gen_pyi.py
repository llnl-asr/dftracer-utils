#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the generated parts of the type declarations from api.def.

Each region lies between `# BEGIN GENERATED` and `# END GENERATED` lines:

* the methods of the classes Series, StrAccessor, DtAccessor and ListAccessor
  in python/dftracer/utils/dftracer_utils_ext.pyi, one per PY_METHOD row of the
  class Series, Str, Dt or List, typed from its `args` and `result` tokens;
* the Expr methods declared under `if TYPE_CHECKING:` in
  python/dftracer/utils/columnar.py, one per row with on_expr YES;
* the aliases of python/dftracer/utils/_aliases.py in the class bodies of
  Series, Expr, DataFrame and LazyFrame, between
  `# BEGIN GENERATED aliases` and `# END GENERATED aliases`, so a type checker
  sees what `_aliases.install` sets at import.

Run it from the repository root after editing src/dftracer/utils/python/core/
api.def:

    python scripts/gen_pyi.py          # rewrite the regions
    python scripts/gen_pyi.py --check  # exit 1 when a region is stale

tests/python/test_api_table.py runs the check.
"""

import argparse
import ast
import importlib.util
import re
import sys
from pathlib import Path
from typing import Callable, Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import api_def  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
PKG = ROOT / "python" / "dftracer" / "utils"
BEGIN = "# BEGIN GENERATED from api.def by scripts/gen_pyi.py"
END = "# END GENERATED"
ALIAS_BEGIN = "# BEGIN GENERATED aliases from _aliases.py by scripts/gen_pyi.py"
ALIAS_END = "# END GENERATED aliases"

RESULT_TYPES: Dict[str, str] = {
    "SAME": "Self",
    "BOOL": '"PublicSeries[bool]"',
    "INT": '"PublicSeries[int]"',
    "FLOAT": '"PublicSeries[float]"',
    "STR": '"PublicSeries[str]"',
    "ANY": '"PublicSeries[Scalar]"',
}


VALUE_RESULTS: Dict[str, List[str]] = {
    "I64": ["int"],
    "F64": ["float"],
    "ELEMENT": ["Optional[T_co]"],
}
# SCALAR: a number of the column's own type, int or float, else either.
SCALAR_OVERLOADS = (
    ('self: "Series[int]"', "int"),
    ('self: "Series[float]"', "float"),
    ('self: "Series[Scalar]"', "Union[int, float]"),
)


def class_lines(cls: str) -> Callable[[List[api_def.Row]], List[str]]:
    def make(rows: List[api_def.Row]) -> List[str]:
        return series_lines([r for r in rows if r.cls == cls])

    return make


def series_lines(rows: List[api_def.Row]) -> List[str]:
    out = []
    for r in rows:
        if r.args == "NONE":
            params = ""
        elif r.args == "SCALAR":
            params = f", {r.keywords[0]}: Union[bool, int, float, str]"
        elif r.args == "NUMBER2":
            params = "".join(f", {k}: Union[int, float]" for k in r.keywords)
        elif r.args in ("STR", "STR2"):
            params = "".join(f", {k}: str" for k in r.keywords)
        elif r.args == "INT":
            params = f", {r.keywords[0]}: int"
        else:
            raise ValueError(f"{r.name}: unknown args token {r.args}")
        if r.result == "SCALAR":
            for self_type, result in SCALAR_OVERLOADS:
                out.append("    @overload")
                out.append(f"    def {r.name}({self_type}{params}) -> {result}: ...")
        elif r.result in VALUE_RESULTS:
            out.append(f"    def {r.name}(self{params}) -> {VALUE_RESULTS[r.result][0]}: ...")
        else:
            out += wrapped(r.name, f"self{params}", RESULT_TYPES[r.result])
    return out


LINE_LENGTH = 100


def wrapped(name: str, params: str, result: str) -> List[str]:
    """One `def` line, split as ruff format splits a line over the limit."""
    line = f"    def {name}({params}) -> {result}: ..."
    if len(line) <= LINE_LENGTH:
        return [line]
    inner = f"        {params}"
    if len(inner) <= LINE_LENGTH:
        return [f"    def {name}(", inner, f"    ) -> {result}: ..."]
    pieces = [p.strip() for p in params.split(", ")]
    return [f"    def {name}(", *[f"        {p}," for p in pieces], f"    ) -> {result}: ..."]


def alias_module():
    spec = importlib.util.spec_from_file_location("_aliases", PKG / "_aliases.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["_aliases"] = module
    spec.loader.exec_module(module)
    return module


def _defines(cls: ast.ClassDef, name: str) -> bool:
    """Whether the class body defines ``name`` as a def, an assignment or, under
    an `if TYPE_CHECKING:`, a declaration."""
    for node in ast.walk(ast.Module(body=cls.body, type_ignores=[])):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == name:
            return True
        if isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id == name for t in node.targets
        ):
            return True
    return False


def _owner(cls_name: str, file: str, target: str) -> str:
    """How a class body names ``target``: bare when the body defines it, else
    through the first base class that does (a class of the package, or the
    native base `_ext.<Class>` whose declarations the .pyi gives)."""
    trees = {f.name: ast.parse(f.read_text(encoding="utf-8")) for f in PKG.glob("*.py")}
    classes = {
        n.name: n for tree in trees.values() for n in tree.body if isinstance(n, ast.ClassDef)
    }
    cls = classes[cls_name]
    if _defines(cls, target):
        return target
    for base in cls.bases:
        expr = base.value if isinstance(base, ast.Subscript) else base
        if isinstance(expr, ast.Name) and expr.id in classes and _defines(classes[expr.id], target):
            return f"{expr.id}.{target}"
    native = ast.unparse(
        cls.bases[-1].value if isinstance(cls.bases[-1], ast.Subscript) else cls.bases[-1]
    )
    return f"{native}.{target}"


def alias_lines(cls: str, file: str) -> Callable[[List[api_def.Row]], List[str]]:
    def make(_rows: List[api_def.Row]) -> List[str]:
        aliases = [a for a in alias_module().ALIASES if a.cls == cls and not a.adapter]
        lines = ["    if TYPE_CHECKING:"]
        lines += [f"        {a.alias} = {_owner(cls, file, a.target)}" for a in aliases]
        lines.append("        pass")
        return lines

    return make


def expr_lines(rows: List[api_def.Row]) -> List[str]:
    out = []
    for r in rows:
        if r.on_expr:
            if r.args != "NONE":
                raise ValueError(f"{r.name}: Expr takes only no-argument rows")
            out.append(f'        def {r.name}(self) -> "Expr": ...')
    return out


TARGETS: List[Tuple[str, Callable, Callable, str, str, str]] = [
    (
        "dftracer_utils_ext.pyi",
        class_lines("Series"),
        lambda r: r.cls == "Series",
        "class Series(",
        BEGIN,
        END,
    ),
    (
        "dftracer_utils_ext.pyi",
        class_lines("Str"),
        lambda r: r.cls == "Str",
        "class StrAccessor:",
        BEGIN,
        END,
    ),
    (
        "dftracer_utils_ext.pyi",
        class_lines("Dt"),
        lambda r: r.cls == "Dt",
        "class DtAccessor:",
        BEGIN,
        END,
    ),
    (
        "dftracer_utils_ext.pyi",
        class_lines("List"),
        lambda r: r.cls == "List",
        "class ListAccessor:",
        BEGIN,
        END,
    ),
    ("columnar.py", expr_lines, lambda r: r.on_expr, "class Expr:", BEGIN, END),
    (
        "series.py",
        alias_lines("Series", "series.py"),
        lambda r: False,
        "class Series(",
        ALIAS_BEGIN,
        ALIAS_END,
    ),
    (
        "columnar.py",
        alias_lines("Expr", "columnar.py"),
        lambda r: False,
        "class Expr:",
        ALIAS_BEGIN,
        ALIAS_END,
    ),
    (
        "dataframe.py",
        alias_lines("DataFrame", "dataframe.py"),
        lambda r: False,
        "class DataFrame(",
        ALIAS_BEGIN,
        ALIAS_END,
    ),
    (
        "lazyframe.py",
        alias_lines("LazyFrame", "lazyframe.py"),
        lambda r: False,
        "class LazyFrame(",
        ALIAS_BEGIN,
        ALIAS_END,
    ),
]


def hand_duplicates(
    text: str, names: List[str], scope: str, begin_mark: str = BEGIN, end_mark: str = END
) -> List[str]:
    """Names of generated methods that the class starting at the line `scope`
    also declares by hand outside its generated region."""
    src = text.split("\n")
    first = next(i for i, line in enumerate(src) if line.startswith(scope))
    last = next(
        (i for i in range(first + 1, len(src)) if src[i] and not src[i][0].isspace()), len(src)
    )
    begin = next(i for i in range(first, last) if src[i].strip() == begin_mark)
    end = next(i for i in range(begin, last) if src[i].strip() == end_mark)
    outside = "\n".join(src[first:begin] + src[end:last])
    return [n for n in names if re.search(rf"^\s+def {n}\(", outside, re.M)]


def render(text: str, lines: List[str], scope: str, begin_mark: str, end_mark: str) -> str:
    src = text.split("\n")
    first = next(i for i, line in enumerate(src) if line.startswith(scope))
    begin = next(i for i in range(first, len(src)) if src[i].strip() == begin_mark)
    end = next(i for i in range(begin, len(src)) if src[i].strip() == end_mark)
    return "\n".join(src[: begin + 1] + lines + src[end:])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    rows = api_def.rows()
    stale = []
    texts = {name: (PKG / name).read_text(encoding="utf-8") for name, *_ in TARGETS}
    for name, make, select, scope, begin_mark, end_mark in TARGETS:
        old = texts[name]
        new = render(old, make(rows), scope, begin_mark, end_mark)
        clash = hand_duplicates(
            old, [r.name for r in rows if select(r)], scope, begin_mark, end_mark
        )
        if clash:
            print(f"{name}: declared by hand and generated: {clash}", file=sys.stderr)
            return 1
        if new != old:
            if name not in stale:
                stale.append(name)
            texts[name] = new
    if not args.check:
        for name in stale:
            (PKG / name).write_text(texts[name], encoding="utf-8")
    if args.check and stale:
        print("stale: " + ", ".join(stale) + " (run python scripts/gen_pyi.py)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
