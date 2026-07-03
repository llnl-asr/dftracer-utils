Arrow Data Infrastructure
=========================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/utilities/common/arrow>`.


Arrow data interchange infrastructure using nanoarrow. All classes are in
the ``dftracer::utils::utilities::common::arrow`` namespace and reachable
through the umbrella header:

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/arrow/arrow.h>

   using namespace dftracer::utils::utilities::common::arrow;

The columnar builder and ``ArrowExportResult`` are guarded by
``DFTRACER_UTILS_ENABLE_ARROW`` (ON by default). The IPC writer/reader,
parallel reader, and partitioning types additionally require
``DFTRACER_UTILS_ENABLE_ARROW_IPC``.

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
           PW["PartitionWriter"]
           PR["PartitionRouter"]
       end

       subgraph Read["File Input"]
           IRD["IpcReader"]
           PRD["read_arrow_files_parallel"]
       end

       RBB -->|"finish()"| AER
       AER -->|"write_batch()"| IPC
       AER -->|"route()"| PR
       PR -->|"per-partition"| PW
       AER -->|"PyCapsule"| Python["Python ArrowBatch"]
       IRD -->|"read_batch()"| AER
       PRD -->|"per file"| AER

RecordBatchBuilder
------------------

*What*: type-safe columnar builder that produces an Arrow record batch via
nanoarrow. *When*: build a batch in memory before handing it to an
``IpcWriter``, a ``PartitionRouter``, or the Python boundary. Two modes:

- **Static schema**: ``declare_schema()`` upfront, direct index append,
  no hash lookups. Best for utility ``to_arrow()`` methods with known schemas.
- **Dynamic schema**: ``add_or_get_column()`` discovers columns from data,
  ``end_row()`` backfills nulls for missing columns. Best for
  ``TraceReader.iter_arrow()`` with arbitrary JSON.

Once the first row has been finalized the schema is **locked**
(``lock_schema()``): subsequent rows may only append values into the
already-discovered columns, and attempts to add new columns after the lock
are rejected. This makes batches produced by the dynamic path safe to
concatenate across a ``TraceReader::read_arrow()`` stream without re-keying.

Column types are ``ColumnType::{INT64, UINT64, DOUBLE, STRING, BOOL,
DICT_STRING}``. String columns copy and own their data, so there is no
lifetime requirement on the source strings passed to ``append_string()``.
The builder is **not** thread-safe: use one per worker/coroutine.

Key signatures:

.. code-block:: cpp

   void declare_schema(std::initializer_list<ColumnSpec> specs);
   std::size_t add_or_get_column(std::string_view name, ColumnType type);
   std::optional<std::size_t> find_column(std::string_view name) const;
   void append_int64(std::size_t col_idx, std::int64_t value);
   void append_uint64(std::size_t col_idx, std::uint64_t value);
   void append_double(std::size_t col_idx, double value);
   void append_string(std::size_t col_idx, std::string_view value);
   void append_dict_string(std::size_t col_idx, std::string_view value);
   void append_bool(std::size_t col_idx, bool value);
   void append_null(std::size_t col_idx);
   void end_row();
   ArrowExportResult finish();
   void reset(bool keep_schema = true);

Static-schema example:

.. code-block:: cpp

   RecordBatchBuilder builder;
   builder.declare_schema({
       {"id", ColumnType::INT64},
       {"name", ColumnType::STRING},
       {"value", ColumnType::DOUBLE},
   });

   builder.append_int64(0, 42);
   builder.append_string(1, "hello");
   builder.append_double(2, 3.14);
   builder.end_row();          // validates every column was appended

   ArrowExportResult batch = builder.finish();

Dynamic-schema example (columns discovered per row, nulls backfilled):

.. code-block:: cpp

   RecordBatchBuilder builder;
   std::size_t c_dur = builder.add_or_get_column("dur", ColumnType::INT64);
   builder.append_int64(c_dur, 1200);
   builder.end_row();          // "dur" only

   std::size_t c_name = builder.add_or_get_column("name", ColumnType::STRING);
   builder.append_string(c_name, "read");
   builder.end_row();          // backfills null into "dur" for this row

   ArrowExportResult batch = builder.finish();

ArrowExportResult
-----------------

*What*: move-only RAII container owning both the ``ArrowArray`` and
``ArrowSchema`` produced by ``RecordBatchBuilder::finish()`` (backed by
``nanoarrow::UniqueSchema`` / ``nanoarrow::UniqueArray``). *Why*: it is
self-contained and safe to move across threads and channels, so a builder
worker can hand a batch to a writer coroutine without a copy.

Key signatures:

.. code-block:: cpp

   ArrowArray*  get_array()  noexcept;
   ArrowSchema* get_schema() noexcept;
   int64_t      num_rows()    const noexcept;
   int64_t      num_columns() const noexcept;
   bool         valid()       const noexcept;
   nanoarrow::UniqueArray  release_array();
   nanoarrow::UniqueSchema release_schema();

Example:

.. code-block:: cpp

   ArrowExportResult batch = builder.finish();
   if (batch.valid()) {
       LOG_INFO("%lld rows x %lld cols", (long long)batch.num_rows(),
                (long long)batch.num_columns());
   }

array_view
----------

*What*: ``init_array_view()`` is a free helper that initializes a nanoarrow
``ArrowArrayView`` from a schema and binds it to an array in one call.
*When*: read the values back out of an ``ArrowExportResult`` (or any
``ArrowArray``) column-by-column. On any failure the view is reset and the
nanoarrow error code is returned; on success the caller owns the view and
must call ``ArrowArrayViewReset`` on it.

Signature:

.. code-block:: cpp

   int init_array_view(ArrowArrayView& view, ArrowSchema* schema,
                       ArrowArray* array);   // NANOARROW_OK on success

Example:

.. code-block:: cpp

   ArrowArrayView view;
   int rc = init_array_view(view, batch.get_schema(), batch.get_array());
   if (rc == NANOARROW_OK) {
       // read from view.children[...] here
       ArrowArrayViewReset(&view);
   }

IpcWriter
---------

*What*: async streaming Arrow IPC file writer (``.arrow`` / ``.arrows``).
*Why*: output that pyarrow, polars, DuckDB, and any Arrow-compatible tool
can read. *When*: persist one or more batches to disk from inside an
executor. It supports buffer-level compression: when built with
``DFTRACER_UTILS_ENABLE_ZSTD``, ``IpcCompression::ZSTD`` is the default for
new files, producing pyarrow-compatible compressed IPC streams
(``DEFAULT_ARROW_IPC_COMPRESSION`` picks ZSTD if available, else NONE).

Guarded by ``DFTRACER_UTILS_ENABLE_ARROW_IPC``. Move-only and not
thread-safe; it uses ``Executor::current()`` for I/O, so every method must
be ``co_await``-ed from within an executor context. Sequence:
``open()`` -> ``write_batch()`` [1..N] -> ``close()``. Each coroutine
returns ``int`` (0 on success).

Key signatures:

.. code-block:: cpp

   coro::CoroTask<int> open(const std::string& path,
                            IpcCompression compression = DEFAULT_ARROW_IPC_COMPRESSION,
                            std::size_t pool_slots = 4);
   coro::CoroTask<int> write_batch(ArrowExportResult& batch);
   coro::CoroTask<int> write_batches(std::vector<ArrowExportResult>& batches);
   coro::CoroTask<int> close();
   bool is_open() const noexcept;

Example (awaited inside a coroutine):

.. code-block:: cpp

   coro::CoroTask<int> write_output(ArrowExportResult& batch) {
       IpcWriter writer;
       if (co_await writer.open("output.arrows") != 0) co_return -1;
       if (co_await writer.write_batch(batch) != 0) co_return -1;
       co_await writer.close();
       co_return 0;
   }

Writing many batches at once:

.. code-block:: cpp

   coro::CoroTask<int> write_all(std::vector<ArrowExportResult>& batches) {
       IpcWriter writer;
       co_await writer.open("output.arrows");
       co_await writer.write_batches(batches);
       co_await writer.close();
       co_return 0;
   }

IpcReader
---------

*What*: RAII reader for the Arrow IPC file format written by ``IpcWriter``.
*When*: read batches back for processing. Unlike ``IpcWriter``, its methods
are **synchronous** (not coroutines): it uses memory-mapped I/O for
zero-copy access, a shared schema (no per-batch deep copy), and buffer reuse
for ZSTD decompression compatible with pyarrow / polars. Sequence:
``open()`` -> ``num_batches()`` -> ``read_batch(i)`` (or ``read_all()`` /
``for_each_batch()``). Move-only, not thread-safe.

Guarded by ``DFTRACER_UTILS_ENABLE_ARROW_IPC``.

Key signatures:

.. code-block:: cpp

   int open(const std::string& path);              // 0 on success
   std::size_t num_batches() const noexcept;
   std::int64_t total_rows() const noexcept;
   ArrowExportResult read_batch(std::size_t index);
   std::vector<ArrowExportResult> read_all();
   int for_each_batch(std::function<int(ArrowExportResult&)> callback);

Example:

.. code-block:: cpp

   IpcReader reader;
   if (reader.open("output.arrows") == 0) {
       for (std::size_t i = 0; i < reader.num_batches(); ++i) {
           ArrowExportResult batch = reader.read_batch(i);
           consume(batch);
       }
   }

parallel_reader
---------------

*What*: free coroutine helpers that read many Arrow IPC files concurrently.
*When*: fan out over a directory of ``.arrow`` files. ``read_arrow_file_async``
reads one file; ``read_arrow_files_parallel`` collects all results before
returning; ``read_arrow_files_streaming`` delivers each file result via a
callback in completion order (return ``false`` to cancel) and must run
within a ``CoroScope``.

Guarded by ``DFTRACER_UTILS_ENABLE_ARROW_IPC``.

Key signatures:

.. code-block:: cpp

   coro::CoroTask<ArrowFileReadResult> read_arrow_file_async(std::string path);
   coro::CoroTask<ParallelReadResult>  read_arrow_files_parallel(
       std::vector<std::string> paths);
   coro::CoroTask<ParallelReadResult>  read_arrow_files_streaming(
       CoroScope& scope, std::vector<std::string> paths,
       FileResultCallback callback);   // std::function<bool(ArrowFileReadResult&&)>

Example:

.. code-block:: cpp

   coro::CoroTask<std::int64_t> count_rows(std::vector<std::string> paths) {
       ParallelReadResult res = co_await read_arrow_files_parallel(std::move(paths));
       LOG_INFO("read %zu files, %zu failed", res.files_read, res.files_failed);
       co_return res.total_rows;
   }

PartitionWriter
---------------

*What*: async wrapper around ``IpcWriter`` that writes ``part-NNNNN.arrow``
files into a directory, rotating to a new file when a byte threshold is
exceeded. *When*: use as the per-partition output of ``PartitionRouter``, or
directly when a single output stream with automatic rotation is needed.
``close()`` returns ``PartitionWriteStats`` (files, per-file row counts,
totals). Guarded by ``DFTRACER_UTILS_ENABLE_ARROW_IPC``.

Key signatures:

.. code-block:: cpp

   coro::CoroTask<int> open(const std::string& output_dir, int64_t chunk_size_bytes,
                            IpcCompression compression = DEFAULT_ARROW_IPC_COMPRESSION);
   coro::CoroTask<int> write_batch(ArrowExportResult& batch);
   coro::CoroTask<PartitionWriteStats> close();

Example:

.. code-block:: cpp

   coro::CoroTask<int> write_rotating(ArrowExportResult& batch) {
       PartitionWriter writer;
       co_await writer.open("out/data", /*chunk_size_bytes=*/64 * 1024 * 1024);
       co_await writer.write_batch(batch);
       PartitionWriteStats stats = co_await writer.close();
       co_return static_cast<int>(stats.total_rows);
   }

PartitionRouter
---------------

*What*: multi-partition Arrow router. *When*: split an inbound batch across
many output directories keyed by column value, hash bucket, or a predicate
"view". It dispatches rows into one ``PartitionWriter`` per partition and
aggregates ``RouterWriteStats`` across all of them. Partitioning is driven by
``PartitionConfig`` (``Mode::{NONE, COLUMN, BUCKETED, VIEW}``,
``partition_columns``, ``num_buckets``, ``views``); ``VIEW`` mode uses
predicates registered with ``register_predicate()``. ``open()`` and
``register_predicate()`` are synchronous; ``write_batch()`` and ``close()``
are coroutines. Guarded by ``DFTRACER_UTILS_ENABLE_ARROW_IPC``.

Key signatures:

.. code-block:: cpp

   int open(const std::string& output_dir, const PartitionConfig& config,
            int64_t chunk_size_bytes,
            IpcCompression compression = DEFAULT_ARROW_IPC_COMPRESSION);
   void register_predicate(const std::string& view_name, PredicateEvaluator evaluator);
   coro::CoroTask<int> write_batch(ArrowExportResult& batch);
   coro::CoroTask<RouterWriteStats> close();

Example (partition by the ``cat`` column):

.. code-block:: cpp

   coro::CoroTask<int64_t> partition_by_cat(ArrowExportResult& batch) {
       PartitionConfig config;
       config.mode = PartitionConfig::Mode::COLUMN;
       config.partition_columns = {"cat"};

       PartitionRouter router;
       router.open("out/by_cat", config, /*chunk_size_bytes=*/64 * 1024 * 1024);
       co_await router.write_batch(batch);
       RouterWriteStats stats = co_await router.close();
       co_return stats.total_rows;
   }
