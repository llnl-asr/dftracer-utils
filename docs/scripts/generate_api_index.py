#!/usr/bin/env python3
"""Generate comprehensive C++ API reference pages from Doxygen XML output.

Auto-discovers all public classes/structs from Doxygen XML, groups them
by namespace into module pages, and generates per-module RST files with
Breathe doxygen directives.

Modules are discovered automatically from the namespace hierarchy - no
hardcoded module list needed. The namespace tree is split at a
configurable depth to produce reasonably-sized pages.

This script is called automatically by conf.py before Sphinx builds.

Usage:
    python generate_api_index.py [--xml-dir DIR] [--output-dir DIR]

Defaults:
    --xml-dir      docs/doxygen/xml
    --output-dir   docs/source/cpp_api/api
"""

from __future__ import annotations

import argparse
import os
import subprocess
import xml.etree.ElementTree as ET
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

# Root namespace prefix - everything under this is considered public API
ROOT_NS = "dftracer::utils"

# Skip these namespace segments
SKIP_SEGMENTS = {"detail", "internal", "impl"}

# Skip items from these file paths
SKIP_PATHS = ["/cpmsource/", "/third_party/", "/test/", "/tests/"]

# Only include items from files matching this
INCLUDE_ROOT = "dftracer/utils"

# Map namespace prefixes to related guide pages (for cross-references)
GUIDE_PAGES: dict[str, str] = {
    "coro": "coro",
    "io": "io",
    "rocksdb": "rocksdb",
    "task_graph": "task_graph",
    "utilities.common.arrow": "arrow",
    "utilities.indexer": "indexer",
    "utilities.reader": "reader",
    "utilities.composites.dft.aggregators": "dft_aggregators",
    "utilities.composites.dft.indexing": "dft_indexing",
}

# Human-readable title overrides (key = namespace suffix after ROOT_NS)
TITLE_OVERRIDES: dict[str, str] = {
    "coro": "Coroutine Primitives",
    "io": "Async I/O",
    "rocksdb": "RocksDB",
    "task_graph": "Task Graph",
    "server": "HTTP Server",
    "call_tree": "Call Tree",
    "mpi": "MPI Utilities",
    "utilities.common.statistics": "Statistics (DDSketch, Histogram)",
    "utilities.common.arrow": "Arrow Data Interchange",
    "utilities.common.json": "JSON Utilities",
    "utilities.common.query": "Query DSL",
    "utilities.compression.zlib": "Compression (Zlib)",
    "utilities.fileio": "File I/O",
    "utilities.filesystem": "Filesystem",
    "utilities.hash": "Hash Utilities",
    "utilities.text": "Text Processing",
    "utilities.indexer": "Indexer",
    "utilities.reader": "Reader",
    "utilities.replay": "Replay",
    "utilities.composites.dft.aggregators": "DFTracer Aggregation Pipeline",
    "utilities.composites.dft.statistics": "DFTracer Statistics",
    "utilities.composites.dft.indexing": "DFTracer Bloom Filter Indexing",
    "utilities.composites.dft.views": "DFTracer Views & Predicates",
    "utilities.composites.dft.comparator": "DFTracer Comparator",
    "utilities.composites.dft.reorganize": "DFTracer Reorganization",
    "utilities.composites.dft": "DFTracer Composites (General)",
    "utilities.composites": "Generic Composites",
}


def is_inner_type(name: str, all_names: set[str]) -> bool:
    """Check if a name is an inner/nested type of another class.

    A type is "inner" if removing its last segment produces a name
    that also exists in the API (i.e. its parent is a known class/struct).
    """
    parts = name.rsplit("::", 1)
    if len(parts) < 2:
        return False
    parent = parts[0]
    return parent in all_names


