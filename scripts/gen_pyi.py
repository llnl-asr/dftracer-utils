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
import sys
from pathlib import Path
from typing import Callable, Dict, List

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
}


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


TARGETS: Dict[str, Callable[[List[api_def.Row]], List[str]]] = {
    "dftracer_utils_ext.pyi": series_lines,
    "columnar.py": expr_lines,
}


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
    for name, make in TARGETS.items():
        path = PKG / name
        old = path.read_text(encoding="utf-8")
        new = render(old, make(rows))
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
