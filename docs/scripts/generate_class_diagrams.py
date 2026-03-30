#!/usr/bin/env python3
"""Generate Mermaid class hierarchy diagrams from Doxygen XML output.

Parses Doxygen XML to extract class/struct inheritance relationships,
groups them by namespace/component, and generates .mmd files for
inclusion in Sphinx documentation via sphinxcontrib-mermaid.

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
    derived: list[str] = field(default_factory=list)
    members: list[tuple[str, str, str]] = field(default_factory=list)  # (visibility, type, name)
    is_abstract: bool = False
    is_template: bool = False
    template_params: str = ""
    brief: str = ""


def parse_index(xml_dir: Path) -> dict[str, ClassInfo]:
    """Parse Doxygen index.xml to get all class/struct compounds."""
    index_path = xml_dir / "index.xml"
    if not index_path.exists():
        print(f"Error: {index_path} not found. Run doxygen first.", file=sys.stderr)
        sys.exit(1)

    tree = ET.parse(index_path)
    root = tree.getroot()

    classes: dict[str, ClassInfo] = {}
    for compound in root.findall("compound"):
        kind = compound.get("kind")
        if kind not in ("class", "struct"):
            continue
        name_el = compound.find("name")
        refid = compound.get("refid")
        if name_el is None or name_el.text is None or refid is None:
            continue
        name = name_el.text
        classes[name] = ClassInfo(name=name, kind=kind, refid=refid)

    return classes


def enrich_from_xml(classes: dict[str, ClassInfo], xml_dir: Path) -> None:
    """Parse individual class XML files to get inheritance and members."""
    for cls in list(classes.values()):
        xml_path = xml_dir / f"{cls.refid}.xml"
        if not xml_path.exists():
            continue

        tree = ET.parse(xml_path)
        root = tree.getroot()
        cd = root.find("compounddef")
        if cd is None:
            continue

        # Template parameters
        tpl = cd.find("templateparamlist")
        if tpl is not None:
            cls.is_template = True
            params = []
            for param in tpl.findall("param"):
                type_el = param.find("type")
                declname = param.find("declname")
                defval = param.find("defval")
                p = ""
                if type_el is not None and type_el.text:
                    p = type_el.text.strip()
                if declname is not None and declname.text:
                    p += " " + declname.text.strip()
                if defval is not None:
                    dv = "".join(defval.itertext()).strip()
                    p += f" = {dv}"
                params.append(p.strip())
            cls.template_params = ", ".join(params)

        # Brief description
        brief = cd.find("briefdescription")
        if brief is not None:
            cls.brief = "".join(brief.itertext()).strip()

        # Base classes
        for base in cd.findall("basecompoundref"):
            base_name = base.text
            if base_name:
                cls.bases.append(base_name)

        # Derived classes
        for derived in cd.findall("derivedcompoundref"):
            derived_name = derived.text
            if derived_name:
                cls.derived.append(derived_name)

        # Check if abstract (has pure virtual methods)
        for section in cd.findall("sectiondef"):
            for member in section.findall("memberdef"):
                if member.get("virt") == "pure-virtual":
                    cls.is_abstract = True
                prot = member.get("prot", "public")
                if prot == "private":
                    continue
                mkind = member.get("kind")
                if mkind != "function":
                    continue
                mname = member.find("name")
                mtype = member.find("type")
                if mname is not None and mname.text is not None:
                    type_text = ""
                    if mtype is not None:
                        type_text = "".join(mtype.itertext()).strip()
                    vis = "+" if prot == "public" else "#"
                    cls.members.append((vis, type_text, mname.text))


def short_name(fqn: str) -> str:
    """Extract short class name from fully qualified name."""
    base = re.sub(r"<.*>", "", fqn)
    parts = base.split("::")
    return parts[-1] if parts else fqn


def sanitize_mermaid_id(name: str) -> str:
    """Make a name safe for Mermaid node identifiers."""
    return re.sub(r"[^a-zA-Z0-9_]", "_", name)


def escape_mermaid(text: str) -> str:
    """Escape special characters for Mermaid labels."""
    return text.replace('"', "#quot;").replace("<", "&lt;").replace(">", "&gt;")


# Component groupings: (diagram_name, namespace_prefixes, title)
COMPONENT_GROUPS = [
    (
        "coro",
        ["dftracer::utils::coro::"],
        "Coroutine Primitives",
    ),
    (
        "pipeline_tasks",
        [
            "dftracer::utils::Task",
            "dftracer::utils::TaskResult",
            "dftracer::utils::CoroScope",
            "dftracer::utils::NoOpTask",
        ],
        "Task System",
    ),
    (
        "pipeline_executor",
        [
            "dftracer::utils::Executor",
            "dftracer::utils::Scheduler",
            "dftracer::utils::Watchdog",
            "dftracer::utils::Pipeline",
            "dftracer::utils::PipelineError",
            "dftracer::utils::ExecutorConfig",
            "dftracer::utils::TaskInfo",
            "dftracer::utils::TaskProgress",
            "dftracer::utils::ExecutorProgress",
            "dftracer::utils::TimerService",
            "dftracer::utils::ShardedMutex",
        ],
        "Executor and Runtime",
    ),
    (
        "utilities",
        ["dftracer::utils::utilities::Utility"],
        "Utility Base",
    ),
    (
        "reader",
        ["dftracer::utils::utilities::reader::"],
        "Reader Components",
    ),
    (
        "indexer",
        ["dftracer::utils::utilities::indexer::"],
        "Indexer Components",
    ),
    (
        "io",
        ["dftracer::utils::io::"],
        "I/O Backends",
    ),
    (
        "sqlite",
        ["dftracer::utils::sqlite::"],
        "SQLite Components",
    ),
    (
        "dft_aggregators",
        ["dftracer::utils::utilities::composites::dft::aggregators::"],
        "DFTracer Aggregation Pipeline",
    ),
    (
        "dft_indexing",
        ["dftracer::utils::utilities::composites::dft::indexing::"],
        "DFTracer Indexing System",
    ),
    (
        "dft_composites",
        [
            "dftracer::utils::utilities::composites::dft::",
            "dftracer::utils::utilities::composites::Chunk",
            "dftracer::utils::utilities::composites::Batch",
            "dftracer::utils::utilities::composites::Directory",
            "dftracer::utils::utilities::composites::Metadata",
        ],
        "DFTracer Composites",
    ),
    (
        "compression",
        ["dftracer::utils::utilities::compression::"],
        "Compression Utilities",
    ),
    (
        "fileio",
        ["dftracer::utils::utilities::fileio::"],
        "File I/O Utilities",
    ),
    (
        "text",
        ["dftracer::utils::utilities::text::"],
        "Text Processing Utilities",
    ),
    (
        "call_tree",
        ["dftracer::utils::call_tree::"],
        "Call Tree Analysis",
    ),
    (
        "statistics",
        [
            "dftracer::utils::utilities::common::statistics::",
            "dftracer::utils::utilities::composites::dft::statistics::",
        ],
        "Statistics Utilities",
    ),
    (
        "server",
        ["dftracer::utils::server::"],
        "Server Components",
    ),
]


# Suffixes that indicate internal implementation machinery.
_INTERNAL_SUFFIXES = (
    "Awaitable",
    "Awaiter",
    "WaiterNode",
    "State",
    "SharedState",
)

# Short names that are always internal implementation details.
_INTERNAL_NAMES = frozenset(
    {
        "promise_type",
        "FinalAwaiter",
        "Awaiter",
        "Adopt",
        "iterator",
        "sentinel",
        "PromiseBase",
    }
)


def _is_internal_nested(cls: ClassInfo, group_prefixes: list[str]) -> bool:
    """Return True if a class is an internal detail that should be hidden.

    Filters out:
    - Classes from std:: or other external namespaces
    - Deeply nested types (1+ levels below a class in the group)
    - Known internal names (promise_type, FinalAwaiter, etc.)
    - Type traits / metaprogramming helpers (no public methods)
    - Internal awaitable/state machinery
    """
    name = cls.name
    sname = short_name(name)

    # External namespace classes (std::, boost::, etc.)
    if name.startswith("std::") or name.startswith("boost::"):
        return True

    # Known internal names
    if sname in _INTERNAL_NAMES:
        return True

    # Type traits: typically have no public methods and are template-only
    # Heuristic: all-lowercase short name with underscores = type trait
    if re.match(r"^[a-z_]+$", sname) and not cls.members:
        return True

    # Check nesting depth relative to group prefix.
    # Namespace segments like "internal", "detail", "impl" don't count
    # as class nesting — they're organizational namespaces.
    _NS_SEGMENTS = {"internal", "detail", "impl", "types", "sources"}
    for prefix in group_prefixes:
        clean_prefix = prefix.rstrip("::")
        if name.startswith(clean_prefix + "::"):
            remainder = name[len(clean_prefix) + 2 :]
            # Strip organizational namespace segments
            parts = remainder.split("::")
            class_parts = [p for p in parts if p not in _NS_SEGMENTS]
            # class_parts like ["Reader"] = depth 0 -> keep
            # class_parts like ["Channel", "ProducerGuard"] = depth 1 -> SKIP
            depth = len(class_parts) - 1
            if depth >= 1:
                return True

    # Internal suffixes for top-level classes in the namespace
    # e.g. WhenAllVectorState, WhenAnySharedState
    if any(sname.endswith(s) for s in _INTERNAL_SUFFIXES):
        return True

    return False


def classify_class(name: str) -> list[str]:
    """Return which component groups a class belongs to."""
    groups = []
    for group_name, prefixes, _ in COMPONENT_GROUPS:
        for prefix in prefixes:
            if name.startswith(prefix) or name == prefix.rstrip("::"):
                groups.append(group_name)
                break
    return groups


def collect_group_classes(classes: dict[str, ClassInfo], group_name: str) -> list[ClassInfo]:
    """Collect non-internal classes belonging to a component group."""
    # Find the prefixes for this group
    prefixes: list[str] = []
    for gn, gp, _ in COMPONENT_GROUPS:
        if gn == group_name:
            prefixes = gp
            break

    result = []
    for cls in classes.values():
        if group_name in classify_class(cls.name):
            if not _is_internal_nested(cls, prefixes):
                result.append(cls)
    return result


def find_utility_subclasses(classes: dict[str, ClassInfo]) -> list[ClassInfo]:
    """Find all classes that inherit from Utility<...>."""
    result = []
    for cls in classes.values():
        for base in cls.bases:
            if "Utility<" in base or "Utility &lt;" in base:
                result.append(cls)
                break
    return result


def generate_mermaid_classdiagram(
    group_classes: list[ClassInfo],
    all_classes: dict[str, ClassInfo],
    include_external_bases: bool = True,
    max_members: int = 3,
) -> str:
    """Generate Mermaid classDiagram for a component group."""
    lines = ["classDiagram"]

    group_names = {cls.name for cls in group_classes}
    external_refs: set[str] = set()

    for cls in sorted(group_classes, key=lambda c: c.name):
        sname = short_name(cls.name)
        mid = sanitize_mermaid_id(cls.name)

        # Class definition with members
        shown = 0
        member_lines = []
        for vis, mtype, mname in cls.members:
            if shown >= max_members:
                break
            if mname.startswith("~") or mname.startswith("operator"):
                continue
            # Skip constructors (same name as class)
            if mname == short_name(cls.name):
                continue
            ret = short_name(mtype) if mtype else "void"
            if len(ret) > 20:
                ret = ret[:17] + "..."
            member_lines.append(f"    {mid} : {vis}{mname}() {ret}")
            shown += 1

        lines.append(f'    class {mid}["{sname}"]')
        if cls.is_abstract:
            lines.append(f"    <<abstract>> {mid}")
        lines.extend(member_lines)

        # Track external bases
        if include_external_bases:
            for base in cls.bases:
                base_clean = re.sub(r"<.*>", "", base).strip()
                if base_clean not in group_names:
                    external_refs.add(base_clean)

    # External base classes (skip std:: and boost:: internals)
    external_refs = {
        e for e in external_refs if not e.startswith("std::") and not e.startswith("boost::")
    }
    for ext in sorted(external_refs):
        sname = short_name(ext)
        mid = sanitize_mermaid_id(ext)
        lines.append(f'    class {mid}["{sname}"]')
        lines.append(f"    style {mid} fill:#e0e0e0,stroke:#999")

    # Inheritance relationships
    for cls in sorted(group_classes, key=lambda c: c.name):
        mid = sanitize_mermaid_id(cls.name)
        for base in cls.bases:
            base_clean = re.sub(r"<.*>", "", base).strip()
            base_mid = sanitize_mermaid_id(base_clean)
            if base_clean in group_names or (
                include_external_bases and base_clean in external_refs
            ):
                lines.append(f"    {base_mid} <|-- {mid}")

    return "\n".join(lines)


def _classify_utility_group(cls: ClassInfo) -> str:
    """Classify a Utility subclass into a display group."""
    ns = cls.name.rsplit("::", 1)[0] if "::" in cls.name else ""
    if "aggregators" in ns:
        return "Aggregators"
    if "indexing" in ns:
        return "Indexing"
    if "statistics" in ns:
        return "Statistics"
    if "views" in ns or "View" in cls.name:
        return "Views"
    if "composites" in ns:
        return "Composites"
    if "compression" in ns:
        return "Compression"
    if "fileio" in ns:
        return "File I/O"
    if "text" in ns:
        return "Text"
    if "reader" in ns:
        return "Reader"
    if "indexer" in ns:
        return "Indexer"
    if "filesystem" in ns:
        return "Filesystem"
    if "call_tree" in ns:
        return "Call Tree"
    if "hash" in ns:
        return "Hash"
    if "replay" in ns:
        return "Replay"
    return "Other"


def generate_utility_hierarchy_mermaid(
    classes: dict[str, ClassInfo],
) -> str:
    """Generate a Mermaid graph TD showing Utility subclasses by category.

    Uses a flowchart (graph TD) instead of classDiagram because
    50+ classes inheriting from one base is unreadable as a class diagram.
    Each category becomes a single node listing its classes.
    """
    utility_subs = find_utility_subclasses(classes)

    # Group by category
    groups: dict[str, list[ClassInfo]] = {}
    for cls in utility_subs:
        group = _classify_utility_group(cls)
        groups.setdefault(group, []).append(cls)

    lines = ["graph TD"]

    # Base Utility node
    lines.append(
        '    Utility["<b>Utility&lt;I, O, Tags...&gt;</b>'
        '<br/><i>process() → CoroTask&lt;O&gt;</i>"]'
    )

    # Tags note
    lines.append(
        '    Tags["<b>Tags</b><br/>NeedsContext<br/>Parallelizable<br/>Cacheable<br/>Retryable"]'
    )
    lines.append("    Utility -.- Tags")

    # Category nodes
    for group_label in sorted(groups.keys()):
        members = groups[group_label]
        mid = sanitize_mermaid_id(group_label)
        class_names = sorted(short_name(c.name) for c in members)
        # Build multi-line label: bold title + class names
        label_parts = [f"<b>{group_label}</b>"]
        for name in class_names:
            label_parts.append(name)
        label = "<br/>".join(label_parts)
        lines.append(f'    {mid}["{label}"]')

    # Arrows from Utility to each category
    for group_label in sorted(groups.keys()):
        mid = sanitize_mermaid_id(group_label)
        lines.append(f"    Utility --> {mid}")

    # Styling
    lines.append("    style Utility fill:#4a90d9,stroke:#2c5f8a,color:#fff")
    lines.append("    style Tags fill:#f5f5f5,stroke:#ccc,color:#666")
    for group_label in sorted(groups.keys()):
        mid = sanitize_mermaid_id(group_label)
        lines.append(f"    style {mid} fill:#e8f4e8,stroke:#5a9,color:#333")

    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--xml-dir",
        type=Path,
        default=Path("docs/doxygen/xml"),
        help="Doxygen XML output directory",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("docs/source/_generated"),
        help="Output directory for .mmd files",
    )
    args = parser.parse_args()

    xml_dir = args.xml_dir
    output_dir = args.output_dir

    print(f"Parsing Doxygen XML from {xml_dir}...")
    classes = parse_index(xml_dir)
    print(f"  Found {len(classes)} classes/structs")

    print("Enriching with inheritance and member info...")
    enrich_from_xml(classes, xml_dir)

    inheritance_count = sum(1 for c in classes.values() if c.bases)
    print(f"  {inheritance_count} classes have base classes")

    output_dir.mkdir(parents=True, exist_ok=True)

    # Generate per-component diagrams (classDiagram)
    generated = 0
    for group_name, _, title in COMPONENT_GROUPS:
        group_classes = collect_group_classes(classes, group_name)
        if not group_classes:
            continue

        mmd = generate_mermaid_classdiagram(group_classes, classes)
        out_path = output_dir / f"{group_name}.mmd"
        out_path.write_text(mmd)
        print(f"  {out_path}: {len(group_classes)} classes")
        generated += 1

    # Generate the Utility hierarchy overview (graph TD flowchart)
    mmd = generate_utility_hierarchy_mermaid(classes)
    out_path = output_dir / "utility_hierarchy.mmd"
    out_path.write_text(mmd)
    print(f"  {out_path}: Utility hierarchy overview")
    generated += 1

    print(f"Generated {generated} Mermaid diagrams in {output_dir}")


if __name__ == "__main__":
    main()