@dataclass
class APIItem:
    name: str
    kind: str  # class, struct, function
    refid: str
    file: str = ""
    brief: str = ""
    is_inner: bool = False
    line: int | None = None
    bodyfile: str = ""
    bodystart: int | None = None
    bodyend: int | None = None
    # Free-function fields (kind == "function")
    ns_suffix: str | None = None  # enclosing namespace suffix, dotted (e.g. "io")
    arg_types: str = ""  # "(type1, type2)" for overload disambiguation
    overloaded: bool = False  # the qualified name has more than one overload


@dataclass
class Module:
    """A discovered module (namespace group)."""

    ns_suffix: str  # namespace suffix after ROOT_NS (e.g. "coro", "utilities.fileio")
    full_ns: str  # full namespace
    title: str
    filename: str  # output path relative to output_dir (e.g. "coroutines" or "composites/dft_aggregators")
    guide_page: str | None
    items: list[APIItem] = field(default_factory=list)


def parse_doxygen_xml(xml_dir: Path) -> list[APIItem]:
    """Parse Doxygen index.xml to get all public API items."""
    index_path = xml_dir / "index.xml"
    if not index_path.exists():
        raise FileNotFoundError(
            f"{index_path} not found. Run doxygen first:\n  cd docs && doxygen Doxyfile"
        )

    tree = ET.parse(index_path)
    root = tree.getroot()

    items: list[APIItem] = []
    seen: set[str] = set()

    for compound in root.findall("compound"):
        kind = compound.get("kind")
        if kind not in ("class", "struct"):
            continue

        name = compound.findtext("name", "")
        refid = compound.get("refid", "")

        if not name.startswith(ROOT_NS):
            continue

        # Skip internal segments
        parts = name.split("::")
        if any(p in SKIP_SEGMENTS for p in parts):
            continue

        if name in seen:
            continue
        seen.add(name)

        file_path = ""
        brief = ""
        line = None
        bodyfile = ""
        bodystart = None
        bodyend = None
        detail_xml = xml_dir / f"{refid}.xml"
        if detail_xml.exists():
            try:
                dtree = ET.parse(detail_xml)
                droot = dtree.getroot()
                # Use the compound's OWN location (direct child of
                # compounddef). ".//location" would return the first member's
                # location, pointing source links inside the class.
                loc = droot.find("compounddef/location")
                if loc is not None:
                    file_path = loc.get("file", "")
                    bodyfile = loc.get("bodyfile", "")
                    line_attr = loc.get("line")
                    bodystart_attr = loc.get("bodystart")
                    bodyend_attr = loc.get("bodyend")
                    line = int(line_attr) if line_attr and line_attr.isdigit() else None
                    bodystart = (
                        int(bodystart_attr)
                        if bodystart_attr and bodystart_attr.isdigit()
                        else None
                    )
                    bodyend = (
                        int(bodyend_attr)
                        if bodyend_attr and bodyend_attr.isdigit()
                        else None
                    )
                bd = droot.find(".//briefdescription/para")
                if bd is not None and bd.text:
                    brief = bd.text.strip()
            except ET.ParseError:
                pass

        if file_path:
            if INCLUDE_ROOT not in file_path:
                continue
            if any(skip in file_path for skip in SKIP_PATHS):
                continue

        items.append(
            APIItem(
                name=name,
                kind=kind,
                refid=refid,
                file=file_path,
                brief=brief,
                line=line,
                bodyfile=bodyfile,
                bodystart=bodystart,
                bodyend=bodyend,
            )
        )

    # Mark inner types (second pass, needs full name set)
    all_names = {i.name for i in items}
    for item in items:
        item.is_inner = is_inner_type(item.name, all_names)

    # Filter out template specializations (e.g. CoroTask< void >) since
    # they share member IDs with the primary template and cause duplicates.
    # Keep them only if there's no primary template in the set.
    primary_names = {i.name for i in items if "<" not in i.name}
    filtered = []
    for item in items:
        if "<" in item.name:
            # Check if primary template exists (name before first <)
            base = item.name.split("<")[0].rstrip()
            if base in primary_names:
                continue  # skip specialization
        filtered.append(item)
    items = filtered

    return items


