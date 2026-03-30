Arrow Data Infrastructure
=========================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/utilities/common/arrow>`.


Arrow data interchange infrastructure using nanoarrow. All classes are in
the ``dftracer::utils::utilities::common::arrow`` namespace.

Guarded by ``DFTRACER_UTILS_ENABLE_ARROW`` (ON by default).

.. mermaid::

   graph LR
       subgraph Build["Building"]
           RBB["RecordBatchBuilder"]
       end

       subgraph Transport["Transport"]
           AER["ArrowExportResult"]
       end

       subgraph Write["File Output"]
           IPC["IpcWriter"]
       end

       RBB -->|"finish()"| AER
       AER -->|"write_batch()"| IPC
       AER -->|"PyCapsule"| Python["Python ArrowBatch"]

RecordBatchBuilder
------------------

Type-safe columnar builder with two modes:

- **Static schema**: ``declare_schema()`` upfront, direct index append,
  no hash lookups. Best for utility ``to_arrow()`` methods with known schemas.
- **Dynamic schema**: ``add_or_get_column()`` discovers columns from data,
  ``end_row()`` backfills nulls for missing columns. Best for
  ``TraceReader.iter_arrow()`` with arbitrary JSON.

String columns store ``string_view`` into source data for zero-copy during
build; bulk copy only at ``finish()``. Caller must keep source data alive
until ``finish()`` returns.

ArrowExportResult
-----------------

Move-only RAII wrapper holding ``nanoarrow::UniqueSchema`` and
``nanoarrow::UniqueArray``. Self-contained and safe to send across
threads and channels.

IpcWriter
---------

Streaming Arrow IPC file writer. Writes ``.arrows`` files that can be
read by pyarrow, polars, DuckDB, and any Arrow-compatible tool.

Guarded by ``DFTRACER_UTILS_ENABLE_ARROW_IPC``.

Usage Example
-------------

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/arrow/arrow.h>

   using namespace dftracer::utils::utilities::common::arrow;

   // Build a batch
   RecordBatchBuilder builder;
   builder.declare_schema({
       {"id", ColumnType::INT64},
       {"name", ColumnType::STRING},
       {"value", ColumnType::DOUBLE},
   });
   builder.append_int64(0, 42);
   builder.append_string(1, "hello");
   builder.append_double(2, 3.14);
   builder.end_row();
   auto batch = builder.finish();

   // Write to IPC file
   IpcWriter writer;
   writer.open("output.arrows");
   writer.write_batch(batch);
   writer.close();
