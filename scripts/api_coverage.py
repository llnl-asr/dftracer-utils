#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks the public names of pandas and Polars against ours.

For each upstream class (pandas: DataFrame, Series, DataFrameGroupBy,
SeriesGroupBy and the .str / .dt accessors; Polars: DataFrame, Series,
LazyFrame, Expr, GroupBy, LazyGroupBy and the str / dt / list namespaces) it
lists the public names of the installed library, compares them with the names
of our class (the alias tables install the other spellings as real attributes,
so an alias counts), and subtracts the checked-in exclusion list
scripts/api_coverage_exclude.txt. It exits non-zero on

  * a name we neither have nor exclude,
  * an exclusion line that names something we now have (a stale line),
  * a malformed exclusion line or an unknown reason kind.

An excluded name that the installed library does not have (another version) is
not an error. A library that is not installed is skipped, and the script says
so.

    scripts/api_coverage.py            # check, both libraries
    scripts/api_coverage.py --report   # the coverage table (and the check)
    scripts/api_coverage.py --lib pandas

The exclusion file has one line per name:

    lib Class.name | reason-kind | detail

Reason kinds: out-of-scope (plotting, style, sparse, foreign I/O, sql, torch
and jax, remote, plan serialization), python-semantics (the name has no
meaning for a typed columnar engine), needs-index (waits for the row index
step), needs-kernel (an engine kernel is missing; the detail names it),
deprecated-upstream (deprecated or removed upstream).
"""

from __future__ import annotations

import argparse
import importlib
import re
import sys
from pathlib import Path
from typing import Callable, Dict, List, Optional, Set, Tuple

EXCLUDE = Path(__file__).resolve().parent / "api_coverage_exclude.txt"
KINDS = (
    "out-of-scope",
    "python-semantics",
    "needs-index",
    "needs-kernel",
    "deprecated-upstream",
)


def _public(obj: object) -> Set[str]:
    return {n for n in dir(obj) if not n.startswith("_")}


def _pandas_classes() -> Dict[str, Callable[[], object]]:
    import pandas as pd

    return {
        "DataFrame": lambda: pd.DataFrame,
        "Series": lambda: pd.Series,
        "DataFrameGroupBy": lambda: pd.DataFrame({"a": [1]}).groupby("a"),
        "SeriesGroupBy": lambda: pd.Series([1]).groupby([1]),
        "Series.str": lambda: pd.Series(["a"]).str,
        "Series.dt": lambda: pd.Series(pd.to_datetime(["2020-01-01"])).dt,
    }


def _polars_classes() -> Dict[str, Callable[[], object]]:
    import datetime

    import polars as pl

    day = datetime.datetime(2020, 1, 1)
    return {
        "DataFrame": lambda: pl.DataFrame,
        "Series": lambda: pl.Series,
        "LazyFrame": lambda: pl.LazyFrame,
        "Expr": lambda: pl.Expr,
        "GroupBy": lambda: pl.DataFrame({"a": [1]}).group_by("a"),
        "LazyGroupBy": lambda: pl.LazyFrame({"a": [1]}).group_by("a"),
        "Series.str": lambda: pl.Series(["a"]).str,
        "Series.dt": lambda: pl.Series([day]).dt,
        "Series.list": lambda: pl.Series([[1]]).list,
        "Expr.str": lambda: pl.col("a").str,
        "Expr.dt": lambda: pl.col("a").dt,
        "Expr.list": lambda: pl.col("a").list,
    }


def _expr_instance() -> object:
    """An Expr resolves the unary math names (``abs``, ``sqrt``, ...) in
    ``__getattr__``, so ``dir`` of the class does not show them."""
    import dftracer.utils as dft

    return dft.col("x")


def _ours(lib: str) -> Dict[str, Optional[object]]:
    import dftracer.utils as dft

    expr = importlib.import_module("dftracer.utils.expr")
    lazyframe = importlib.import_module("dftracer.utils.lazyframe")
    series = importlib.import_module("dftracer.utils.series")

    s_str, s_dt, s_list = series._StrAccessor, series._DtAccessor, series._ListAccessor
    common = {
        "DataFrame": dft.DataFrame,
        "Series": dft.Series,
        "Series.str": s_str,
        "Series.dt": s_dt,
    }
    if lib == "pandas":
        return {
            **common,
            "DataFrameGroupBy": dft.GroupBy,
            "SeriesGroupBy": dft.GroupBy,
        }
    return {
        **common,
        "LazyFrame": dft.LazyFrame,
        "Expr": dft.Expr,
        "GroupBy": dft.GroupBy,
        "LazyGroupBy": lazyframe.LazyGroupBy,
        "Series.list": s_list,
        "Expr.str": expr._ExprStr,
        "Expr.dt": expr._ExprDt,
        "Expr.list": getattr(expr, "_ExprList", None),
    }


LIBS: Dict[str, Callable[[], Dict[str, Callable[[], object]]]] = {
    "pandas": _pandas_classes,
    "polars": _polars_classes,
}


def load_exclusions(path: Path) -> Tuple[Dict[Tuple[str, str, str], Tuple[str, str]], List[str]]:
    rows: Dict[Tuple[str, str, str], Tuple[str, str]] = {}
    errors: List[str] = []
    for no, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split("|", 2)]
        if len(parts) != 3 or " " not in parts[0] or "." not in parts[0]:
            errors.append(f"{path.name}:{no}: want 'lib Class.name | kind | detail': {raw}")
            continue
        lib, ref = parts[0].split(" ", 1)
        cls, name = ref.rsplit(".", 1)
        kind, detail = parts[1], parts[2]
        if kind not in KINDS:
            errors.append(f"{path.name}:{no}: unknown reason kind '{kind}'")
        elif not detail:
            errors.append(f"{path.name}:{no}: empty detail")
        elif lib not in LIBS:
            errors.append(f"{path.name}:{no}: unknown library '{lib}'")
        elif (lib, cls, name) in rows:
            errors.append(f"{path.name}:{no}: duplicate {lib} {cls}.{name}")
        else:
            rows[(lib, cls, name)] = (kind, detail)
    return rows, errors


class Row:
    def __init__(self, lib: str, cls: str) -> None:
        self.lib, self.cls = lib, cls
        self.upstream: Set[str] = set()
        self.have: Set[str] = set()
        self.excluded: Dict[str, str] = {}
        self.missing: Set[str] = set()


def measure(libs: List[str]) -> Tuple[List[Row], List[str], List[str]]:
    exclusions, errors = load_exclusions(EXCLUDE)
    rows: List[Row] = []
    skipped: List[str] = []
    for lib in libs:
        try:
            importlib.import_module(lib)
        except ImportError:
            skipped.append(lib)
            continue
        ours = _ours(lib)
        for cls, make in LIBS[lib]().items():
            row = Row(lib, cls)
            row.upstream = _public(make())
            mine = ours.get(cls)
            mine_names = _public(mine) if mine is not None else set()
            if cls == "Expr":
                inst = _expr_instance()
                mine_names |= {n for n in row.upstream if hasattr(inst, n)}
            row.have = row.upstream & mine_names
            for name in sorted(row.upstream - mine_names):
                entry = exclusions.get((lib, cls, name))
                if entry is None:
                    row.missing.add(name)
                else:
                    row.excluded[name] = entry[0]
            for (xlib, xcls, name), (kind, detail) in exclusions.items():
                if xlib != lib or xcls != cls:
                    continue
                if name in mine_names:
                    errors.append(f"stale exclusion: {lib} {cls}.{name} ({kind}) is now ours")
                if kind == "needs-kernel":
                    # A missing kernel is not a method we have under another
                    # name: a detail that sends the reader to one ("use x",
                    # "x is the form", "x exists") is an implementation to do.
                    for t in sorted(mine_names - {name}):
                        pointer = (
                            rf"\buse (?:\w+, )*{t}\b"
                            rf"|\b{t}\b(?:\([^)]*\))? (?:\w+ )?(?:is|are|exists?)\b"
                            rf"|\bform[s]? (?:is|are) (?:\w+ )*{t}\b"
                            rf"|\b{t}\b and \w+ are the forms?"
                        )
                        if len(t) > 3 and re.search(pointer, detail):
                            errors.append(
                                f"needs-kernel {lib} {cls}.{name}: the detail points to our "
                                f"existing {t}; implement it as forwarding or composition, "
                                "or say what kernel is missing"
                            )
                            break
            rows.append(row)
    known = {(r.lib, r.cls) for r in rows}
    for lib, cls, name in exclusions:
        if lib in libs and lib not in skipped and (lib, cls) not in known:
            errors.append(f"exclusion for an unknown class: {lib} {cls}.{name}")
    return rows, errors, skipped


def report(rows: List[Row]) -> str:
    out = []
    for lib in LIBS:
        mine = [r for r in rows if r.lib == lib]
        if not mine:
            continue
        mod = importlib.import_module(lib)
        out.append(f"{lib} {mod.__version__}")
        head = ["class", "upstream", "ours"] + list(KINDS) + ["missing"]
        table = [head]
        for r in mine:
            counts = [sum(1 for k in r.excluded.values() if k == kind) for kind in KINDS]
            table.append(
                [r.cls, str(len(r.upstream)), str(len(r.have))]
                + [str(c) for c in counts]
                + [str(len(r.missing))]
            )
        widths = [max(len(row[i]) for row in table) for i in range(len(head))]
        for i, row in enumerate(table):
            out.append("  ".join(c.ljust(widths[j]) for j, c in enumerate(row)).rstrip())
            if i == 0:
                out.append("  ".join("-" * w for w in widths))
        out.append("")
    return "\n".join(out)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lib", choices=list(LIBS), help="check one library only")
    ap.add_argument("--report", action="store_true", help="print the coverage table")
    ap.add_argument("--list-missing", action="store_true", help="list each missing name")
    args = ap.parse_args(argv)
    rows, errors, skipped = measure([args.lib] if args.lib else list(LIBS))
    for lib in skipped:
        print(f"{lib} is not installed: skipped", file=sys.stderr)
    if args.report:
        print(report(rows))
    for r in rows:
        if r.missing:
            errors.append(
                f"{r.lib} {r.cls}: {len(r.missing)} name(s) neither ours nor excluded"
                + (
                    ": " + " ".join(sorted(r.missing))
                    if args.list_missing or not args.report
                    else ""
                )
            )
    for e in errors:
        print(e, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
