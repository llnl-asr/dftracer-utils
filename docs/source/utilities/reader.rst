:description: The streaming reader for indexed compressed traces: line- and byte-based access, zero-copy reads, and async I/O over .pfw.gz plus .dftindex.

Reader
================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference </cpp_api/reader>`.

Streaming reader for compressed trace files with support for line-based and byte-based access, zero-copy reads, and async I/O.

.. code-block:: cpp

   #include <dftracer/utils/utilities/reader/trace_reader.h>

Overview
--------

The reader provides random access into indexed compressed files
(``.pfw.gz`` + ``.dftindex``). It supports multiple stream types for
different access patterns and both synchronous and asynchronous reads.

The high-level :cpp:class:`dftracer::utils::utilities::reader::TraceReader`
exposes:

- **Directory input**: the higher-level :class:`~dftracer.utils.TraceViewer`
  (see :ref:`Python API <reader-python-api>` below) accepts a directory of
  ``.pfw.gz`` files that share one ``.dftindex`` root and are processed in
  parallel (each file becomes one or more checkpoint-level work items
  routed across the runtime thread pool).
- **JSON streaming** (``read_json``): each line is parsed once with a
  reused ``simdjson`` ondemand ``JsonParser``; the yielded ``JsonLine``
  borrows the parser until the next ``next()`` call.
- **Arrow streaming** (``read_arrow``): yields native
  ``ArrowExportResult`` record batches sized at ``batch_size`` rows.
- **Query filtering**: an optional ``query`` DSL string is compiled into
  AND-of-EQ probes when possible. The compiled probes evaluate directly
  against simdjson fields, with a uniform-match shortcut when every
  candidate chunk fully matches the predicate (no per-event re-evaluation).
- **Line-range work items**: the dispatcher splits a file's checkpoints
  into independent line-range work items that the runtime executes in
  parallel; ``ReadConfig::skip_pruning`` lets the dispatcher avoid
  re-running the chunk pruner per work item.
- **Batch chunk pruning**: a single pruner pass per file feeds all work
  items, using ``ChunkPrunerUtility`` over bloom filters, chunk
  statistics, and the manifest CF.
- **flatten_objects**: when set, top-level JSON object values
  (e.g. ``args``) are expanded one level into ``parent.child`` columns
  with native Arrow types; deeper nesting still round-trips as a JSON
  text column.

ReaderFactory
-------------

Creates readers with automatic format detection.

.. code-block:: cpp

   using namespace dftracer::utils::utilities::reader::internal;

   auto reader = ReaderFactory::create(
       "trace.pfw.gz",       // Compressed file
       "trace-dir/.dftindex" // Index directory (empty string auto-generates one)
   );

   // Query file metadata
   std::size_t num_lines = reader->get_num_lines();
   std::size_t max_bytes = reader->get_max_bytes();

Stream Types
------------

.. code-block:: cpp

   enum class StreamType {
       BYTES,              // Raw bytes, no line awareness
       LINE_BYTES,         // Line-boundary-aligned bytes (one line at a time)
       MULTI_LINES_BYTES,  // Line-boundary-aligned bytes (multiple lines)
       LINE,               // Single parsed line per read()
       MULTI_LINES         // Multiple parsed lines per read()
   };

   enum class RangeType {
       LINE_RANGE,  // Range specified as line numbers (1-based)
       BYTE_RANGE   // Range specified as byte offsets
   };

StreamConfig
~~~~~~~~~~~~

Fluent builder for configuring streams:

.. code-block:: cpp

   StreamConfig config;
   config.stream_type(StreamType::LINE)
         .range_type(RangeType::LINE_RANGE)
         .from(100)    // Start line (1-based)
         .to(200);     // End line (inclusive)

Reading Lines
-------------

**Read lines one at a time:**

.. code-block:: cpp

   auto reader = ReaderFactory::create("trace.pfw.gz", "trace-dir/.dftindex");

   StreamConfig config;
   config.stream_type(StreamType::LINE)
         .range_type(RangeType::LINE_RANGE)
         .from(1)
         .to(100);

   auto stream = reader->stream(config);
   std::string buffer(1024 * 1024, '\0');

   while (!stream->done()) {
       std::size_t bytes = stream->read(buffer.data(), buffer.size());
       if (bytes == 0) break;

       std::string_view line(buffer.data(), bytes);
       process(line);
   }

**Async reading (coroutine):**

.. code-block:: cpp

   auto stream = reader->stream(config);
   std::string buffer(1024 * 1024, '\0');

   while (!stream->done()) {
       std::size_t bytes = co_await stream->read_async(
           buffer.data(), buffer.size());
       if (bytes == 0) break;

       std::string_view line(buffer.data(), bytes);
       process(line);
   }

Reading Byte Ranges
-------------------

Read lines within a byte range (useful for parallel splitting):

.. code-block:: cpp

   StreamConfig config;
   config.stream_type(StreamType::LINE_BYTES)
         .range_type(RangeType::BYTE_RANGE)
         .from(0)
         .to(1024 * 1024);  // First 1MB

   auto stream = reader->stream(config);
   // Each read() returns one complete line within the byte range

Line Processing Callbacks
-------------------------

Process lines without materializing them into a container:

.. code-block:: cpp

   class MyProcessor : public LineProcessor {
   public:
       coro::CoroTask<bool> process(const char* data,
                                     std::size_t length) override {
           std::string_view line(data, length);
           // Process line...
           co_return true;  // Continue processing
       }
   };

   MyProcessor processor;
   co_await reader->read_lines_with_processor_async(1, 1000, processor);  // Lines 1-1000

C API
-----

Opaque handle-based interface for C interoperability:

.. code-block:: c

   #include <dftracer/utils/utilities/reader/internal/reader.h>

   /* Create reader (index_ckpt_size = 0 uses the default) */
   dftu_reader_handle_t reader = dftu_reader_create(
       "trace.pfw.gz", "trace-dir/.dftindex", 0);

   /* Query metadata (returns non-zero on error) */
   size_t num_lines = 0;
   dftu_reader_get_num_lines(reader, &num_lines);

   /* Create stream */
   dftu_stream_config_t config = {
       .stream_type = DFTU_STREAM_TYPE_LINE,
       .range_type = DFTU_RANGE_TYPE_LINES,
       .start = 1,
       .end = 100,
       .buffer_size = 0,  /* 0 = use default */
   };
   dftu_reader_stream_t stream = dftu_reader_stream(reader, &config);

   /* Read lines */
   char buffer[1024 * 1024];
   while (!dftu_reader_stream_done(stream)) {
       size_t bytes = dftu_reader_stream_read(
           stream, buffer, sizeof(buffer));
       if (bytes == 0) break;
       /* process buffer[0..bytes-1] */
   }

   /* Cleanup */
   dftu_reader_stream_destroy(stream);
   dftu_reader_destroy(reader);

.. _reader-python-api:

Python API
----------

There is no Python binding for ``TraceReader`` directly; the Python API
reads traces through :class:`dftracer.utils.TraceViewer`, a lazy,
composable view that wraps the same reader/indexer machinery. Its
``collect()`` builds the query plan and returns a ``LazyFrame``; that
``LazyFrame``'s own ``collect()`` runs the scan and returns an Arrow-backed
:class:`~dftracer.utils.DataFrame`.

.. code-block:: python

   from dftracer.utils import TraceViewer

   viewer = TraceViewer("trace.pfw.gz")

   # Filter with the query DSL and collect to a DataFrame
   df = viewer.filter("cat == 'POSIX'").collect().collect()
   print(df)

See Also
--------

- :doc:`indexer` - Indexer that builds the ``.dftindex`` store readers depend on
- :doc:`fileio` - Higher-level file I/O with async generators
- :doc:`/api/index` - Python API documentation