def _ns_suffix_from_full(ns_full: str) -> str:
    """Namespace suffix after ROOT_NS, dotted. ``dftracer::utils::io`` -> ``io``."""
    if ns_full == ROOT_NS:
        return ""
    suffix = ns_full[len(ROOT_NS) :]
    if suffix.startswith("::"):
        suffix = suffix[2:]
    return suffix.replace("::", ".")


def parse_namespace_functions(xml_dir: Path) -> list[APIItem]:
    """Parse free (namespace-level) functions from Doxygen XML.

    Complements parse_doxygen_xml (classes/structs only). Operators are
    skipped; overloaded names are flagged so the emitter can disambiguate
    with a parameter-type signature.
    """
    index_path = xml_dir / "index.xml"
    tree = ET.parse(index_path)
    root = tree.getroot()

    raw: list[APIItem] = []
    for compound in root.findall("compound"):
        if compound.get("kind") != "namespace":
            continue
        ns_name = compound.findtext("name", "")
        if not ns_name.startswith(ROOT_NS):
            continue
        if any(p in SKIP_SEGMENTS for p in ns_name.split("::")):
            continue

        detail_xml = xml_dir / f"{compound.get('refid', '')}.xml"
        if not detail_xml.exists():
            continue
        try:
            droot = ET.parse(detail_xml).getroot()
        except ET.ParseError:
            continue

        for md in droot.findall('.//sectiondef[@kind="func"]/memberdef[@kind="function"]'):
            fname = (md.findtext("name") or "").strip()
            if not fname or fname.startswith("operator"):
                continue
            # Skip member-template specializations that doxygen lists under
            # the namespace (e.g. "Env::get< std::string_view >"); "::" or "<"
            # in the unqualified name means it is not a plain free function.
            if "::" in fname or "<" in fname:
                continue

            loc = md.find("location")
            file_path = loc.get("file", "") if loc is not None else ""
            if file_path:
                if INCLUDE_ROOT not in file_path:
                    continue
                if any(skip in file_path for skip in SKIP_PATHS):
                    continue

            line = bodystart = bodyend = None
            bodyfile = ""
            if loc is not None:
                la, bsa, bea = loc.get("line"), loc.get("bodystart"), loc.get("bodyend")
                bodyfile = loc.get("bodyfile", "")
                line = int(la) if la and la.isdigit() else None
                bodystart = int(bsa) if bsa and bsa.isdigit() else None
                bodyend = int(bea) if bea and bea.isdigit() else None

            ptypes: list[str] = []
            for p in md.findall("param"):
                t = p.find("type")
                if t is not None:
                    ptypes.append(" ".join("".join(t.itertext()).split()))
            arg_types = "(" + ", ".join(ptypes) + ")"

            brief_el = md.find("briefdescription/para")
            brief = "".join(brief_el.itertext()).strip() if brief_el is not None else ""

            raw.append(
                APIItem(
                    name=f"{ns_name}::{fname}",
                    kind="function",
                    refid=md.get("id", ""),
                    file=file_path,
                    brief=brief,
                    line=line,
                    bodyfile=bodyfile,
                    bodystart=bodystart,
                    bodyend=bodyend,
                    ns_suffix=_ns_suffix_from_full(ns_name),
                    arg_types=arg_types,
                )
            )

    # Flag overloads (same qualified name appears more than once).
    counts: dict[str, int] = defaultdict(int)
    for i in raw:
        counts[i.name] += 1
    seen: set[tuple[str, str]] = set()
    out: list[APIItem] = []
    for i in raw:
        i.overloaded = counts[i.name] > 1
        key = (i.name, i.arg_types)
        if key in seen:
            continue
        seen.add(key)
        out.append(i)
    return out


# Minimum items for a module to get its own page. Smaller modules are
# merged into their parent namespace.
MIN_MODULE_SIZE = 3


