Common
======

Shared utilities used across the library: JSON parsing, query language,
statistics collection, and Arrow data interchange.

JSON
----

JSON parsing uses `simdjson <https://github.com/simdjson/simdjson>`_ exclusively (DOM and On-Demand APIs). ``JsonValue`` is a lightweight wrapper around ``simdjson::dom::element``; ``JsonParser`` exposes the On-Demand API for zero-copy lazy field access.

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/json/json.h>
   #include <dftracer/utils/utilities/common/json/json_value.h>
   #include <dftracer/utils/utilities/common/json/parser.h>

JsonValue
~~~~~~~~~

Wrapper over ``simdjson::dom::element`` with fluent navigation and type-safe accessors. Non-owning: only valid while the backing ``simdjson::dom::document`` is alive.

**Parse and navigate:**

.. code-block:: cpp

   simdjson::dom::parser parser;
   simdjson::dom::element doc = parser.parse(json_str);
   JsonValue root(doc);

   // Fluent navigation with defaults
   std::string name = root["metadata"]["name"].get<std::string>("unknown");
   uint64_t id = root["id"].get<uint64_t>(0);
   double value = root["data"]["value"].get<double>(1.0);

**Dot-path navigation:**

.. code-block:: cpp

   auto nested = root.at("metadata.config.timeout");
   if (nested.exists()) {
       int timeout = nested.get<int>(30);
   }

**Optional access:**

.. code-block:: cpp

   if (auto val = root["optional_field"].get_optional<int64_t>()) {
       use(*val);
   }

**Type checking:**

.. code-block:: cpp

   JsonValue val = root["field"];
   if (val.is_string()) { /* ... */ }
   if (val.is_number()) { /* ... */ }
   if (val.is_object()) { /* ... */ }
   if (val.is_array())  { /* ... */ }
   if (val.exists())    { /* not null */ }

JsonDocGuard
~~~~~~~~~~~~

RAII helper that owns a ``simdjson::dom::parser``; ``parse(data, len)`` reuses
the parser buffer and ``root()`` returns the parsed element. Use across
short-lived parse sites; ``StringJsonParserUtility`` is preferred when the
document must outlive a ``co_await`` boundary.

.. code-block:: cpp

   JsonDocGuard guard;
   if (guard.parse(data, len)) {
       JsonValue root(guard.root());
       // ... use root ...
   }

JsonParser (On-Demand)
~~~~~~~~~~~~~~~~~~~~~~

On-Demand parser for zero-copy lazy field access. Reuses an internal padded
buffer across rows; ``string_view`` results are valid until the next
``parse()`` call. Used by the indexing visitors and ``TraceReader::read_json``.

.. code-block:: cpp

   JsonParser parser;

   for (auto& line : input_lines) {
       if (!parser.parse(line)) continue;
       auto name = parser.get_string("name");
       auto ts   = parser.get_int64("ts");

       parser.for_each_field("args", [](std::string_view k,
                                        simdjson::ondemand::value v) {
           // process nested fields
       });
   }

StringJsonParserUtility
~~~~~~~~~~~~~~~~~~~~~~~

Parses JSON strings with owned document lifetime. Safe for use across ``co_await`` boundaries.

.. code-block:: cpp

   StringJsonParserUtility parser;

   // From string
   auto input = StringJsonParserInput::from_string(R"({"x": 42})");
   auto json = co_await parser.process(input);
   int x = json["x"].get<int>(0);

   // From file (async)
   auto input = co_await StringJsonParserInput::from_file_async("config.json");
   auto json = co_await parser.process(input);

   parser.reset();  // Cleanup

ArgsMap and ArgsValueProxy
~~~~~~~~~~~~~~~~~~~~~~~~~~

