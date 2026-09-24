:description: The complete generated API reference: the Python package, the C++20 engine, and the stable dftu\_ C ABI.

Reference
=========

The complete, generated API. Reference is for looking one thing up mid-task; for
learning, start with :doc:`Get started <../getting-started/index>`, and for the
why, see :doc:`Concepts <../concepts/index>`.

.. grid:: 1 1 3 3
   :gutter: 3

   .. grid-item-card:: :octicon:`file-code` Python API
      :link: ../api/index
      :link-type: doc

      The ``dftracer.utils`` package: the DataFrame engine, TraceViewer, the
      query and columnar DSLs, the JIT, and the indexer.

   .. grid-item-card:: :octicon:`cpu` C++ API
      :link: ../cpp_api/index
      :link-type: doc

      The C++20 engine: the runtime, DataFrame ``Series``/``DataFrame``, the
      query builder, the trace layer, and the plugin SDK.

   .. grid-item-card:: :octicon:`file-binary` C API Reference
      :link: ../c_api/index
      :link-type: doc

      The stable ``dftu_`` C ABI for C consumers, FFI, and hand-written
      plugins: the DataFrame, query, and plugin interfaces.

.. toctree::
   :hidden:
   :maxdepth: 1

   ../api/index
   ../cpp_api/index
   ../c_api/index
   record-schema