def discover_modules(items: list[APIItem]) -> list[Module]:
    """Auto-discover modules from the namespace hierarchy of API items.

    Namespaces with fewer than MIN_MODULE_SIZE items are merged into
    their parent namespace to avoid tiny pages.
    """
    ns_items: dict[str, list[APIItem]] = defaultdict(list)

    for item in items:
        # Free functions carry their enclosing namespace directly; the
        # uppercase-terminated heuristic below only works for type names.
        if item.kind == "function":
            ns_items[item.ns_suffix or ""].append(item)
            continue

        suffix = item.name[len(ROOT_NS) :]
        if suffix.startswith("::"):
            suffix = suffix[2:]

        parts = suffix.split("::")
        ns_parts: list[str] = []
        for p in parts:
            if p and p[0].islower():
                ns_parts.append(p)
            elif p == "promise_type":
                break
            else:
                break

        ns_suffix = ".".join(ns_parts) if ns_parts else ""
        ns_items[ns_suffix].append(item)

    # Merge small namespaces into their parent
    merged: dict[str, list[APIItem]] = {}
    for ns_suffix in sorted(ns_items.keys(), key=lambda s: (-s.count("."), s)):
        items_list = ns_items[ns_suffix]
        if len(items_list) < MIN_MODULE_SIZE and ns_suffix:
            # Find parent namespace
            parent = ns_suffix.rsplit(".", 1)[0] if "." in ns_suffix else ""
            ns_items[parent].extend(items_list)
        else:
            merged[ns_suffix] = items_list

    # Rebuild after merging (parents may have absorbed children)
    final: dict[str, list[APIItem]] = {}
    for ns_suffix in sorted(merged.keys()):
        # Re-check: items may have been added by child merges
        all_items = ns_items[ns_suffix]
        if all_items:
            final[ns_suffix] = all_items

    # Build Module objects
    modules: list[Module] = []
    for ns_suffix in sorted(final.keys()):
        full_ns = f"{ROOT_NS}::{ns_suffix.replace('.', '::')}" if ns_suffix else ROOT_NS
        title = TITLE_OVERRIDES.get(ns_suffix, _auto_title(ns_suffix))
        filename = _ns_to_filename(ns_suffix)
        guide_page = GUIDE_PAGES.get(ns_suffix)

        modules.append(
            Module(
                ns_suffix=ns_suffix,
                full_ns=full_ns,
                title=title,
                filename=filename,
                guide_page=guide_page,
                items=final[ns_suffix],
            )
        )

    return modules


def _auto_title(ns_suffix: str) -> str:
    """Generate a human-readable title from namespace suffix."""
    if not ns_suffix:
        return "Core Runtime"
    # Take last segment, capitalize
    last = ns_suffix.rsplit(".", 1)[-1]
    return last.replace("_", " ").title()


def _ns_to_filename(ns_suffix: str) -> str:
    """Convert namespace suffix to output path mirroring the include directory structure.

    e.g. "coro" -> "core/coro"
         "utilities.fileio" -> "utilities/fileio"
         "utilities.composites.dft.aggregators" -> "utilities/composites/dft/aggregators"
         "" -> "core"  (root namespace items)
    """
    if not ns_suffix:
        return "core"

    # Map namespace segments to directory structure matching include/dftracer/utils/
    return ns_suffix.replace(".", "/")


def detect_repo_url(repo_root: Path) -> str:
    """Detect the GitHub repository URL for source links."""
    repo = os.environ.get("READTHEDOCS_GIT_REPOSITORY")
    if repo:
        repo = repo.removesuffix(".git")
        if repo.startswith("git@github.com:"):
            return repo.replace("git@github.com:", "https://github.com/", 1)
        if repo.startswith("https://github.com/"):
            return repo
        if repo.startswith("github.com/"):
            return f"https://{repo}"

    repo = os.environ.get("GITHUB_REPOSITORY")
    if repo:
        return f"https://github.com/{repo}"

    try:
        remote = (
            subprocess.check_output(
                ["git", "remote", "get-url", "origin"],
                cwd=repo_root,
                text=True,
            )
            .strip()
            .removesuffix(".git")
        )
        if remote.startswith("git@github.com:"):
            return remote.replace("git@github.com:", "https://github.com/", 1)
        if remote.startswith("https://github.com/"):
            return remote
    except Exception:
        pass

    return "https://github.com/LLNL/dftracer-utils"


