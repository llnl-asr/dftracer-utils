Python API Reference
====================

This section contains the Python API documentation for dftracer utilities.

.. toctree::
   :maxdepth: 2
   :caption: Python Modules:

   reader
   indexer

Module Overview
---------------

.. mermaid::

   graph LR
       Package["dftracer.utils"] --> Reader["reader<br/>Trace file reading"]
       Package --> Indexer["indexer<br/>Indexing & search"]
       Reader --> Stream["Stream API<br/>(bytes, lines)"]
       Indexer --> Checkpoint["Checkpoint<br/>random access"]

The dftracer utilities Python package provides the following main modules:

- :doc:`reader` - Trace file reading utilities
- :doc:`indexer` - Indexing and searching capabilities
