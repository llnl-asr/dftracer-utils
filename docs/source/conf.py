# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

import datetime
import os
import re
import subprocess
import sys
from pathlib import Path

# conf.py's own directory holds _ext_stub; ensure it is importable.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _ext_stub import install_extension_stub  # noqa: E402

# Auto-generate Mermaid class diagrams from Doxygen XML before building
_docs_dir = Path(__file__).parent.parent  # docs/

# Regenerate the Doxygen XML first so a plain sphinx-build (e.g. Read the Docs,
# which does not run `make html`) never serves stale C/C++ API from an old dump.
# No-op when doxygen is not installed.
import shutil  # noqa: E402

_doxyfile = _docs_dir / "Doxyfile"
if shutil.which("doxygen") and _doxyfile.exists():
    print("Running Doxygen to (re)generate XML...")
    subprocess.run(["doxygen", str(_doxyfile)], cwd=str(_docs_dir), check=False)

_script = _docs_dir / "scripts" / "generate_class_diagrams.py"
_xml_dir = _docs_dir / "doxygen" / "xml"


def _name_files_by_path(xml_dir: Path) -> None:
    # Breathe 5 matches a doxygenfile path against the index name, which
    # Doxygen writes as the bare file name, so "dftracer/utils/query/abi.h"
    # would never match. Name each file compound by its location path.
    index = xml_dir / "index.xml"
    if not index.exists():
        return
    location = re.compile(r'<compounddef[^>]*kind="file".*?<location file="([^"]+)"', re.S)

    def full_name(m: re.Match) -> str:
        compound = xml_dir / f"{m.group(1)}.xml"
        found = location.search(compound.read_text()) if compound.exists() else None
        name = found.group(1) if found else m.group(2)
        return f'<compound refid="{m.group(1)}" kind="file"><name>{name}</name>'

    text = index.read_text()
    renamed = re.sub(r'<compound refid="([^"]+)" kind="file"><name>([^<]*)</name>', full_name, text)
    if renamed != text:
        index.write_text(renamed)


_name_files_by_path(_xml_dir)


# Doxygen gives a function-pointer field or typedef as type "R (*)", name,
# args "(...)", which Breathe joins into the invalid "R (*) name (...)"; move
# the closing parenthesis so it reads "R (* name)(...)".
_FUNCTION_POINTER = re.compile(
    r'(<memberdef kind="(?:variable|typedef)"[^>]*>\s*<type>(?:(?!</type>).)*?)'
    r"\(\*\)</type>((?:(?!</memberdef>).)*?<argsstring>)\(",
    re.S,
)
# Doxygen writes constexpr / consteval both as an attribute and in the type,
# and Breathe prints both; keep the attribute.
_SPECIFIER_IN_TYPE = re.compile(
    r'(<memberdef [^>]*\b(constexpr|consteval)="yes"[^>]*>\s*<type>)\2\b\s*'
)


def _fix_member_declarations(xml_dir: Path) -> None:
    for path in xml_dir.glob("*.xml"):
        text = path.read_text()
        fixed = _FUNCTION_POINTER.sub(r"\1(*</type>\2)(", text)
        fixed = _SPECIFIER_IN_TYPE.sub(r"\1", fixed)
        if fixed != text:
            path.write_text(fixed)


_fix_member_declarations(_xml_dir)
_gen_dir = _docs_dir / "source" / "_generated"
if _script.exists() and _xml_dir.exists():
    print("Generating Mermaid class diagrams from Doxygen XML...")
    subprocess.run(
        [
            sys.executable,
            str(_script),
            "--xml-dir",
            str(_xml_dir),
            "--output-dir",
            str(_gen_dir),
        ],
        check=False,
    )

# Generate Python collaboration diagrams from the package sources (ast-based,
# no Doxygen needed).
_py_script = _docs_dir / "scripts" / "generate_python_diagrams.py"
_pkg_dir = _docs_dir.parent / "python" / "dftracer" / "utils"
if _py_script.exists() and _pkg_dir.exists():
    print("Generating Mermaid diagrams from Python sources...")
    subprocess.run(
        [
            sys.executable,
            str(_py_script),
            "--pkg-dir",
            str(_pkg_dir),
            "--output-dir",
            str(_gen_dir),
        ],
        check=False,
    )

