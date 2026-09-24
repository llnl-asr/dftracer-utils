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
    "task_graph": "task_graph",
    "utilities.common.arrow": "arrow",
    "index": "indexer",
    "utilities.reader": "reader",
    "utilities.composites.dft.aggregators": "dft_aggregators",
    "utilities.composites.dft.indexing": "dft_indexing",
}

# Human-readable title overrides (key = namespace suffix after ROOT_NS)
TITLE_OVERRIDES: dict[str, str] = {
    "": "Runtime classes",
    "dataframe": "DataFrame and Series",
    "dataframe.field": "Field builder",
    "query": "Query builder",
    "plugins": "Plugin SDK",
    "plugins.reflect": "Reflection",
    "plugins.scalar": "Scalars",
    "logger": "Logging",
    "json": "JSON",
    "hash": "Hashing",
    "coro": "Coroutine Primitives",
    "io": "Async I/O",
    "task_graph": "Task Graph",
    "server": "HTTP Server",
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
    "index": "Indexer",
    "index.store": "Index Storage",
    "index.store.queries": "Index Queries",
    "index.gzip": "Gzip Checkpoints",
    "index.extensions": "Index Extensions",
    "index.profiles.dft": "dftracer Profile",
    "index.profiles.dft.agg": "dftracer Aggregation",
    "index.build": "Index Build",
    "index.plan": "Pruning",
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


CANONICAL_REPO_URL = "https://github.com/llnl-asr/dftracer-utils"


def detect_repo_url(repo_root: Path) -> str:
    """Detect the GitHub repository URL for source links.

    Resolves to the canonical LLNL repo by default. A contributor's local
    fork remote must never leak into published source links, so `git remote`
    is deliberately not consulted; set DFTRACER_DOCS_REPO_URL (or run under
    ReadTheDocs / GitHub Actions) to point elsewhere.
    """
    override = os.environ.get("DFTRACER_DOCS_REPO_URL")
    if override:
        return override.removesuffix(".git")

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

    return CANONICAL_REPO_URL


def detect_source_ref(repo_root: Path) -> str:
    """Detect the git ref used for source links."""
    override = os.environ.get("DFTRACER_DOCS_SOURCE_REF")
    if override:
        return override

    for env_name in ("READTHEDOCS_GIT_COMMIT_HASH", "GITHUB_SHA"):
        value = os.environ.get(env_name)
        if value:
            return value

    # Local builds point at the canonical repo (see detect_repo_url), where a
    # fork's local HEAD commit does not exist, so pin to the default branch
    # rather than an unresolvable SHA. CI provides the exact commit above.
    return "develop"


def _repo_rel(repo_root: Path, candidate: str) -> str | None:
    """Resolve one Doxygen location path to a repo-relative path, or None."""
    if not candidate:
        return None
    rel = Path(candidate)
    for base in (repo_root / "include", repo_root / "src"):
        full = base / rel
        if full.exists():
            return full.relative_to(repo_root).as_posix()
    return None


def resolve_repo_path(repo_root: Path, item: APIItem) -> str | None:
    """Resolve a Doxygen location path to a repo-relative source file."""
    for candidate in (item.bodyfile, item.file):
        rel = _repo_rel(repo_root, candidate)
        if rel is not None:
            return rel
    return None


def _line_count(path: Path) -> int:
    try:
        with path.open("rb") as f:
            return sum(1 for _ in f)
    except OSError:
        return 0


def _validated_range(repo_root: Path, rel: str, start: int, end: int) -> tuple[str, int, int] | None:
    """Clamp/validate a line range against the file's real length.

    Doxygen XML can go stale relative to the working tree (line numbers past
    the current file length); an out-of-range ``literalinclude`` only warns, but
    that is a new warning, so treat an untrustworthy range as unresolvable.
    """
    n = _line_count(repo_root / rel)
    if n == 0 or start < 1 or start > n or end > n:
        return None
    return rel, start, end


def resolve_source_lines(repo_root: Path, item: APIItem) -> tuple[str, int, int] | None:
    """Resolve the item's implementation body to (repo_rel_path, start, end).

    Prefers the definition (Doxygen ``bodyfile`` / ``bodystart`` / ``bodyend``)
    so the embedded source shows the real implementation, and falls back to the
    declaration location when no separate body is recorded. Returns None when no
    on-disk file and valid line range can be resolved, so callers can skip the
    embed gracefully rather than emit a literalinclude that warns or fails.
    """
    if item.bodyfile and item.bodystart:
        rel = _repo_rel(repo_root, item.bodyfile)
        if rel is not None:
            end = item.bodyend if (item.bodyend and item.bodyend >= item.bodystart) else item.bodystart
            valid = _validated_range(repo_root, rel, item.bodystart, end)
            if valid is not None:
                return valid
    if item.file:
        rel = _repo_rel(repo_root, item.file)
        if rel is not None:
            start = item.bodystart or item.line
            if start is None:
                return None
            end = item.bodyend if (item.bodyend and item.bodyend >= start) else start
            return _validated_range(repo_root, rel, start, end)
    return None


def emit_source_block(
    lines: list[str],
    rst_dir: Path,
    repo_root: Path,
    repo_url: str,
    source_ref: str,
    item: APIItem,
) -> None:
    """Append a GitHub source link for ``item`` to ``lines``.

    Emits a lightweight link to the definition on GitHub. The raw source is not
    embedded inline: for the C++ SDK types whose method signatures wrap the C
    ABI, an embedded header would paste ``dftu_`` C symbols into the C++ pages,
    which must stay pure C++ (the C ABI lives under ``c_api/``). Emits nothing
    when the source cannot be resolved.
    """
    link = source_link(repo_root, repo_url, source_ref, item)
    if link:
        lines.append(".. rst-class:: api-source-link")
        lines.append("")
        lines.append(f"   `source <{link}>`_")
        lines.append("")


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
    rst_dir: Path,
) -> str:
    """Generate an includable RST fragment for a single module (namespace).

    Emitted as a ``.rst.inc`` fragment (see :func:`generate`) that a curated
    thematic page pulls in with ``.. include::``. The fragment opens at section
    level (``-``) so it nests under the including page's title, and carries no
    toctree of its own. Every rendered symbol appears in exactly one fragment,
    so no breathe declaration is documented twice.
    """
    mod.items.sort(key=lambda x: (x.is_inner, x.name))

    lines: list[str] = []
    lines.append(mod.title)
    lines.append("-" * len(mod.title))
    lines.append("")
    lines.append(f"Namespace: ``{mod.full_ns}``")
    lines.append("")

    top_level = [
        i for i in mod.items if not i.is_inner and i.kind in ("class", "struct")
    ]

    for item in top_level:
        directive = "doxygenclass" if item.kind == "class" else "doxygenstruct"
        emit_source_block(lines, rst_dir, repo_root, repo_url, source_ref, item)
        lines.append(f".. {directive}:: {item.name}")
        lines.append("   :project: dftracer-utils")
        lines.append("   :members:")
        lines.append("   :undoc-members:")
        lines.append("")

    functions = sorted(
        (i for i in mod.items if i.kind == "function"), key=lambda x: x.name
    )
    if functions:
        lines.append("Free functions")
        lines.append("~" * len("Free functions"))
        lines.append("")
        for item in functions:
            # Overloaded names need a parameter-type signature to resolve.
            target = item.name + (item.arg_types if item.overloaded else "")
            emit_source_block(lines, rst_dir, repo_root, repo_url, source_ref, item)
            lines.append(f".. doxygenfunction:: {target}")
            lines.append("   :project: dftracer-utils")
            lines.append("")

    return "\n".join(lines)


