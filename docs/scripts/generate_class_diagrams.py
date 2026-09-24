#!/usr/bin/env python3
"""Generate Mermaid diagrams for the C++ API reference from Doxygen XML.

Two kinds of diagram are produced:

- ``architecture.mmd``: the fixed layered-library dependency graph plus the
  AsyncOp compose model. This reflects the build structure, not any single
  class, so it is authored here rather than derived from XML.
- One collaboration diagram per component group: the group's classes with
  their inheritance (``<|--``) and composition (``o--``) edges. The codebase
  is composition-based (the old Utility inheritance tree is gone), so these
  are driven by member-variable types, not by an inheritance hierarchy. Only
  classes that participate in at least one edge are drawn; a group with no
  edges produces no file. main() prints the groups that got a diagram so the
  .rst pages can be wired accordingly.

Usage:
    python generate_class_diagrams.py [--xml-dir DIR] [--output-dir DIR]

Defaults:
    --xml-dir    docs/doxygen/xml
    --output-dir docs/source/_generated
"""

from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class ClassInfo:
    name: str  # fully qualified name
    kind: str  # "class" or "struct"
    refid: str
    bases: list[str] = field(default_factory=list)
    composes: list[str] = field(default_factory=list)  # fqns held as members
    members: list[tuple[str, str, str]] = field(default_factory=list)  # (vis, type, name)
    is_abstract: bool = False
    is_template: bool = False
    brief: str = ""


def parse_index(xml_dir: Path) -> tuple[dict[str, ClassInfo], dict[str, str]]:
    """Parse Doxygen index.xml. Returns (classes-by-fqn, fqn-by-refid)."""
    index_path = xml_dir / "index.xml"
    if not index_path.exists():
        print(f"Error: {index_path} not found. Run doxygen first.", file=sys.stderr)
        sys.exit(1)

    root = ET.parse(index_path).getroot()
    classes: dict[str, ClassInfo] = {}
    by_refid: dict[str, str] = {}
    for compound in root.findall("compound"):
        if compound.get("kind") not in ("class", "struct"):
            continue
        name_el = compound.find("name")
        refid = compound.get("refid")
        if name_el is None or name_el.text is None or refid is None:
            continue
        classes[name_el.text] = ClassInfo(name=name_el.text, kind=compound.get("kind"), refid=refid)
        by_refid[refid] = name_el.text
    return classes, by_refid


def enrich_from_xml(classes: dict[str, ClassInfo], by_refid: dict[str, str], xml_dir: Path) -> None:
    """Add inheritance, composition (via member-type refs), and public methods."""
    for cls in list(classes.values()):
        xml_path = xml_dir / f"{cls.refid}.xml"
        if not xml_path.exists():
            continue
        cd = ET.parse(xml_path).getroot().find("compounddef")
        if cd is None:
            continue

        if cd.find("templateparamlist") is not None:
            cls.is_template = True

        brief = cd.find("briefdescription")
        if brief is not None:
            cls.brief = "".join(brief.itertext()).strip()

        for base in cd.findall("basecompoundref"):
            if base.text:
                cls.bases.append(base.text)

        seen_composes: set[str] = set()
        for section in cd.findall("sectiondef"):
            for member in section.findall("memberdef"):
                if member.get("virt") == "pure-virtual":
                    cls.is_abstract = True
                mkind = member.get("kind")
                prot = member.get("prot", "public")

                # Composition: any member variable whose type names a known
                # class (Doxygen emits a <ref> for it). Private members count -
                # composition is often held privately.
                if mkind == "variable":
                    type_el = member.find("type")
                    if type_el is not None:
                        for ref in type_el.findall("ref"):
                            target = by_refid.get(ref.get("refid", ""))
                            if target and target != cls.name and target not in seen_composes:
                                seen_composes.add(target)
                                cls.composes.append(target)
                    continue

                if mkind != "function" or prot == "private":
                    continue
                mname = member.find("name")
                if mname is None or mname.text is None:
                    continue
                mtype = member.find("type")
                type_text = "".join(mtype.itertext()).strip() if mtype is not None else ""
                cls.members.append(("+" if prot == "public" else "#", type_text, mname.text))


