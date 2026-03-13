.. dftracer utilities documentation master file

Welcome to dftracer utilities documentation!
============================================

**dftracer utilities** is a collection of utilities for `DFTracer <https://dftracer.readthedocs.io/>`_,
providing powerful tools for trace file reading, indexing, and processing. The library includes
both C++ APIs and Python bindings for flexible integration.

Features
--------

- **High-performance trace file reading**: Efficient reading of compressed trace files
- **Indexing capabilities**: Fast indexing and searching of trace data
- **Pipeline processing**: Parallel data processing with tasks, coroutines, and channels
- **Task graphs**: DAG-based workflow builder with fan-out, fan-in, map, reduce patterns
- **Python bindings**: Easy-to-use Python interface
- **Cross-platform**: Works on Linux, macOS, and other Unix-like systems

.. toctree::
   :maxdepth: 1
   :caption: Links:

   DFTracer Documentation <https://dftracer.readthedocs.io/>
   DFTracer GitHub <https://github.com/LLNL/dftracer>

.. toctree::
    :maxdepth: 2
    :caption: Contents:

    installation
    quickstart
    pipeline
    cli
    server
    utilities
    replay
    api/index
    cpp_api/index
    developers

Getting Started
---------------

To get started with dftracer utilities, check out the :doc:`installation` guide
and then follow the :doc:`quickstart` tutorial.

Installation
~~~~~~~~~~~~

.. code-block:: bash

   pip install dftracer-utils

For more detailed installation instructions, see :doc:`installation`.

Quick Example
~~~~~~~~~~~~~

.. code-block:: python

   from dftracer.utils import Reader

   # Read a trace file
   reader = Reader("path/to/trace.pfw.gz", "path/to/trace.pfw.gz.idx")

   # Read specific lines
   lines = reader.read_lines(0, 100)  # Read first 100 lines
   for line in lines:
       print(line)

Indices and tables
==================

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`