Owned key/value map used for trace event ``args``. Replaces ``JsonValue`` for
event-args storage in the DFT composites: keys are interned with the global
:cpp:class:`dftracer::utils::StringIntern` and values are a typed
``std::variant`` (string, int64, uint64, double, bool). ``ArgsValueProxy``
mirrors the ``JsonValue`` accessor surface (``get<T>``, ``get_optional<T>``,
``is_string()`` ...) so event visitors can be written generically.

.. code-block:: cpp

   #include <dftracer/utils/utilities/composites/dft/args_map.h>

   using dftracer::utils::utilities::composites::dft::ArgsMap;

   ArgsMap args;
   args.insert("hhash", std::uint64_t{0x1234});
   args.set_valid(true);

   uint64_t h = args["hhash"].get<uint64_t>(0);
   args.for_each_member([](std::string_view k, auto v) { /* ... */ });

JsonDictValue (Python)
~~~~~~~~~~~~~~~~~~~~~~

Python-facing wrapper that exposes a parsed JSON object as a lazy
``Mapping``. Used by the ``TraceReader`` Python binding to surface parsed
events without materialising a ``dict`` per row. Defined in
``src/dftracer/utils/python/json.h``.

.. note::

   :cpp:class:`dftracer::utils::StringIntern` was reimplemented as a
   lock-free open-chained hash table with a fast-path id table
   (``FAST_CAPACITY = 1<<20``). Lookups are fully lock-free; only the rare
   first insert of a string takes the insertion mutex.

Query
-----

Query DSL for filtering JSON events. Recursive descent parser, AST evaluator,
and ``Query`` class that owns a parsed AST.

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/query/query.h>

Query Class
~~~~~~~~~~~

Owns a parsed query AST. Provides evaluation against ``JsonValue`` (for JSON
events) and ``ValueMap`` (for typed key-value maps).

.. code-block:: cpp

   using namespace dftracer::utils::utilities::common::query;

   // Parse and evaluate against JSON
   auto q = parse_or_throw(R"(cat == "POSIX" and dur > 1000)");
   JsonValue event = ...;
   if (q.evaluate(event)) { /* match */ }

   // Evaluate against a typed map (no JSON needed)
   ValueMap fields = {{"cat", std::string("POSIX")}, {"dur", uint64_t(2000)}};
   if (q.evaluate(fields)) { /* match */ }

   // Safe parsing with error handling
   auto result = Query::from_string(R"(cat in ["POSIX", "STDIO"])");
   if (!result) {
       std::cerr << result.error().format();  // formatted error with column indicator
   }

Parser
~~~~~~

Tokenizes and parses query DSL strings into an AST. Keywords (``and``, ``or``,
``not``, ``in``, ``true``, ``false``) are case-insensitive. String values are
case-sensitive.

.. code-block:: cpp

   // Tokenize
   auto tokens = tokenize(R"(cat == "POSIX" AND dur > 1000)");

   // Parse tokens into AST
   auto ast = parse(R"(name in ["read", "write"] or not cat == "MPI")");

Evaluator
~~~~~~~~~

Evaluates an AST node against a ``JsonValue`` or ``ValueMap``. Missing fields
and type mismatches evaluate to false.

.. code-block:: cpp

   // Direct AST evaluation (Query::evaluate wraps this)
   bool match = evaluate(*ast, json_event);
   bool match2 = evaluate(*ast, value_map);

AST Types
~~~~~~~~~

The query AST uses ``std::variant``-based nodes:

- ``CompareNode`` — field comparison (``==``, ``!=``, ``>``, ``<``, ``>=``, ``<=``)
- ``InNode`` / ``NotInNode`` — set membership
- ``AndNode`` / ``OrNode`` — logical connectives
- ``NotNode`` — logical negation

.. code-block:: cpp

   // Programmatic AST construction
   auto node = make_node(CompareNode{
       FieldNode{"cat"}, CompareOp::EQ, LiteralNode{std::string("POSIX")}});

   // AST to string (round-trip)
   std::string s = to_string(*node);  // cat == "POSIX"

