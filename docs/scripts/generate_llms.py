"""Emit llms.txt and llms-full.txt at the end of an HTML build.

``llms.txt`` is a nested map linking to the rendered pages; ``llms-full.txt``
inlines every page converted to Markdown in reading order. Links are relative.

Wired from ``conf.py`` via ``build-finished``. Needs markdownify +
beautifulsoup4; without them it falls back to the raw reStructuredText sources.
"""

from __future__ import annotations

import re
from pathlib import Path

try:
    from bs4 import BeautifulSoup
    from markdownify import markdownify as _markdownify

    _HAVE_MD = True
except Exception:
    _HAVE_MD = False

# Sphinx smartquotes / autodoc glyphs -> plain ASCII (keyed by code point).
_ASCII = {
    0x2192: "->",
    0x00B6: "",
    0x00A0: " ",
    0x2018: "'",
    0x2019: "'",
    0x201C: '"',
    0x201D: '"',
    0x2013: "-",
    0x2014: "-",
    0x2026: "...",
}

_SUMMARY = (
    "Reading, indexing, aggregating, and analyzing DFTracer trace files "
    "(.pfw / .pfw.gz): a coroutine-based C++20 engine with a native columnar "
    "SIMD DataFrame, the duql query language, and a plugin SDK, exposed through Python "
    "bindings (import as `dftracer.utils`) and `dftracer_*` CLI binaries."
)

_CONTEXT = (
    "Links are relative to this file. `llms-full.txt` inlines every page as "
    "Markdown (the API reference expanded) in one file."
)

# Top-level toctree entries treated as secondary (skippable for short context).
_OPTIONAL = {"developers", "changelog", "citation", "open-source"}


def _title(app, docname: str) -> str:
    node = app.env.titles.get(docname)
    return (node.astext() if node is not None else docname).translate(_ASCII)


def _description(app, docname: str) -> str:
    """The page's :description: meta field, if any (empty otherwise)."""
    meta = app.env.metadata.get(docname, {})
    return str(meta.get("description", "")).strip().translate(_ASCII)


def _children(app, docname: str) -> list[str]:
    """Docnames this page pulls in through its toctree directives, in order."""
    return app.env.toctree_includes.get(docname, [])


def _page_uri(app, docname: str) -> str:
    uri = app.builder.get_target_uri(docname)
    if uri.endswith("/"):
        uri += "index.html"
    return uri


def _src_link(app, docname: str) -> str:
    """Relative link to the raw reStructuredText source (fallback)."""
    rel = Path(app.env.doc2path(docname)).relative_to(app.srcdir)
    return f"_sources/{rel.as_posix()}{app.config.html_sourcelink_suffix or '.txt'}"


def clean_html_to_md(html: str) -> str | None:
    """Clean Markdown from a rendered-page HTML fragment or full document.

    Shared by the llms.txt dump and the per-page copy-to-clipboard button
    (conf.py). Accepts either a whole built page or just its body HTML.
    """
    if not _HAVE_MD:
        return None
    soup = BeautifulSoup(html, "html.parser")
    root = soup.find("article", attrs={"role": "main"}) or soup
    for el in root.select(".headerlink, .viewcode-link, .mermaid"):
        el.decompose()
    # Cross-reference links are noise in a flat text dump: every C++ type
    # reference carries a mangled ``#_CPPv4...`` anchor and every Python type an
    # intersphinx URL. Keep the link text, drop the link.
    for a in root.select("a.reference.internal"):
        a.replace_with(a.get_text())
    for a in root.select("a.reference.external[href]"):
        if "docs.python.org" in a.get("href", ""):
            a.replace_with(a.get_text())
    try:
        text = _markdownify(
            str(root),
            heading_style="ATX",
            escape_underscores=False,
            escape_asterisks=False,
        )
    except TypeError:  # older markdownify without the escape_* options
        text = _markdownify(str(root), heading_style="ATX")
    text = text.translate(_ASCII)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def _page_markdown(app, docname: str) -> str | None:
    """Clean Markdown for a built page, from its rendered HTML file."""
    if not _HAVE_MD:
        return None
    html_path = Path(app.outdir) / _page_uri(app, docname)
    try:
        return clean_html_to_md(html_path.read_text(encoding="utf-8"))
    except OSError:
        return None