def short_name(fqn: str) -> str:
    return re.sub(r"<.*>", "", fqn).split("::")[-1]


def sanitize_mermaid_id(name: str) -> str:
    return re.sub(r"[^a-zA-Z0-9_]", "_", re.sub(r"<.*>", "", name))


# Component groups, keyed to the cpp_api page that includes them.
# (diagram_name, [namespace prefixes or exact class names], title)
COMPONENT_GROUPS = [
    ("coro", ["dftracer::utils::coro::"], "Coroutine Primitives"),
    (
        "runtime",
        [
            "dftracer::utils::Runtime",
            "dftracer::utils::Executor",
            "dftracer::utils::ExecutorConfig",
            "dftracer::utils::ExecutorProgress",
            "dftracer::utils::TaskExecutor",
            "dftracer::utils::Task",
            "dftracer::utils::TypedTask",
            "dftracer::utils::TaskHandle",
            "dftracer::utils::TypedTaskHandle",
            "dftracer::utils::TaskResult",
            "dftracer::utils::TaskInfo",
            "dftracer::utils::TaskProgress",
            "dftracer::utils::NoOpTask",
            "dftracer::utils::CoroScope",
            "dftracer::utils::Pipeline",
            "dftracer::utils::PipelineConfig",
            "dftracer::utils::PipelineOutput",
            "dftracer::utils::PipelineError",
            "dftracer::utils::TimerService",
            "dftracer::utils::ShardedMutex",
            "dftracer::utils::ObjectPool",
            "dftracer::utils::BufferPool",
            "dftracer::utils::StringIntern",
            "dftracer::utils::StringArena",
        ],
        "Executor and Runtime",
    ),
    ("task_graph", ["dftracer::utils::task_graph::"], "Task Graph"),
    ("io", ["dftracer::utils::io::"], "I/O Backends"),
    ("query", ["dftracer::utils::query::"], "Query DSL"),
    (
        "dataframe",
        ["dftracer::utils::dataframe::"],
        "Columnar Engine",
    ),
    ("plugins", ["dftracer::utils::plugins::"], "Plugins"),
    (
        "trace",
        ["dftracer::utils::trace::"],
        "Trace Domain",
    ),
    ("reader", ["dftracer::utils::utilities::reader::"], "Trace Reader"),
    ("index", ["dftracer::utils::index::"], "Index"),
    ("arrow", ["dftracer::utils::utilities::common::arrow::"], "Arrow Bridge"),
    (
        "utilities",
        [
            "dftracer::utils::utilities::fileio",
            "dftracer::utils::utilities::filesystem::",
            "dftracer::utils::utilities::hash::",
            "dftracer::utils::utilities::text::",
            "dftracer::utils::utilities::replay::",
            "dftracer::utils::utilities::dlio::",
            "dftracer::utils::utilities::common::statistics::",
        ],
        "Utilities",
    ),
    ("server", ["dftracer::utils::server::"], "HTTP Server"),
]


_INTERNAL_SUFFIXES = ("Awaitable", "Awaiter", "WaiterNode", "State", "SharedState", "Impl")
_INTERNAL_NAMES = frozenset(
    {"promise_type", "FinalAwaiter", "Awaiter", "Adopt", "iterator", "sentinel", "PromiseBase"}
)
_NS_SEGMENTS = {
    "internal", "detail", "impl", "types", "sources", "reflect",
    "store", "cf", "queries", "gzip", "extensions", "profiles", "dft", "agg",
    "build", "plan", "cache",
}