Statistics
----------

Percentile estimation and histogram utilities for trace analysis.

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/statistics/ddsketch.h>
   #include <dftracer/utils/utilities/common/statistics/log2_histogram.h>
   #include <dftracer/utils/utilities/common/statistics/timestamp_histogram.h>

DDSketch
~~~~~~~~

Deterministic, merge-order-independent percentile estimation with bounded relative error.

**Basic usage:**

.. code-block:: cpp

   DDSketch sketch(0.01);  // 1% relative accuracy

   for (double duration : durations) {
       sketch.add(duration);
   }

   double p50 = sketch.quantile(0.5);
   double p95 = sketch.quantile(0.95);
   double p99 = sketch.quantile(0.99);

   printf("p50=%.1f p95=%.1f p99=%.1f (n=%lu)\n",
          p50, p95, p99, sketch.count());

**Merging sketches from distributed sources:**

.. code-block:: cpp

   DDSketch sketch_a(0.01);
   DDSketch sketch_b(0.01);

   // ... populate independently ...

   // Merge is commutative and associative
   sketch_a.merge(sketch_b);
   // sketch_a now contains combined data

**Serialization:**

.. code-block:: cpp

   auto bytes = sketch.serialize();
   DDSketch restored = DDSketch::deserialize(bytes.data(), bytes.size());

Log2Histogram
~~~~~~~~~~~~~

Fixed 65-bin logarithmic histogram covering the ``uint64_t`` range. Bin 0 holds value 0, bin *k* (1-64) holds values in [2^(k-1), 2^k).

**Basic usage:**

.. code-block:: cpp

   Log2Histogram hist;

   for (uint64_t duration_us : durations) {
       hist.add(duration_us);
   }

   double p50 = hist.approx_percentile(0.5);
   double p99 = hist.approx_percentile(0.99);

**Visualization:**

.. code-block:: cpp

   // ASCII bar chart
   std::cout << hist.render_ascii(50, "us");
   //     [0, 0) us                |# 1
   //     [64, 128) us             |############ 100

   // Unicode block elements
   std::cout << hist.render_blocks(50, "us");

**Merging and serialization:**

.. code-block:: cpp

   Log2Histogram a, b;
   // ... populate ...
   a.merge(b);  // Commutative

   std::string json = a.to_json();
   Log2Histogram restored = Log2Histogram::from_json(json);

TimestampHistogram
~~~~~~~~~~~~~~~~~~

Sparse fixed-width (100 ms) histogram over event timestamps. Used by the
chunk pruner to compute time-range selectivity and to weight sub-bucket
expansions for adaptive aggregation.

.. code-block:: cpp

   TimestampHistogram th;
   for (auto ts_us : timestamps) th.add(ts_us);

   std::uint64_t in_window = th.count_in_range(ts_lo, ts_hi);
   double sel = th.selectivity(ts_lo, ts_hi);

   auto bytes = th.serialize();
   auto restored = TimestampHistogram::deserialize(bytes.data(), bytes.size());

Arrow
-----

Arrow data interchange via `nanoarrow <https://github.com/apache/nanoarrow>`_.
Guarded by ``DFTRACER_UTILS_ENABLE_ARROW`` (ON by default).

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/arrow/arrow.h>

RecordBatchBuilder
~~~~~~~~~~~~~~~~~~

Type-safe columnar builder with two modes:

- **Static schema** — ``declare_schema()`` upfront, direct index append.
  Best for utility ``to_arrow()`` methods with known column layouts.
- **Dynamic schema** — ``add_or_get_column()`` discovers columns from data,
  ``end_row()`` backfills nulls. Best for arbitrary JSON (e.g.,
  ``TraceReader.iter_arrow()``).

Column types: ``INT64``, ``UINT64``, ``DOUBLE``, ``STRING``, ``BOOL``.