# ---- llms.txt (map) --------------------------------------------------------


def _list_item(app, docname: str, depth: int, link: str) -> str:
    item = f"{'  ' * depth}- [{_title(app, docname)}]({link})"
    desc = _description(app, docname)
    return f"{item}: {desc}" if desc else item


def _subtree(app, docname: str, depth: int, seen: set[str], link_for) -> list[str]:
    out = [_list_item(app, docname, depth, link_for(app, docname))]
    for child in _children(app, docname):
        if child in seen:
            continue
        seen.add(child)
        out.extend(_subtree(app, child, depth + 1, seen, link_for))
    return out


def _build_llms_txt(app, link_for) -> str:
    root = app.config.root_doc
    lines = [f"# {app.config.project}", "", f"> {_SUMMARY}", "", _CONTEXT, ""]

    optional: list[str] = []
    more: list[str] = []  # non-section top-level leaves (cli, environment, ...)
    for entry in _children(app, root):
        if entry in _OPTIONAL:
            optional.append(entry)
            continue
        children = _children(app, entry)
        if children:  # a section index: one H2, its subtree nested beneath
            lines.append(f"## {_title(app, entry)}")
            lines.append("")
            seen = {root, entry}
            for child in children:
                if child in seen:
                    continue
                seen.add(child)
                lines.extend(_subtree(app, child, 0, seen, link_for))
            lines.append("")
        else:
            more.append(entry)

    for heading, group in (("More", more), ("Optional", optional)):
        if group:
            lines.append(f"## {heading}")
            lines.append("")
            lines.extend(_list_item(app, d, 0, link_for(app, d)) for d in group)
            lines.append("")

    return "\n".join(lines).rstrip() + "\n"


# ---- ordering + full dump --------------------------------------------------


def _ordered_pages(app) -> list[str]:
    """All docnames in toctree (reading) order, depth first from the root."""
    root = app.config.root_doc
    seen = {root}
    order = [root]

    def walk(docname: str) -> None:
        for child in _children(app, docname):
            if child in seen:
                continue
            seen.add(child)
            order.append(child)
            walk(child)

    walk(root)
    return order


def _build_llms_full_txt(app, ordered: list[str], page_body) -> str:
    parts = [
        f"# {app.config.project}",
        "",
        f"> {_SUMMARY}",
        "",
        "This file inlines every documentation page in reading order.",
        "",
    ]
    for docname in ordered:
        body = page_body(app, docname)
        if body is None:
            continue
        link = _page_uri(app, docname) if _HAVE_MD else _src_link(app, docname)
        parts.append("=" * 80)
        parts.append(f"# {_title(app, docname)}")
        parts.append(f"Source: {link}")
        parts.append("=" * 80)
        parts.append("")
        parts.append(body)
        parts.append("")
    return "\n".join(parts).rstrip() + "\n"


def generate(app, exception) -> None:
    if exception is not None or app.builder.name not in ("html", "dirhtml"):
        return
    outdir = Path(app.outdir)
    ordered = _ordered_pages(app)

    if _HAVE_MD:  # links map to the rendered pages; the full dump is Markdown
        link_for = _page_uri

        def page_body(app, docname):
            return _page_markdown(app, docname)
    else:  # fallback: link and inline the raw reStructuredText sources
        link_for = _src_link

        def page_body(app, docname):
            try:
                return Path(app.env.doc2path(docname)).read_text(encoding="utf-8").rstrip()
            except OSError:
                return None

    (outdir / "llms.txt").write_text(_build_llms_txt(app, link_for), encoding="utf-8")
    (outdir / "llms-full.txt").write_text(
        _build_llms_full_txt(app, ordered, page_body), encoding="utf-8"
    )