# Auto-generate C++ API reference pages from Doxygen XML
_api_script = _docs_dir / "scripts" / "generate_api_index.py"
_api_out = _docs_dir / "source" / "cpp_api" / "_generated"
if _api_script.exists() and _xml_dir.exists():
    print("Generating C++ API reference pages from Doxygen XML...")
    subprocess.run(
        [
            sys.executable,
            str(_api_script),
            "--xml-dir",
            str(_xml_dir),
            "--output-dir",
            str(_api_out),
        ],
        check=False,
    )

ON_READTHEDOCS = os.environ.get("READTHEDOCS", "").lower() == "true"
PYTHON_SOURCE_DIR = _docs_dir.parent / "python"
autodoc_mock_imports = []


if ON_READTHEDOCS:
    sys.path.insert(0, str(PYTHON_SOURCE_DIR))
    install_extension_stub()
    # Mock the heavy optional deps. pandas hard-imports pyarrow (reads
    # pa.__version__), so pandas/numpy must be mocked whenever pyarrow is,
    # or importing dftracer.utils.dfanalyzer for autodoc fails.
    autodoc_mock_imports = [
        "numpy",
        "pandas",
        "pyarrow",
        "dask",
        "dask.distributed",
    ]

try:
    import dftracer.utils

    print("✓ dftracer.utils package found and imported successfully.")
except (ImportError, ModuleNotFoundError) as e:
    if not ON_READTHEDOCS and PYTHON_SOURCE_DIR.exists():
        print(f"Warning: installed dftracer.utils package not found: {e}")
        print("Falling back to source package with RTD extension stubs.")
        sys.path.insert(0, str(PYTHON_SOURCE_DIR))
        install_extension_stub()
        autodoc_mock_imports = [
            "numpy",
            "pandas",
            "pyarrow",
            "dask",
            "dask.distributed",
        ]
        import dftracer.utils  # noqa: F401  (imported for autodoc side effect)
    else:
        print(f"Warning: dftracer.utils package not found: {e}")
        print("API documentation will have limited information.")
        print("To generate full API docs, install the package: pip install -e .")

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information


project = "dftracer-utils"
# Compute the year explicitly rather than relying on Sphinx's strftime %Y
# handling (only added in Sphinx 8.1), so the footer is correct on any version.
copyright = f"{datetime.date.today().year}, Ray Andrew Sinurat, Hariharan Devarajan"
author = "Ray Andrew Sinurat, Hariharan Devarajan"


# The version info for the project. Resolved automatically (git tags via
# setuptools_scm, then the generated _version.py, then installed metadata) so
# the docs never drift from a hardcoded number. The full ``release`` keeps the
# local segment, so dev builds show the distance since the last tag and the
# commit hash (e.g. "0.1.dev535+g926fc871"); ``version`` is the short "X.Y".
def _resolve_release() -> str:
    # 1. Git tags via setuptools_scm - same provider as pyproject.toml, but
    #    keeping the local (+g<hash>) segment so dev docs show the exact commit.
    try:
        from setuptools_scm import get_version

        return get_version(
            root=str(_docs_dir.parent),
            relative_to=__file__,
            version_scheme="post-release",
        )
    except Exception:
        pass
    # 2. The setuptools_scm-generated _version.py, reattaching the commit id.
    try:
        import importlib.util

        vf = _docs_dir.parent / "python" / "dftracer" / "utils" / "_version.py"
        if vf.exists():
            spec = importlib.util.spec_from_file_location("_dftracer_version", vf)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            commit = getattr(mod, "commit_id", None)
            return f"{mod.version}+{commit}" if commit else mod.version
    except Exception:
        pass
    # 3. Installed package metadata (wheel / editable install).
    try:
        from importlib.metadata import version as _pkg_version

        return _pkg_version("dftracer-utils")
    except Exception:
        pass
    return "0.0.0"


release = _resolve_release()