def detect_source_ref(repo_root: Path) -> str:
    """Detect the git ref used for source links."""
    for env_name in ("READTHEDOCS_GIT_COMMIT_HASH", "GITHUB_SHA"):
        value = os.environ.get(env_name)
        if value:
            return value

    try:
        return (
            subprocess.check_output(
                ["git", "rev-parse", "HEAD"],
                cwd=repo_root,
                text=True,
            )
            .strip()
        )
    except Exception:
        return "develop"


def resolve_repo_path(repo_root: Path, item: APIItem) -> str | None:
    """Resolve a Doxygen location path to a repo-relative source file."""
    candidates = []
    if item.bodyfile:
        candidates.append(item.bodyfile)
    if item.file:
        candidates.append(item.file)

    for candidate in candidates:
        rel = Path(candidate)
        for base in (repo_root / "include", repo_root / "src"):
            full = base / rel
            if full.exists():
                return full.relative_to(repo_root).as_posix()
    return None


def source_link(repo_root: Path, repo_url: str, source_ref: str, item: APIItem) -> str | None:
    """Build a GitHub source link for an API item."""
    rel = resolve_repo_path(repo_root, item)
    if rel is None:
        return None

    start = item.bodystart or item.line
    end = item.bodyend or start
    url = f"{repo_url}/blob/{source_ref}/{rel}"
    if start is not None:
        url += f"#L{start}"
        if end is not None and end != start:
            url += f"-L{end}"
    return url


def generate_module_rst(
    mod: Module,
    repo_root: Path,
    repo_url: str,
    source_ref: str,
) -> str:
    """Generate RST for a single module page."""
    mod.items.sort(key=lambda x: (x.is_inner, x.name))

    lines: list[str] = []
    lines.append(mod.title)
    lines.append("=" * len(mod.title))
    lines.append("")
    lines.append(f"Namespace: ``{mod.full_ns}``")
    lines.append("")
    if mod.guide_page:
        lines.append(f"For usage guide and examples, see :doc:`/cpp_api/{mod.guide_page}`.")
        lines.append("")

    top_level = [
        i for i in mod.items if not i.is_inner and i.kind in ("class", "struct")
    ]

    for item in top_level:
        directive = "doxygenclass" if item.kind == "class" else "doxygenstruct"
        link = source_link(repo_root, repo_url, source_ref, item)
        if link:
            lines.append(f".. rst-class:: api-source-link")
            lines.append("")
            lines.append(f"   `source <{link}>`_")
            lines.append("")
        lines.append(f".. {directive}:: {item.name}")
        lines.append("   :project: dftracer-utils")
        lines.append("   :members:")
        lines.append("   :undoc-members:")
        lines.append("")

    functions = sorted(
        (i for i in mod.items if i.kind == "function"), key=lambda x: x.name
    )
    if functions:
        lines.append("Free Functions")
        lines.append("-" * len("Free Functions"))
        lines.append("")
        for item in functions:
            # Overloaded names need a parameter-type signature to resolve.
            target = item.name + (item.arg_types if item.overloaded else "")
            link = source_link(repo_root, repo_url, source_ref, item)
            if link:
                lines.append(f".. rst-class:: api-source-link")
                lines.append("")
                lines.append(f"   `source <{link}>`_")
                lines.append("")
            lines.append(f".. doxygenfunction:: {target}")
            lines.append("   :project: dftracer-utils")
            lines.append("")

    return "\n".join(lines)


