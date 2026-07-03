Error Handling
==============

dftracer utilities uses a typed error model built around a single error
category enum, a value type for recoverable failures, and an exception
hierarchy for unrecoverable ones. Both the C++ API and the Python bindings
expose the same categories, so a failure raised deep in the library surfaces
as a specific, catchable error.

Everything below lives in ``dftracer::utils`` and is declared in
``<dftracer/utils/core/common/error.h>``.

ErrorCode
---------

``ErrorCode`` is the coarse failure category attached to every error:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Code
     - Meaning
   * - ``UNKNOWN``
     - Unclassified failure.
   * - ``INTERNAL``
     - Broken invariant or library bug.
   * - ``INVALID_ARGUMENT``
     - Bad caller input.
   * - ``NOT_FOUND``
     - Missing file, key, or entity.
   * - ``IO``
     - Filesystem or I/O failure.
   * - ``PARSE``
     - Parse or decode failure (JSON, trace format, ...).
   * - ``COMPRESSION``
     - (De)compression failure or corrupt compressed data.
   * - ``QUERY``
     - Query DSL error.
   * - ``READER``
     - Reader subsystem failure.
   * - ``INDEXER``
     - Indexer subsystem failure.
   * - ``PIPELINE``
     - Pipeline or executor failure.
   * - ``AGGREGATION``
     - Aggregation subsystem failure.

``error_code_name(ErrorCode)`` returns the code's name as a ``const char*``.

DFTUtilsError and Result<T>
---------------------------

``DFTUtilsError`` is the error carried as a value. Use it with ``Result<T>``
for recoverable failures, reserving exceptions for the unrecoverable:

.. code-block:: cpp

   struct DFTUtilsError {
       ErrorCode code = ErrorCode::UNKNOWN;
       std::string message;
       std::string format() const;  // "<CODE>: <message>"
   };

   template <typename T>
   using Result = expected<T, DFTUtilsError>;  // tl::expected

Construct the error channel with ``make_error``:

.. code-block:: cpp

   Result<std::size_t> parse_count(std::string_view s) {
       if (s.empty()) {
           return make_error(ErrorCode::INVALID_ARGUMENT, "empty input");
       }
       return s.size();
   }

   auto r = parse_count(text);
   if (!r) {
       LOG_ERROR("%s", r.error().format().c_str());
   } else {
       use(*r);
   }

DFT_TRY
-------

``DFT_TRY`` is a propagation helper for coroutines whose return type is
``Result<T>``. Used as a statement, it evaluates a ``Result``-returning
expression; on failure it ``co_return``\ s the moved-out error, otherwise it
binds the unwrapped moved-out success to the given declaration. This removes
the manual check-and-return boilerplate when chaining fallible steps:

.. code-block:: cpp

   coro::CoroTask<Result<std::size_t>> total_size() {
       DFT_TRY(auto a, co_await read_one());
       DFT_TRY(auto b, co_await read_two());
       co_return a.size() + b.size();
   }

If ``read_one()`` fails, its error is returned immediately and ``read_two()``
is never called.

DFTUtilsException
-----------------

``DFTUtilsException`` derives ``std::runtime_error`` and carries an
``ErrorCode`` retrievable via ``code()``. It is the base of every domain
exception, so a single ``catch`` can inspect the category:

.. code-block:: cpp

   try {
       reader.read_lines(...);
   } catch (const DFTUtilsException& e) {
       if (e.code() == ErrorCode::NOT_FOUND) { /* ... */ }
       LOG_ERROR("%s", e.what());
   }

Two factories build the message without a manual ``std::string`` concat.
``cat`` concatenates typed arguments (numbers go through ``to_chars``, no
format string); ``fmt`` is printf-style:

.. code-block:: cpp

   throw DFTUtilsException::cat(ErrorCode::IO,
                                "Cannot open ", path, ": errno=", e);
   throw DFTUtilsException::fmt(ErrorCode::IO,
                                "Cannot open %s: errno=%d", path.c_str(), e);

Subsystem exceptions
--------------------

The subsystem exceptions derive ``DFTUtilsException`` and fix the
``ErrorCode`` for their domain, so ``catch (const DFTUtilsException&)`` and
``catch (const std::runtime_error&)`` both still work:

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Class
     - ErrorCode
     - Header
   * - ``PipelineError``
     - ``PIPELINE``
     - ``core/pipeline/error.h``
   * - ``reader::ReaderError``
     - ``READER``
     - ``utilities/reader/error.h``
   * - ``indexer::IndexerError``
     - ``INDEXER``
     - ``utilities/indexer/error.h``
   * - ``common::query::QueryParseError``
     - ``QUERY``
     - ``utilities/common/query/parser.h``

Each subsystem exception also carries a finer-grained ``Type`` enum for its
domain (for example ``ReaderError::FILE_IO_ERROR``,
``IndexerError::DATABASE_ERROR``, ``PipelineError::TIMEOUT_ERROR``),
retrievable via ``get_type()`` (``IndexerError`` uses ``type()``). The
``ErrorCode`` from ``code()`` stays fixed for the subsystem; the ``Type``
narrows the cause within it.

rethrow_and_clear
-----------------

For coroutine plumbing that stores a captured failure as a
``std::exception_ptr``, ``rethrow_and_clear`` (in
``<dftracer/utils/core/common/exception_helpers.h>``) moves the stored
exception out, clears the slot, then rethrows. Clearing first prevents
rethrowing the same exception twice if the holder is awaited again:

.. code-block:: cpp

   std::exception_ptr pending;
   // ...
   if (pending) {
       dftracer::utils::rethrow_and_clear(pending);  // [[noreturn]]
   }

Python
------

The Python bindings raise the matching typed exception, mapped from the C++
``ErrorCode``. Every one derives ``DFTUtilsError``, which in turn derives the
built-in ``RuntimeError`` (so existing ``except RuntimeError`` code keeps
working). See :doc:`../quickstart` for a usage example.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Python exception
     - Raised for ErrorCode
   * - ``DFTUtilsError`` (base)
     - ``UNKNOWN``, ``INTERNAL``
   * - ``DFTUtilsValueError``
     - ``INVALID_ARGUMENT``
   * - ``DFTUtilsNotFoundError``
     - ``NOT_FOUND``
   * - ``DFTUtilsIOError``
     - ``IO``
   * - ``DFTUtilsParseError``
     - ``PARSE``
   * - ``DFTUtilsCompressionError``
     - ``COMPRESSION``
   * - ``DFTUtilsQueryError``
     - ``QUERY``
   * - ``DFTUtilsReaderError``
     - ``READER``
   * - ``DFTUtilsIndexerError``
     - ``INDEXER``
   * - ``DFTUtilsPipelineError``
     - ``PIPELINE``
   * - ``DFTUtilsAggregationError``
     - ``AGGREGATION``
