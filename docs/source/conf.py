# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

import os
import subprocess
import sys
from pathlib import Path

# Don't add project root to path - we want to use the installed package from site-packages
# If we add the project root, Python will try to import from source which doesn't have the compiled .so
# sys.path.insert(0, str(Path(__file__).parent.parent.parent))

# Auto-generate Mermaid class diagrams from Doxygen XML before building
_docs_dir = Path(__file__).parent.parent  # docs/
_script = _docs_dir / "scripts" / "generate_class_diagrams.py"
_xml_dir = _docs_dir / "doxygen" / "xml"
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

# Auto-generate C++ API reference pages from Doxygen XML
_api_script = _docs_dir / "scripts" / "generate_api_index.py"
_api_out = _docs_dir / "source" / "cpp_api" / "api"
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

# Mock imports for packages that may not be available during doc build
autodoc_mock_imports = []

# Try to import the package
try:
    import dftracer.utils

    print("✓ dftracer.utils package found and imported successfully.")
except (ImportError, ModuleNotFoundError) as e:
    print(f"Warning: dftracer.utils package not found: {e}")
    print("API documentation will have limited information.")
    print("To generate full API docs, install the package: pip install -e .")
    # Don't mock - let it fail to show what's missing
    # autodoc_mock_imports = ['dftracer', 'dftracer.utils']

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information


project = "dftracer-utils"
copyright = "%Y, Ray Andrew Sinurat, Hariharan Devarajan"
author = "Ray Andrew Sinurat, Hariharan Devarajan"

# The version info for the project
# Try to get version from the package
try:
    from importlib.metadata import version

    release = version("dftracer-utils")
    version = ".".join(release.split(".")[:2])
except Exception:
    version = "0.1"
    release = "0.1.0"

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
    "breathe",  # Always enable breathe
    "sphinx.ext.ifconfig",  # For conditional inclusion
    "sphinxcontrib.mermaid",  # Mermaid diagrams
]

# Mermaid configuration
mermaid_version = "11"
mermaid_init_js = "mermaid.initialize({startOnLoad:true, theme:'neutral'});"
mermaid_d3_zoom = True

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

html_theme = "furo"
html_static_path = ["_static"]

# Search configuration
html_search_language = "en"

# Theme options
html_theme_options = {
    "navigation_with_keys": True,
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

# -- Options for todo extension ----------------------------------------------
todo_include_todos = True

# -- Options for autosummary -------------------------------------------------
autosummary_generate = False