String columns store ``string_view`` into source data — zero copies during
build, bulk copy only at ``finish()``. Caller must keep source data alive
until ``finish()`` returns.

**Static schema example:**

.. code-block:: cpp

   RecordBatchBuilder builder;
   builder.declare_schema({
       {"id", ColumnType::INT64},
       {"name", ColumnType::STRING},
       {"value", ColumnType::DOUBLE},
   });
   builder.reserve(1000);

   builder.append_int64(0, 42);
   builder.append_string(1, "hello");
   builder.append_double(2, 3.14);
   builder.end_row();

   auto result = builder.finish();  // ArrowExportResult
   // result.num_rows() == 1, result.num_columns() == 3

   builder.reset(true);  // keep schema, clear data for next batch

**Dynamic schema example:**

.. code-block:: cpp

   RecordBatchBuilder builder;

   // Columns discovered from data
   auto col_x = builder.add_or_get_column("x", ColumnType::INT64);
   builder.append_int64(col_x, 1);
   builder.end_row();  // row 0: x=1

   auto col_y = builder.add_or_get_column("y", ColumnType::STRING);
   // col_y is new — backfills null for row 0
   builder.append_int64(col_x, 2);
   builder.append_string(col_y, "hello");
   builder.end_row();  // row 1: x=2, y="hello"

   auto result = builder.finish();
   // result.num_rows() == 2, result.num_columns() == 2
   // column "y" has null in row 0

ArrowExportResult
~~~~~~~~~~~~~~~~~

Move-only RAII wrapper holding ``nanoarrow::UniqueSchema`` +
``nanoarrow::UniqueArray``. Self-contained, safe to send across threads
and channels.

.. code-block:: cpp

   auto result = builder.finish();
   assert(result.valid());
   assert(result.num_rows() == 100);
   assert(result.num_columns() == 5);

   // Access raw Arrow C Data Interface pointers
   ArrowSchema* schema = result.get_schema();
   ArrowArray* array = result.get_array();

   // Move ownership out
   auto schema = result.release_schema();
   auto array = result.release_array();

IpcWriter
~~~~~~~~~

Streaming Arrow IPC file writer (``.arrows`` format). Writes files
readable by pyarrow, polars, DuckDB, and any Arrow-compatible tool.

Guarded by ``DFTRACER_UTILS_ENABLE_ARROW_IPC``.

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/arrow/ipc_writer.h>

   IpcWriter writer;
   writer.open("output.arrows");

   // Stream batches — schema written on first write_batch()
   for (auto& batch : batches) {
       auto arrow = batch.to_arrow();
       writer.write_batch(arrow);
   }

   writer.close();  // writes footer, closes file

Batch to_arrow() Pattern
~~~~~~~~~~~~~~~~~~~~~~~~~

Streaming utilities yield batch structs with a ``to_arrow()`` method.
This keeps Arrow conversion in C++ (not the Python binding) and allows
both C++ and Python consumers to use Arrow output.

.. code-block:: cpp

   // ViewReaderBatch, AggregationBatch both follow this pattern
   struct MyBatch {
       std::vector<std::string> events;

       ArrowExportResult to_arrow() const {
           RecordBatchBuilder builder;
           // ... parse events, append columns ...
           return builder.finish();
       }
   };

   // Consuming a StreamingUtility with Arrow output
   MyStreamingUtility util;
   auto gen = util.process(input);
   while (auto batch = co_await gen.next()) {
       auto arrow = batch->to_arrow();
       ipc_writer.write_batch(arrow);
   }

See Also
--------

- :doc:`composites` - Composites that use DDSketch, Log2Histogram, and Query for chunk statistics and filtering
- :doc:`/cpp_api/dft_indexing` - ChunkPrunerUtility and ChunkDimensionStats that use Query for chunk skipping
- :doc:`/cpp_api/arrow` - Full C++ API reference for Arrow classes
- :doc:`/cpp_api/index` - Full C++ API documentation
