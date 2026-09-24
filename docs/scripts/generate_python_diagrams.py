#!/usr/bin/env python3
"""Generate Mermaid collaboration diagrams for the Python API reference.

Parses the package's Python sources with ast and, for each api page, draws the
documented classes with their inheritance (``<|--``) and association (``o--``)
edges. Associations come from type annotations - method return/parameter types
and annotated attributes that name another package class. Classes from another
module (or a private wrapper base) appear as greyed leaves, mirroring the C++
collaboration diagrams.

Usage:
    python generate_python_diagrams.py [--pkg-dir DIR] [--output-dir DIR]
"""

from __future__ import annotations

import argparse
import ast
import re
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class PyClass:
    name: str
    module: str
    bases: list[str] = field(default_factory=list)
    assoc: list[str] = field(default_factory=list)  # referenced class short-names
    methods: list[str] = field(default_factory=list)
    is_enum: bool = False


# api page -> the class short-names it documents (from the .rst autodoc targets).
PAGE_CLASSES = {
    "columnar": ["Expr", "ColumnExpr", "Columnar", "Agg", "GroupBy"],
    "dataframe": ["DataFrame"],
    "series": ["Series"],
    "trace_viewer": ["TraceViewer", "Session", "Handle", "Containment"],
    "query": ["Field", "Expr"],
    "runtime": ["Runtime", "TaskHandle"],
    "indexer": ["Indexer", "AggregationConfig", "IndexStatus", "CheckpointIndexer"],
    "reader": ["JsonDictValue"],
    "dfanalyzer": ["DFAnalyzerAggregatedTraceViewer", "HLMConfig"],
}

# Which module file backs each page's classes.
PAGE_MODULE = {
    "columnar": "columnar",
    "dataframe": "dataframe",
    "series": "series",
    "trace_viewer": "trace_viewer",
    "query": "query",
    "runtime": "runtime",
    "indexer": "indexer",
    "reader": "reader",
    "dfanalyzer": "dfanalyzer",
}

_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_TYPING_NOISE = frozenset(
    {
        "Optional", "Union", "List", "Dict", "Tuple", "Sequence", "Any", "Iterable",
        "Iterator", "Mapping", "Set", "Callable", "Literal", "Type", "None", "bool",
        "int", "float", "str", "bytes", "object", "self",
    }
)


def _names_in_annotation(node: ast.expr | None) -> list[str]:
    if node is None:
        return []
    try:
        text = ast.unparse(node)
    except Exception:
        return []
    return [t for t in _IDENT.findall(text) if t not in _TYPING_NOISE]


def parse_module(path: Path, module: str) -> dict[str, PyClass]:
    classes: dict[str, PyClass] = {}
    tree = ast.parse(path.read_text())
    for node in tree.body:
        if not isinstance(node, ast.ClassDef):
            continue
        pc = PyClass(name=node.name, module=module)
        for base in node.bases:
            bname = ast.unparse(base)
            pc.bases.append(bname.split("[")[0].split(".")[-1].strip("\"'"))
        pc.is_enum = any(b in ("Enum", "IntEnum", "StrEnum") for b in pc.bases)
        assoc: set[str] = set()
        for item in ast.walk(node):
            if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)):
                for a in _names_in_annotation(item.returns):
                    assoc.add(a)
                for arg in list(item.args.args) + list(item.args.kwonlyargs):
                    for a in _names_in_annotation(arg.annotation):
                        assoc.add(a)
                if not item.name.startswith("_"):
                    pc.methods.append(item.name)
            elif isinstance(item, ast.AnnAssign):
                for a in _names_in_annotation(item.annotation):
                    assoc.add(a)
        pc.assoc = sorted(assoc - {node.name})
        classes[node.name] = pc
    return classes


def sanitize(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def build_diagram(
    page: str, registry: dict[str, PyClass], max_members: int = 2
) -> str | None:
    module = PAGE_MODULE[page]
    documented = [c for c in PAGE_CLASSES[page] if c in registry and registry[c].module == module]
    if not documented:
        return None
    in_group = set(documented)

    inherit: set[tuple[str, str]] = set()
    assoc: set[tuple[str, str]] = set()
    external: set[str] = set()
    for cname in documented:
        pc = registry[cname]
        for base in pc.bases:
            if base in registry:
                inherit.add((base, cname))
                if base not in in_group:
                    external.add(base)
        for target in pc.assoc:
            if target in registry and target != cname:
                assoc.add((cname, target))
                if target not in in_group:
                    external.add(target)

    endpoints = {n for e in inherit for n in e} | {n for e in assoc for n in e}
    if not (inherit or assoc):
        return None

    lines = ["classDiagram"]
    for name in sorted(endpoints):
        mid = sanitize(name)
        lines.append(f'    class {mid}["{name}"]')
        if name in external:
            continue
        pc = registry[name]
        for m in pc.methods[:max_members]:
            lines.append(f"    {mid} : +{m}()")

    for base, derived in sorted(inherit):
        lines.append(f"    {sanitize(base)} <|-- {sanitize(derived)}")
    for owner, held in sorted(assoc):
        lines.append(f"    {sanitize(owner)} o-- {sanitize(held)}")
    for ext in sorted(external):
        lines.append(f"    style {sanitize(ext)} fill:#1a2436,stroke:#556,color:#8fa")

    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pkg-dir", type=Path, default=Path("python/dftracer/utils"))
    parser.add_argument("--output-dir", type=Path, default=Path("docs/source/_generated"))
    args = parser.parse_args()

    registry: dict[str, PyClass] = {}
    for py in sorted(args.pkg_dir.glob("*.py")):
        module = py.stem
        try:
            for name, pc in parse_module(py, module).items():
                registry.setdefault(name, pc)
        except SyntaxError:
            continue
    print(f"Parsed {len(registry)} Python classes from {args.pkg_dir}")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    produced: list[str] = []
    for page in PAGE_CLASSES:
        mmd = build_diagram(page, registry)
        out = args.output_dir / f"py_{page}.mmd"
        if mmd is None:
            if out.exists():
                out.unlink()
            continue
        out.write_text(mmd)
        produced.append(page)
        print(f"  py_{page}.mmd: {mmd.count('<|--') + mmd.count('o--')} edges")

    print(f"Python diagrams: {', '.join(produced) if produced else '(none)'}")


if __name__ == "__main__":
    main()