def _build_toctree_hierarchy(
    modules: list[Module],
) -> dict[str, list]:
    """Build a directory tree from module filenames.

    Returns a dict mapping directory paths to lists of (entry, is_dir) tuples.
    entry is a module filename (leaf) or subdir name (branch).
    """
    dirs: dict[str, set[str]] = defaultdict(set)  # dir -> child dirs
    dir_leaves: dict[str, list[Module]] = defaultdict(list)  # dir -> leaf modules

    for mod in modules:
        parts = mod.filename.rsplit("/", 1)
        if len(parts) == 1:
            # Top-level module
            dir_leaves[""].append(mod)
        else:
            parent_dir, _leaf = parts
            dir_leaves[parent_dir].append(mod)
            # Register all ancestor directories
            segments = parent_dir.split("/")
            for i in range(len(segments)):
                ancestor = "/".join(segments[:i])
                child = "/".join(segments[: i + 1])
                dirs[ancestor].add(child)

    return dirs, dir_leaves


def generate_index_rst(modules: list[Module], output_dir: Path) -> None:
    """Generate index pages at each directory level."""
    dirs, dir_leaves = _build_toctree_hierarchy(modules)

    # Collect all directories that need an index
    all_dirs = set(dirs.keys()) | set(dir_leaves.keys())
    # Add intermediate dirs that only have subdirs
    for d in list(dirs.keys()):
        for child in dirs[d]:
            all_dirs.add(child)

    for dir_path in sorted(all_dirs):
        _generate_dir_index(dir_path, dirs, dir_leaves, modules, output_dir)


def _generate_dir_index(
    dir_path: str,
    dirs: dict[str, set[str]],
    dir_leaves: dict[str, list[Module]],
    all_modules: list[Module],
    output_dir: Path,
) -> None:
    """Generate an index.rst for a specific directory level."""
    is_root = dir_path == ""

    if is_root:
        title = "API Reference"
    else:
        last_segment = dir_path.rsplit("/", 1)[-1]
        title = TITLE_OVERRIDES.get(
            dir_path.replace("/", "."),
            last_segment.replace("_", " ").title() + " API",
        )

    lines: list[str] = []
    lines.append(title)
    lines.append("=" * len(title))
    lines.append("")

    if is_root:
        lines.append(
            "Complete reference for all public C++ classes and structs, "
            "auto-generated from Doxygen XML."
        )
        lines.append("")
        lines.append(".. tip::")
        lines.append("")
        lines.append(
            "   Each module page lists all classes and structs with full "
            "   member documentation. Mirrors the ``include/dftracer/utils/`` "
            "   directory structure."
        )
        lines.append("")

    # Toctree entries: subdirectory indexes + leaf modules
    entries: list[str] = []

    # Subdirectories (link to their index)
    child_dirs = sorted(dirs.get(dir_path, set()))
    for child in child_dirs:
        rel = child[len(dir_path) :].lstrip("/") if dir_path else child
        entries.append(f"{rel}/index")

    # Leaf modules in this directory; ones that collide with a subdir of the
    # same name are emitted as "<name>/_namespace" so the namespace page lives
    # inside the subdir's toctree (see resolved_filename in generate()).
    child_names = {c.rsplit("/", 1)[-1] for c in child_dirs}
    leaves = sorted(dir_leaves.get(dir_path, []), key=lambda m: m.filename)
    for mod in leaves:
        rel = mod.filename[len(dir_path) :].lstrip("/") if dir_path else mod.filename
        if rel in child_names:
            continue
        entries.append(rel)

    # Also include the namespace overview page when this dir's name was a
    # colliding leaf in the parent (file written as "<this>/_namespace.rst").
    if dir_path:
        leaf_name = dir_path.rsplit("/", 1)[-1] if "/" in dir_path else dir_path
        parent_dir = dir_path.rsplit("/", 1)[0] if "/" in dir_path else ""
        parent_leaves = dir_leaves.get(parent_dir, [])
        for mod in parent_leaves:
            parent_rel = (
                mod.filename[len(parent_dir) :].lstrip("/") if parent_dir else mod.filename
            )
            if parent_rel == leaf_name:
                entries.insert(0, "_namespace")
                break

    if entries:
        lines.append(".. toctree::")
        lines.append("   :maxdepth: 1")
        lines.append("")
        for entry in entries:
            lines.append(f"   {entry}")
        lines.append("")

    # Summary table (only at root)
    if is_root:
        lines.append("Summary")
        lines.append("-------")
        lines.append("")
        lines.append(".. list-table::")
        lines.append("   :header-rows: 1")
        lines.append("   :widths: 50 15 35")
        lines.append("")
        lines.append("   * - Module")
        lines.append("     - Items")
        lines.append("     - Namespace")

        collisions = {
            m.filename
            for m in all_modules
            if any(
                other.filename.startswith(m.filename + "/")
                for other in all_modules
                if other is not m
            )
        }
        total = 0
        for mod in all_modules:
            count = len(mod.items)
            total += count
            doc_path = (
                f"{mod.filename}/_namespace"
                if mod.filename in collisions
                else mod.filename
            )
            lines.append(f"   * - :doc:`{doc_path}`")
            lines.append(f"     - {count}")
            lines.append(f"     - ``{mod.full_ns}``")

        lines.append("   * - **Total**")
        lines.append(f"     - **{total}**")
        lines.append("     -")
        lines.append("")

    # Write
    if dir_path:
        out_path = output_dir / dir_path / "index.rst"
    else:
        out_path = output_dir / "index.rst"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines))


