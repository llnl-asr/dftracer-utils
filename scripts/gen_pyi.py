#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the generated parts of the type declarations from api.def.

Two regions, each between `# BEGIN GENERATED` and `# END GENERATED` lines:

* the Series methods of python/dftracer/utils/dftracer_utils_ext.pyi, one per
  PY_METHOD(Series, ...) row, typed from its `args` and `result` tokens;
* the Expr methods declared under `if TYPE_CHECKING:` in
  python/dftracer/utils/columnar.py, one per row with on_expr YES.

Run it from the repository root after editing src/dftracer/utils/python/core/
api.def:

    python scripts/gen_pyi.py          # rewrite the regions
    python scripts/gen_pyi.py --check  # exit 1 when a region is stale

tests/python/test_api_table.py runs the check.
"""

import argparse
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

RESULT_TYPES: Dict[str, str] = {
    "SAME": "Self",
    "BOOL": '"PublicSeries[bool]"',
    "INT": '"PublicSeries[int]"',
    "FLOAT": '"PublicSeries[float]"',
    "STR": '"PublicSeries[str]"',
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


def series_lines(rows: List[api_def.Row]) -> List[str]:
    out = []
    for r in rows:
        if r.cls != "Series":
            continue
        if r.args == "NONE":
            params = ""
        elif r.args == "SCALAR":
            params = f", {r.keywords[0]}: Union[bool, int, float, str]"
        elif r.args == "NUMBER2":
            params = "".join(f", {k}: Union[int, float]" for k in r.keywords)
        else:
            raise ValueError(f"{r.name}: unknown args token {r.args}")
        if r.result == "SCALAR":
            for self_type, result in SCALAR_OVERLOADS:
                out.append("    @overload")
                out.append(f"    def {r.name}({self_type}{params}) -> {result}: ...")
        elif r.result in VALUE_RESULTS:
            out.append(f"    def {r.name}(self{params}) -> {VALUE_RESULTS[r.result][0]}: ...")
        else:
            out.append(f"    def {r.name}(self{params}) -> {RESULT_TYPES[r.result]}: ...")
    return out


def expr_lines(rows: List[api_def.Row]) -> List[str]:
    out = []
    for r in rows:
        if r.on_expr:
            if r.args != "NONE":
                raise ValueError(f"{r.name}: Expr takes only no-argument rows")
            out.append(f'        def {r.name}(self) -> "Expr": ...')
    return out


TARGETS: Dict[str, Tuple[Callable, Callable, str]] = {
    "dftracer_utils_ext.pyi": (series_lines, lambda r: r.cls == "Series", "class Series("),
    "columnar.py": (expr_lines, lambda r: r.on_expr, "class Expr:"),
}


def hand_duplicates(text: str, names: List[str], scope: str) -> List[str]:
    """Names of generated methods that the class starting at the line `scope`
    also declares by hand outside its generated region."""
    src = text.split("\n")
    first = next(i for i, line in enumerate(src) if line.startswith(scope))
    last = next(
        (i for i in range(first + 1, len(src)) if src[i] and not src[i][0].isspace()), len(src)
    )
    begin = next(i for i in range(first, last) if src[i].strip() == BEGIN)
    end = next(i for i in range(begin, last) if src[i].strip() == END)
    outside = "\n".join(src[first:begin] + src[end:last])
    return [n for n in names if re.search(rf"^\s+def {n}\(", outside, re.M)]


def render(text: str, lines: List[str]) -> str:
    src = text.split("\n")
    begin = next(i for i, line in enumerate(src) if line.strip() == BEGIN)
    end = next(i for i in range(begin, len(src)) if src[i].strip() == END)
    return "\n".join(src[: begin + 1] + lines + src[end:])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    rows = api_def.rows()
    stale = []
    for name, (make, select, scope) in TARGETS.items():
        path = PKG / name
        old = path.read_text(encoding="utf-8")
        new = render(old, make(rows))
        clash = hand_duplicates(old, [r.name for r in rows if select(r)], scope)
        if clash:
            print(f"{name}: declared by hand and generated: {clash}", file=sys.stderr)
            return 1
        if new != old:
            stale.append(name)
            if not args.check:
                path.write_text(new, encoding="utf-8")
    if args.check and stale:
        print("stale: " + ", ".join(stale) + " (run python scripts/gen_pyi.py)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