def _is_internal(cls: ClassInfo, prefixes: list[str]) -> bool:
    """True if a class is machinery that should not appear in a diagram."""
    name = cls.name
    sname = short_name(name)

    if name.startswith("std::") or name.startswith("boost::"):
        return True
    if sname in _INTERNAL_NAMES:
        return True
    # Type traits: all-lowercase, no public methods.
    if re.match(r"^[a-z_]+$", sname) and not cls.members:
        return True
    for prefix in prefixes:
        clean = prefix.rstrip("::")
        if name.startswith(clean + "::"):
            parts = name[len(clean) + 2 :].split("::")
            class_parts = [p for p in parts if p not in _NS_SEGMENTS]
            if len(class_parts) - 1 >= 1:  # nested one level below a class
                return True
    if any(sname.endswith(s) for s in _INTERNAL_SUFFIXES):
        return True
    return False


def in_group(name: str, prefixes: list[str]) -> bool:
    for prefix in prefixes:
        if name.startswith(prefix) or name == prefix.rstrip("::"):
            return True
    return False


def collect_group(classes: dict[str, ClassInfo], prefixes: list[str]) -> list[ClassInfo]:
    return [
        c
        for c in classes.values()
        if in_group(c.name, prefixes) and not _is_internal(c, prefixes)
    ]


def _displayable_external(name: str, classes: dict[str, ClassInfo]) -> bool:
    """True if an out-of-group class is a real type worth showing as a leaf."""
    cls = classes.get(name)
    if cls is None:
        return False
    sname = short_name(name)
    if name.startswith("std::") or name.startswith("boost::"):
        return False
    if sname in _INTERNAL_NAMES:
        return False
    if re.match(r"^[a-z_]+$", sname) and not cls.members:  # type trait
        return False
    if any(sname.endswith(s) for s in _INTERNAL_SUFFIXES):
        return False
    return True


# Cap external (cross-group) nodes so a diagram stays readable; above this the
# group's downward dependencies are too broad to draw usefully and are dropped.
_EXTERNAL_CAP = 10


def generate_collaboration(
    candidates: list[ClassInfo], classes: dict[str, ClassInfo], max_members: int = 2
) -> str | None:
    """classDiagram of a group's inheritance + composition edges.

    Edges to types outside the group are kept and the target drawn as a greyed
    leaf, so cross-layer relationships (e.g. TaskGraph holding a core Task) show.
    Only classes touched by an edge are drawn; returns None when the group has
    no relationships worth a diagram, or when its external fan-out is too broad.
    """
    shown = {c.name for c in candidates}
    by_name = {c.name: c for c in candidates}

    inherit: list[tuple[str, str]] = []  # (base, derived)
    compose: list[tuple[str, str]] = []  # (owner, held)
    external: set[str] = set()
    for c in candidates:
        for base in c.bases:
            base_clean = re.sub(r"<.*>", "", base).strip()
            if base_clean in shown:
                inherit.append((base_clean, c.name))
            elif _displayable_external(base_clean, classes):
                inherit.append((base_clean, c.name))
                external.add(base_clean)
        for target in c.composes:
            if target in shown:
                compose.append((c.name, target))
            elif _displayable_external(target, classes):
                compose.append((c.name, target))
                external.add(target)

    if len(external) > _EXTERNAL_CAP:
        inherit = [(b, d) for b, d in inherit if b in shown]
        compose = [(o, h) for o, h in compose if h in shown]
        external = set()

    endpoints = {n for e in inherit for n in e} | {n for e in compose for n in e}
    if not endpoints:
        return None

    # Template specializations (Foo, Foo< void >) collapse to one node id; keep
    # a single representative per id, preferring the one with the most members.
    reps: dict[str, str] = {}
    for name in endpoints:
        mid = sanitize_mermaid_id(name)
        cur = reps.get(mid)
        if cur is None or len(by_name.get(name, ClassInfo("", "", "")).members) > len(
            by_name.get(cur, ClassInfo("", "", "")).members
        ):
            reps[mid] = name

    lines = ["classDiagram"]
    for name in sorted(reps.values()):
        mid = sanitize_mermaid_id(name)
        lines.append(f'    class {mid}["{short_name(name)}"]')
        if name in external:
            continue
        cls = by_name[name]
        if cls.is_abstract:
            lines.append(f"    <<abstract>> {mid}")
        count = 0
        seen_methods: set[str] = set()
        for vis, mtype, mname in cls.members:
            if count >= max_members:
                break
            if mname.startswith("~") or mname.startswith("operator") or mname == short_name(name):
                continue
            if mname in seen_methods:  # collapse overloads
                continue
            seen_methods.add(mname)
            ret = short_name(mtype) if mtype else "void"
            if len(ret) > 20:
                ret = ret[:17] + "..."
            lines.append(f"    {mid} : {vis}{mname}() {ret}")
            count += 1

    # Dedup by rendered id pairs: template specializations collapse to one edge.
    inherit_edges = sorted({(sanitize_mermaid_id(b), sanitize_mermaid_id(d)) for b, d in inherit})
    compose_edges = sorted({(sanitize_mermaid_id(o), sanitize_mermaid_id(h)) for o, h in compose})
    for base, derived in inherit_edges:
        lines.append(f"    {base} <|-- {derived}")
    for owner, held in compose_edges:
        lines.append(f"    {owner} o-- {held}")

    for ext in sorted(external):
        lines.append(f"    style {sanitize_mermaid_id(ext)} fill:#1a2436,stroke:#556,color:#8fa")

    return "\n".join(lines) + "\n"