def generate(xml_dir: Path, output_dir: Path) -> None:
    """Main generation entry point. Called by conf.py or CLI."""
    repo_root = output_dir.parents[3]
    repo_url = detect_repo_url(repo_root)
    source_ref = detect_source_ref(repo_root)
    items = parse_doxygen_xml(xml_dir)
    functions = parse_namespace_functions(xml_dir)
    items.extend(functions)
    print(
        f"  Found {len(items)} public API items "
        f"({len(items) - len(functions)} classes/structs, {len(functions)} functions)"
    )

    modules = discover_modules(items)

    # Detect leaf modules whose filename collides with a sibling subdir:
    # e.g. "utilities/composites.rst" + directory "utilities/composites/".
    # Re-route those leaves into "<filename>/_namespace.rst" so the namespace
    # page lives under the subdir's toctree and Sphinx does not orphan it.
    dir_paths = {mod.filename.rsplit("/", 1)[0] for mod in modules if "/" in mod.filename}
    dir_paths |= {
        "/".join(mod.filename.split("/")[: i + 1])
        for mod in modules
        for i in range(len(mod.filename.split("/")) - 1)
    }
    collisions = {mod.filename for mod in modules if mod.filename in dir_paths}

    def resolved_filename(mod: "Module") -> str:
        return f"{mod.filename}/_namespace" if mod.filename in collisions else mod.filename

    # Generate per-module pages
    output_dir.mkdir(parents=True, exist_ok=True)
    expected_paths = {output_dir / f"{resolved_filename(mod)}.rst" for mod in modules}
    for mod in modules:
        rst = generate_module_rst(mod, repo_root, repo_url, source_ref)
        out_path = output_dir / f"{resolved_filename(mod)}.rst"
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(rst)

    for stale in output_dir.rglob("*.rst"):
        if stale.name == "index.rst":
            continue
        if stale not in expected_paths:
            stale.unlink()

    # Generate index pages at each directory level
    generate_index_rst(modules, output_dir)

    print(f"  Generated {len(modules)} module pages + index in {output_dir}/")
    for mod in modules:
        print(f"    {len(mod.items):3d}  {mod.title} -> {mod.filename}.rst")


def main():
    parser = argparse.ArgumentParser(
        description="Generate C++ API reference pages from Doxygen XML"
    )
    parser.add_argument(
        "--xml-dir",
        type=Path,
        default=Path("docs/doxygen/xml"),
        help="Doxygen XML output directory",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("docs/source/cpp_api/api"),
        help="Output directory for generated RST files",
    )
    args = parser.parse_args()
    generate(args.xml_dir, args.output_dir)


if __name__ == "__main__":
    main()