# Collapse the setuptools_scm local segment to just "+g<7-char hash>",
# dropping the dirty-tree date marker (".dYYYYMMDD") so the version stays short.
release = re.sub(r"\+g([0-9a-fA-F]+).*$", lambda m: "+g" + m.group(1)[:7], release)
version = ".".join(release.split(".")[:2])

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = [
    "sphinx.ext.autodoc",
    # "sphinx.ext.autosummary",  # Disabled: manual docs in api/reader.rst and api/indexer.rst
    "sphinx.ext.napoleon",
    "sphinx.ext.viewcode",
    "sphinx.ext.intersphinx",
    "sphinx.ext.todo",
    "sphinx.ext.coverage",
    "sphinx.ext.mathjax",
    # sphinx_autodoc_typehints disabled: it strips types from signatures
    # and loses C extension __text_signature__. Sphinx's built-in autodoc
    # handles both Python type hints and C extension __text_signature__.
    "myst_parser",  # For Markdown support
    "sphinx_design",  # Tabs, cards, grids (language tabs, landing page)
    "sphinx_copybutton",  # Copy-to-clipboard on code blocks (Shibuya styles it)
    "breathe",  # Always enable breathe
    "sphinx.ext.ifconfig",  # For conditional inclusion
    "sphinxcontrib.mermaid",  # Mermaid diagrams
]

# copybutton: strip prompts and REPL markers so a copy yields runnable input.
copybutton_prompt_text = r">>> |\.\.\. |\$ "
copybutton_prompt_is_regexp = True
copybutton_only_copy_prompt_lines = False

# MyST: enable the extensions the Markdown guides use.
myst_enable_extensions = ["colon_fence", "deflist", "attrs_inline"]
myst_heading_anchors = 3

# Mermaid: brand each diagram with a neon-blue palette injected as mermaid v11
# frontmatter config (see setup() at end of file). Frontmatter is honored at
# parse time - unlike initialize() themeVariables, which mermaid ignores here,
# and unlike CSS/JS, which cannot re-raster the d3-zoom-composited SVG. One
# dark-blue-card palette reads on both light and dark pages.
mermaid_version = "11"
mermaid_d3_zoom = True
_MM_NEON = {
    "primaryColor": "#12233a",
    "mainBkg": "#12233a",
    "secondaryColor": "#16304a",
    "tertiaryColor": "#0e1a2a",
    "primaryBorderColor": "#35e6ff",
    "nodeBorder": "#35e6ff",
    "clusterBorder": "#274b5f",
    "clusterBkg": "#0e1a2a",
    "primaryTextColor": "#dcf7ff",
    "nodeTextColor": "#dcf7ff",
    "textColor": "#dcf7ff",
    "titleColor": "#dcf7ff",
    "lineColor": "#22d3ee",
    "edgeLabelBackground": "#0a1018",
    "classText": "#dcf7ff",
    "fontSize": "14px",
}

# Check if Doxygen XML output exists and set up Breathe config
doxygen_xml_path = Path(__file__).parent.parent / "doxygen" / "xml"
if doxygen_xml_path.exists():
    cpp_api_enabled = True
    # Breathe configuration for C++ documentation
    breathe_projects = {"dftracer-utils": str(doxygen_xml_path)}
    breathe_default_project = "dftracer-utils"
else:
    cpp_api_enabled = False
    print("Warning: Doxygen XML output not found. C++ API documentation will be skipped.")
    print(f"Expected path: {doxygen_xml_path}")
    print("Run 'doxygen Doxyfile' in the docs directory to generate C++ documentation.")

# Napoleon settings for Google/NumPy style docstrings
napoleon_google_docstring = True
napoleon_numpy_docstring = True
napoleon_include_init_with_doc = True
napoleon_include_private_with_doc = False
napoleon_include_special_with_doc = True
napoleon_use_admonition_for_examples = False
napoleon_use_admonition_for_notes = False
napoleon_use_admonition_for_references = False
napoleon_use_ivar = False
napoleon_use_param = True
napoleon_use_rtype = True
napoleon_preprocess_types = False
napoleon_type_aliases = None
napoleon_attr_annotations = True

# Add mappings for intersphinx - link to main DFTracer docs and Python docs
intersphinx_mapping = {
    "python": ("https://docs.python.org/3", None),
    "dftracer": ("https://dftracer.readthedocs.io/en/latest/", None),
}

templates_path = ["_templates"]
exclude_patterns = ["api/_autosummary"]

# The suffix(es) of source filenames.
source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

# The master toctree document.
master_doc = "index"

# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = "shibuya"
html_static_path = ["_static"]
html_css_files = ["custom.css"]
html_js_files = ["copy-page.js"]
html_favicon = "_static/logo-dark.svg"

# Search configuration
html_search_language = "en"