# Bottom-to-top: core is the foundation at the base; each arrow points from a
# library to the one built on top of it (A --> B reads "B is built on A").
ARCHITECTURE_MMD = """graph BT
    utilities["dftracer_utils_utilities<br/><i>readers, index, aggregation,<br/>comparison, statistics, plugins, dlio, replay</i>"]
    dataframe["dftracer_utils_dataframe<br/><i>Series/DataFrame, Highway SIMD kernels,<br/>Arrow bridge, columnar query execution</i>"]
    query["dftracer_utils_query<br/><i>predicate IR, string codec, evaluator</i>"]
    json["dftracer_utils_json<br/><i>simdjson-backed parsing</i>"]
    core["dftracer_utils_core<br/><i>coroutines, tasks, task graph, io backend,<br/>pipelines, primitives</i>"]

    core --> json
    json --> query
    query --> dataframe
    dataframe --> utilities

    compose["AsyncOp compose model<br/><i>| sequence &nbsp; &amp;&amp; both &nbsp; || either &nbsp; .or_else</i>"]
    core -.- compose

    style core fill:#4a90d9,stroke:#2c5f8a,color:#fff
    style dataframe fill:#e8f4e8,stroke:#5a9,color:#333
    style utilities fill:#e8f4e8,stroke:#5a9,color:#333
    style query fill:#f0f0f0,stroke:#999,color:#333
    style json fill:#f0f0f0,stroke:#999,color:#333
    style compose fill:#fff6e0,stroke:#d9a94a,color:#333
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xml-dir", type=Path, default=Path("docs/doxygen/xml"))
    parser.add_argument("--output-dir", type=Path, default=Path("docs/source/_generated"))
    args = parser.parse_args()

    print(f"Parsing Doxygen XML from {args.xml_dir}...")
    classes, by_refid = parse_index(args.xml_dir)
    print(f"  Found {len(classes)} classes/structs")
    enrich_from_xml(classes, by_refid, args.xml_dir)

    args.output_dir.mkdir(parents=True, exist_ok=True)

    (args.output_dir / "architecture.mmd").write_text(ARCHITECTURE_MMD)
    print("  architecture.mmd: layered library graph")

    produced: list[str] = []
    for group_name, prefixes, _ in COMPONENT_GROUPS:
        candidates = collect_group(classes, prefixes)
        mmd = generate_collaboration(candidates, classes)
        out_path = args.output_dir / f"{group_name}.mmd"
        if mmd is None:
            if out_path.exists():
                out_path.unlink()
            continue
        out_path.write_text(mmd)
        produced.append(group_name)
        print(f"  {group_name}.mmd: {mmd.count('<|--') + mmd.count('o--')} edges")

    print(f"Collaboration diagrams: {', '.join(produced) if produced else '(none)'}")


if __name__ == "__main__":
    main()
