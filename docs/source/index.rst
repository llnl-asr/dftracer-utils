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
    :maxdepth: 2
    :caption: Contents:

    installation
    quickstart
    pipeline
    cli
    server
    trace-viewer
    utilities
    api/index
    cpp_api/index
    developers

.. toctree::
   :maxdepth: 1
   :caption: Links:

   DFTracer Documentation <https://dftracer.readthedocs.io/>
   DFTracer GitHub <https://github.com/LLNL/dftracer>

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
   table = agg.process("./traces", time_interval_ms=1000.0)

   # Include extra per-event numeric fields as Arrow columns
   table = agg.process("./traces", custom_metric_fields=["bytes"])

Citation
--------

If you use this software, please cite:

   Ray A. O. Sinurat, William Nixon, Haryadi S. Gunawi, Nikoli Dryden, and
   Hariharan Devarajan. 2026. HORATIO: Bridging Management and Analysis of
   Traces at Scale. In The International Conference on Scalable Scientific Data
   Management 2026 (SSDBM 2026), August 11-13, 2026, San Diego, CA, USA. ACM,
   New York, NY, USA. doi: `10.1145/3828820.3828825
   <https://doi.org/10.1145/3828820.3828825>`_

.. code-block:: bibtex

   @inproceedings{sinurat2026horatio,
     author    = {Sinurat, Ray A. O. and Nixon, William and Gunawi, Haryadi S. and Dryden, Nikoli and Devarajan, Hariharan},
     title     = {HORATIO: Bridging Management and Analysis of Traces at Scale},
     year      = {2026},
     isbn      = {979-8-4007-2708-5},
     publisher = {Association for Computing Machinery},
     address   = {New York, NY, USA},
     doi       = {10.1145/3828820.3828825},
     booktitle = {The International Conference on Scalable Scientific Data Management 2026 (SSDBM 2026)},
     location  = {San Diego, CA, USA},
     series    = {SSDBM 2026},
   }

Indices and tables
==================

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`
