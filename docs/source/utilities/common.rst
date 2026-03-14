Common
================

Shared utilities used across the library: JSON parsing and statistics collection.

JSON
----

Lightweight zero-cost wrapper around `yyjson <https://github.com/ibireme/yyjson>`_ for lazy JSON evaluation.

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/json/json.h>
   #include <dftracer/utils/utilities/common/json/json_doc_guard.h>

JsonValue
~~~~~~~~~

Non-owning view over parsed JSON data with fluent navigation and type-safe accessors.

**Parse and navigate:**

.. code-block:: cpp

   yyjson_doc* doc = yyjson_read(json_str.c_str(), json_str.size(), 0);
   JsonDocGuard guard(doc);  // RAII cleanup

   JsonValue root(yyjson_doc_get_root(doc));

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

.. warning::

   ``JsonValue`` is a non-owning view. It is only valid while the ``yyjson_doc`` is alive. Use ``JsonDocGuard`` for RAII lifetime management.

JsonDocGuard
~~~~~~~~~~~~

RAII guard for ``yyjson_doc*`` to prevent leaks on exceptions or early coroutine returns.

.. code-block:: cpp

   {
       yyjson_doc* doc = yyjson_read(data, len, 0);
       JsonDocGuard guard(doc);

       JsonValue root(yyjson_doc_get_root(doc));
       // ... use root ...
   }  // guard destructor frees doc

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

Statistics
----------

Percentile estimation and histogram utilities for trace analysis.

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/statistics/ddsketch.h>
   #include <dftracer/utils/utilities/common/statistics/log2_histogram.h>

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

See Also
--------

- :doc:`composites` - Composites that use DDSketch and Log2Histogram for chunk statistics
- :doc:`/cpp_api/index` - Full C++ API documentation
