:description: Read a gzip trace at the lowest C++ level: stream lines one at a time with StreamingLineReader, or pull a small file into memory in one call.

Stream lines from a trace
=========================

.. admonition:: Goal
   :class: goal

   Read a gzip trace at the lowest level - one line at a time, without
   loading the whole file - or pull an entire small file into memory in one call.
   These are the raw read paths under the trace readers and views; reach for them
   when you are building your own scanner rather than querying through the
   :doc:`../../trace-viewer`.

This is a C++ API. From Python, read traces through the ``TraceViewer`` and
DataFrame layer instead; these streaming primitives are not bound.

Stream lines with StreamingLineReader
-------------------------------------

``StreamingLineReader`` (header
``dftracer/utils/utilities/fileio/lines/streaming_line_reader.h``, namespace
``dftracer::utils::utilities::fileio::lines``) yields lines from a gzip trace
through an async generator. Configure it with ``StreamingLineReaderConfig``, a
fluent builder:

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>
   using namespace dftracer::utils::utilities::fileio::lines;

   auto cfg = StreamingLineReaderConfig()
                  .with_file("trace-0.pfw.gz");

   auto gen = StreamingLineReader::read_async(cfg);
   while (auto line = co_await gen.next()) {   // std::optional<Line>, nullopt at EOF
       process(*line);
   }

``read_async`` is a ``static`` method returning a ``coro::AsyncGenerator<Line>``;
iterate it with ``co_await gen.next()`` inside a coroutine (see
:doc:`../pipelines/patterns` for running one). ``Line`` is defined in the same
directory's ``line_types.h``.

The reader is oriented at gzip traces. When you provide an index and restrict to
a line range, it takes the indexed seek path; otherwise it streams the members
sequentially:

.. code-block:: cpp

   auto cfg = StreamingLineReaderConfig()
                  .with_file("traces/trace-0.pfw.gz")
                  .with_index("traces/.dftindex")   // the index dir, must already exist
                  .with_line_range(/*start_line=*/1000, /*end_line=*/2000);

``with_index`` takes the ``.dftindex`` directory that ``dftracer_index`` built
for the trace (not a per-file suffix); the reader seeks through it via the same
index the rest of the library uses. The index path is used only when you pass it
explicitly and it exists on disk. There is no auto-discovery: without
``with_index`` it always streams the gzip members sequentially.

Read a whole file with FileReaderUtility
----------------------------------------

When the file is small and you want it all at once, ``FileReaderUtility``
(header ``dftracer/utils/utilities/fileio/file_reader_utility.h``, namespace
``dftracer::utils::utilities::fileio``) reads the entire file into memory as
binary. Its ``operator()`` is a coroutine taking a ``filesystem::FileEntry`` and
returning a ``text::Text``:

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/file_reader_utility.h>
   #include <dftracer/utils/utilities/filesystem/types.h>
   using namespace dftracer::utils::utilities;

   filesystem::FileEntry entry{"trace-0.pfw"};
   fileio::FileReaderUtility read;
   text::Text contents = co_await read(entry);

``FileEntry`` carries ``path``, ``size``, ``mtime``, ``is_directory``, and
``is_regular_file``; construct it from a path (it populates the size by
default). The utility throws ``DFTUtilsException`` on a missing file, a
non-regular file, or an open failure. It does no gzip decode and no line
splitting - it returns the raw bytes. For gzip line streaming, use
``StreamingLineReader`` above.

See also
--------

- :doc:`compression` - produce the multi-member ``.pfw.gz`` these readers
  consume.
- :doc:`../pipelines/patterns` - the coroutine scope that drives ``co_await``.
- :doc:`../../trace-viewer` - the high-level query API most callers want.