def _fragment_key(mod: "Module") -> str:
    """Fragment basename for a module. Root namespace maps to ``core``."""
    return mod.ns_suffix or "core"


def generate(xml_dir: Path, output_dir: Path) -> None:
    """Emit one includable ``.rst.inc`` fragment per namespace module.

    ``output_dir`` is ``docs/source/cpp_api/_generated``. Fragments carry no
    toctree and are pulled into the curated thematic pages under ``cpp_api/``
    with ``.. include::``, so the generated members appear inside the curated
    pages and there is no separate visible ``api/`` section. ``.rst.inc`` is not
    a Sphinx source suffix, so the fragments are never treated as standalone
    documents (no orphan warnings).
    """
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

    output_dir.mkdir(parents=True, exist_ok=True)
    rst_dir = output_dir.parent

    keys: list[str] = []
    for mod in modules:
        key = _fragment_key(mod)
        keys.append(key)
        rst = generate_module_rst(mod, repo_root, repo_url, source_ref, rst_dir)
        (output_dir / f"{key}.rst.inc").write_text(rst)

    expected = {f"{k}.rst.inc" for k in keys}
    for stale in output_dir.glob("*.rst.inc"):
        if stale.name not in expected:
            stale.unlink()

    # Manifest lets a curated page / reviewer confirm every namespace fragment
    # is included by some thematic page (no silently dropped namespace).
    (output_dir / "_manifest.txt").write_text("\n".join(sorted(keys)) + "\n")

    print(f"  Generated {len(modules)} fragments in {output_dir}/")
    for mod in modules:
        print(f"    {len(mod.items):3d}  {mod.title} -> {_fragment_key(mod)}.rst.inc")


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
        default=Path("docs/source/cpp_api/_generated"),
        help="Output directory for generated RST include fragments",
    )
    args = parser.parse_args()
    generate(args.xml_dir, args.output_dir)


if __name__ == "__main__":
    main()
