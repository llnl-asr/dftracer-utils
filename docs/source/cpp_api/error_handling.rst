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
