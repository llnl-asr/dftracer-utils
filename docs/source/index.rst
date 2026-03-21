.. dftracer utilities documentation master file

Welcome to dftracer utilities documentation!
============================================

**dftracer utilities** is a collection of utilities for `DFTracer <https://dftracer.readthedocs.io/>`_,
providing powerful tools for trace file reading, indexing, and processing. The library includes
both C++ APIs and Python bindings for flexible integration.

Features
--------

- **High-performance trace file reading**: Efficient reading of compressed trace files
- **Arrow data interchange**: Columnar Arrow output via nanoarrow for zero-copy access from pyarrow, polars, and DuckDB
- **Utility bindings**: Python bindings for statistics, views, aggregation, bloom queries, and reorganization
- **Indexing capabilities**: Fast indexing and searching of trace data with bloom filters
- **Pipeline processing**: Parallel data processing with tasks, coroutines, and channels
- **Arrow IPC file output**: Write results as Arrow IPC files for pyarrow, polars, and DuckDB
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

   from dftracer.utils import TraceReader

   # Read a trace file (auto-detects index sidecar)
   reader = TraceReader("path/to/trace.pfw.gz")

   # Read all lines as JSON
   for obj in reader.iter_lines_json():
       print(obj["name"], obj["dur"])

   # Read as Arrow for columnar access
   table = reader.read_arrow()
   df = table.to_pandas()  # requires pyarrow

   # Aggregate traces in a directory
   from dftracer.utils.utilities import AggregatorUtility
   agg = AggregatorUtility()
   table = agg.process("./traces", time_interval=1.0)

Indices and tables
==================

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`