# Theme options (Shibuya). Logos and accent are further shaped by custom.css.
html_theme_options = {
    "accent_color": "cyan",
    "light_logo": "_static/logo-light.svg",
    "dark_logo": "_static/logo-dark.svg",
    "github_url": "https://github.com/LLNL/dftracer-utils",
    "globaltoc_expand_depth": 1,
    # Shibuya's built-in "Copy page" fetches the source over the network, which
    # fails for a private repo and behind firewalls; we inject our own that
    # copies embedded Markdown (see _inject_copy_page), so disable the theme's.
    "show_ai_links": False,
    "nav_links": [
        {"title": "Get started", "url": "getting-started/index"},
        {"title": "Tutorials", "url": "tutorials/index"},
        {"title": "Guides", "url": "guides/index"},
        {"title": "Reference", "url": "reference/index"},
        {"title": "Concepts", "url": "concepts/index"},
    ],
}

# -- Options for autodoc -----------------------------------------------------
autodoc_default_options = {
    "members": True,
    "member-order": "bysource",
    "undoc-members": True,
    "exclude-members": "__weakref__",
}

# Type annotations in both signature and description
autodoc_typehints = "both"
autodoc_typehints_description_target = "documented"

# Do not print the module path before class/function names, so the internal
# ``dftracer_utils_ext`` extension module name never leaks into the reference -
# users import everything from ``dftracer.utils``.
add_module_names = False

# -- Options for todo extension ----------------------------------------------
todo_include_todos = True

# -- Options for autosummary -------------------------------------------------
autosummary_generate = False


# -- Mermaid neon theming (frontmatter injection) ----------------------------
import json as _json  # noqa: E402

_MM_FRONTMATTER = (
    "---\nconfig:\n  theme: base\n  themeVariables: " + _json.dumps(_MM_NEON) + "\n---\n"
)


def _inject_mermaid_theme(app, doctree, docname):
    try:
        from sphinxcontrib.mermaid import mermaid as _mermaid_node
    except Exception:
        return
    for node in doctree.findall(_mermaid_node):
        code = node.get("code", "")
        if code and "themeVariables" not in code:
            node["code"] = _MM_FRONTMATTER + code


def _inject_copy_page(app, pagename, templatename, context, doctree):
    """Write the page's Markdown to a sibling <page>.md and inject a "Copy page"
    / "Download page" control that reads it from the same origin. No fetch to an
    external host, so it works with a private repo and behind a firewall, and the
    Markdown lives in its own file rather than bloating the HTML. Same rendering
    as llms-full.txt (see copy-page.js)."""
    if doctree is None or not context.get("body"):
        return
    try:
        import generate_llms

        md = generate_llms.clean_html_to_md(context["body"])
    except Exception:
        return
    if not md:
        return
    md_path = Path(app.outdir) / f"{pagename}.md"
    try:
        md_path.parent.mkdir(parents=True, exist_ok=True)
        md_path.write_text(md, encoding="utf-8")
    except OSError:
        return
    # Sibling file, so a link relative to this page is just its basename.
    name = pagename.rsplit("/", 1)[-1] + ".md"
    context["body"] = (
        f'<div class="dftu-copy-page" data-dftu-copy-page data-md="{name}">'
        '<div class="dftu-copy-page-bar">'
        '<button type="button" class="dftu-copy-page-btn" data-action="copy">'
        "Copy page</button>"
        '<button type="button" class="dftu-copy-page-toggle" aria-haspopup="menu"'
        ' aria-expanded="false" aria-label="More actions"></button>'
        "</div>"
        '<div class="dftu-copy-page-menu" role="menu" hidden>'
        '<button type="button" role="menuitem" data-action="copy">Copy page</button>'
        f'<a role="menuitem" href="{name}" download>Download page</a>'
        "</div>"
        "</div>"
    ) + context["body"]


def setup(app):
    app.connect("doctree-resolved", _inject_mermaid_theme)

    sys.path.insert(0, str(_docs_dir / "scripts"))
    try:
        import generate_llms

        app.connect("build-finished", generate_llms.generate)
        app.connect("html-page-context", _inject_copy_page)
    except Exception as exc:  # a broken llms generator must not fail the build
        print(f"llms.txt generation disabled: {exc}")

    return {"parallel_read_safe": True, "parallel_write_safe": True}
