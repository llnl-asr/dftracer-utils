Utilities API
=============

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/index>`.

Composable processing utilities. For usage examples, see :doc:`/utilities`.

Base Classes
------------

All utilities inherit from ``UtilityBase``, which provides tag introspection,
context management, and naming. Two derived templates define the ``process()``
contract:

- ``Utility<I, O, Tags...>`` - materialized output: ``process()`` returns
  ``CoroTask<O>``
- ``StreamingUtility<I, Batch, Tags...>`` - streaming output: ``process()``
  returns ``AsyncGenerator<Batch>``

.. mermaid::

   classDiagram
       class UtilityBase~I, Tags~ {
           +has_tag~Tag~() bool
           +get_tag~Tag~() Tag
           +get_name() string
           +set_name(string)
           #context() CoroScope
       }
       class Utility~I, O, Tags~ {
           +process(I) CoroTask~O~
       }
       class StreamingUtility~I, Batch, Tags~ {
           +process(I) AsyncGenerator~Batch~
       }
       UtilityBase <|-- Utility
       UtilityBase <|-- StreamingUtility

UtilityBase
~~~~~~~~~~~

Shared base for all utilities. Provides tag introspection (``has_tag<>``,
``get_tag<>``), context management (``context()`` for ``NeedsContext``
utilities), and name/type signature generation.

Utility (Materialized)
~~~~~~~~~~~~~~~~~~~~~~

For utilities that compute a single result. ``process(const I&)`` is a pure
virtual returning ``CoroTask<O>`` - the caller ``co_await``\ s the result.
An rvalue overload ``process(I&&)`` is provided automatically (it moves the
input into stable storage for the coroutine frame).

The output type ``O`` is often ``Result<T>`` (an alias for
``expected<T, DFTUtilsError>`` from ``core/common/error.h``), so recoverable
failures travel as a value rather than an exception. Callers unwrap with
``*result`` / ``result.error()`` or propagate with the ``DFT_TRY`` macro:

.. code-block:: cpp

   // Utility<Input, Result<Output>> - failure as a value
   Result<Output> r = co_await util.process(input);
   if (!r) {
       log(r.error().format());
       co_return dftracer::utils::unexpected(std::move(r).error());
   }
   use(*r);

   // Or propagate in one line inside a Result-returning coroutine:
   DFT_TRY(auto value, co_await util.process(input));

StreamingUtility
~~~~~~~~~~~~~~~~

For utilities that yield results incrementally. ``process()`` returns
``AsyncGenerator<Batch>`` - the caller iterates with
``co_await gen.next()``.

Batch structs typically provide a ``to_arrow()`` method for Arrow
conversion (e.g., ``ViewReaderBatch::to_arrow()``,
``AggregationBatch::to_arrow()``).

.. code-block:: cpp

   // Consuming a StreamingUtility
   ViewReaderUtility reader;
   auto gen = reader.process(input);
   while (auto batch = co_await gen.next()) {
       // Use C++ data directly
       for (const auto& event : batch->events) { ... }

       // Or convert to Arrow
       auto arrow = batch->to_arrow();
   }

Tags
----

Tags are compile-time markers appended to a utility's template parameter list.
They are queried with ``has_tag<Tag>()`` and are not stored per instance.

The only tag is ``tags::NeedsContext`` (``core/utilities/tags/needs_context.h``).
A utility carrying it may call ``context()`` to obtain the ``CoroScope`` used to
spawn dynamic sub-tasks. The context must be bound by an executor, a pipeline,
or ``Runtime::scope()`` before ``process()`` runs; calling ``process()``
directly on a ``NeedsContext`` utility throws.

.. code-block:: cpp

   class MyUtility
       : public Utility<MyInput, Result<MyOutput>, tags::NeedsContext> {
      public:
       coro::CoroTask<Result<MyOutput>> process(const MyInput& in) override {
           CoroScope& scope = this->context();  // requires NeedsContext
           auto fut = scope.spawn(/* ... */);
           co_return co_await fut;
       }
   };

.. note::

   Earlier revisions exposed a behavior/tag framework (caching, monitoring,
   retry, parallelization). That framework has been removed; ``NeedsContext``
   is the only remaining tag.

UtilityExecutor
---------------

``behaviors::UtilityExecutor<I, O, Tags...>``
(``core/utilities/utility_executor.h``) runs a utility's ``process()`` and
injects the ``CoroScope`` when the utility needs it. The context-bound
``execute(ctx, input)`` overload sets the context before ``process()`` and
clears it afterward (including on exception).

.. code-block:: cpp

   auto util = std::make_shared<MyUtility>();
   behaviors::UtilityExecutor<MyInput, Result<MyOutput>, tags::NeedsContext>
       executor(util);

   Result<MyOutput> out = co_await executor.execute(scope, input);

BatchProcessorUtility
---------------------

``composites::BatchProcessorUtility<ItemInput, ItemOutput>``
(``utilities/composites/batch_processor_utility.h``) is a ``NeedsContext``
utility that maps ``std::vector<ItemInput>`` to ``std::vector<ItemOutput>`` by
spawning one coroutine per item and joining with ``when_all``. Construct it
from a per-item function or from a re-entrant sub-utility; an optional
``with_comparator()`` sorts the results.

.. code-block:: cpp

   BatchProcessorUtility<std::string, std::size_t> counter(
       [](CoroScope&, const std::string& path) -> std::size_t {
           return count_lines(path);
       });

   // Items run concurrently; the sub-utility/function must not mutate
   // shared instance state.
   auto results = co_await counter.process(paths);

Module Reference
----------------

Each module below has detailed class documentation in the API Reference:

.. list-table::
   :header-rows: 1
   :widths: 30 50 20

   * - Module
     - Description
     - API Reference
   * - Call Tree
     - Build hierarchical call trees from DFTracer traces
     - :doc:`api/call_tree`
   * - Filesystem
     - Directory scanning utilities
     - :doc:`api/utilities/filesystem`
   * - File I/O
     - File reading, writing, chunk writing, async line generators
     - :doc:`api/utilities/fileio/index`
   * - Compression
     - Streaming zlib compression (GZIP, ZLIB, DEFLATE)
     - :doc:`api/utilities/composites/index`
   * - Text
     - Line splitting, filtering, text processing
     - :doc:`api/utilities/text`
   * - Hash
     - FNV1a, std::hash, MT-safe hasher utilities
     - :doc:`api/utilities/hash`
   * - Statistics
     - DDSketch (percentiles), Log2Histogram (distributions)
     - :doc:`api/utilities/composites/dft/statistics`
   * - Indexer
     - Bloom filter indexes, manifests, provenance tracking
     - :doc:`api/utilities/indexer`
   * - Reader
     - Streaming trace file reader with index support
     - :doc:`api/utilities/reader`
   * - Views
     - View definitions and predicate-based event filtering
     - :doc:`api/utilities/composites/dft/views`
   * - Aggregation
     - Time-bucketed aggregation pipeline with Arrow output
     - :doc:`api/utilities/composites/dft/aggregators`
   * - Comparator
     - Baseline vs variant trace comparison with Cohen's d
     - :doc:`api/utilities/composites/dft/comparator`
   * - Reorganization
     - Parallel event routing and chunked output
     - :doc:`api/utilities/composites/dft/reorganize`
   * - Replay
     - Replay I/O operations from traces
     - :doc:`api/utilities/replay`
