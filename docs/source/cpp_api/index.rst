:description: Index of the C++20 engine API: the coroutine runtime, columnar DataFrame, duql builder, trace-analysis layer, and plugin SDK.

C++ API Reference
=================

The C++20 engine: the coroutine runtime, the columnar DataFrame, the duql
builder, the trace-analysis layer, and the plugin SDK. Every page renders C++
classes and namespaces from the source through Doxygen and Breathe. For the
stable C ABI, see :doc:`../c_api/index`.

Architecture
------------

The five layered libraries and how they build on one another. ``core`` is the
foundation; each arrow points to the library built on top, up to the top domain
layer that composes all below it.

.. mermaid:: /_generated/architecture.mmd

.. toctree::
   :maxdepth: 1
   :caption: Engine

   runtime
   coro
   io
   task_graph

.. toctree::
   :maxdepth: 1
   :caption: Data

   dataframe
   duql
   arrow

.. toctree::
   :maxdepth: 1
   :caption: Traces

   reader
   indexer
   trace

.. toctree::
   :maxdepth: 1
   :caption: Extending and serving

   plugins
   server
   utilities
