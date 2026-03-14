# Minimal makefile for Sphinx documentation

# You can set these variables from the command line, and also
# from the environment for the first two.
SPHINXOPTS    ?=
SPHINXBUILD   ?= sphinx-build
SOURCEDIR     = source
BUILDDIR      = build

# Check if doxygen is available
DOXYGEN := $(shell command -v doxygen 2> /dev/null)
PYTHON   ?= python3

GENDIR   = $(SOURCEDIR)/_generated

# Put it first so that "make" without argument is like "make help".
help:
	@$(SPHINXBUILD) -M help "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

.PHONY: help Makefile clean doxygen diagrams html-nodoxygen

# Clean build directory, doxygen output, and generated diagrams
clean:
	rm -rf $(BUILDDIR)
	rm -rf doxygen
	rm -rf $(GENDIR)

# Generate Doxygen documentation (only if doxygen is installed)
doxygen:
ifdef DOXYGEN
	@echo "Running Doxygen..."
	@doxygen Doxyfile
else
	@echo "Warning: doxygen not found. Skipping C++ API documentation."
	@echo "Install doxygen to generate C++ API docs:"
	@echo "  macOS:        brew install doxygen"
	@echo "  Ubuntu/Debian: sudo apt-get install doxygen"
endif

# Generate Mermaid class diagrams from Doxygen XML
diagrams:
	@if [ -d doxygen/xml ]; then \
		echo "Generating Mermaid class diagrams from Doxygen XML..."; \
		$(PYTHON) scripts/generate_class_diagrams.py \
			--xml-dir doxygen/xml \
			--output-dir $(GENDIR); \
	else \
		echo "Warning: doxygen/xml not found. Run 'make doxygen' first."; \
	fi

# Build HTML with optional Doxygen
html:
ifdef DOXYGEN
	@echo "Building documentation with C++ API (doxygen found)..."
	@$(MAKE) doxygen
	@$(MAKE) diagrams
else
	@echo "Building documentation without C++ API (doxygen not found)..."
	@echo "To include C++ documentation, install doxygen and run 'make html' again."
endif
	@$(SPHINXBUILD) -M html "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

# Build HTML without Doxygen (skip C++ docs)
html-nodoxygen:
	@echo "Building documentation without C++ API..."
	@$(SPHINXBUILD) -M html "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

# Catch-all target: route all unknown targets to Sphinx using the new
# "make mode" option.  $(O) is meant as a shortcut for $(SPHINXOPTS).
%: Makefile
	@$(SPHINXBUILD) -M $@ "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)
