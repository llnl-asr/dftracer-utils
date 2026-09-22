:description: Extend trace analysis with a compiled plugin that rides the fused scan, authored with the C++ SDK or the raw C ABI, shown side by side.

Plugins
=======

.. seealso::

   :doc:`cpp_api/plugins` for the generated plugin C ABI reference, :doc:`jit`
   to author a plugin in Python, and :doc:`tutorials/write-a-c-plugin` for a
   compiled-and-run C++ walkthrough.

A plugin extends trace analysis with your own compiled code, loaded at runtime
from a shared library. It rides the same fused scan the built-in analytics use:
N plugins share one traversal that parses each event once, so
``--plugin a --plugin b`` decompresses and parses the trace a single time.

There are two co-equal ways to author one:

- **The C++ SDK** (``<dftracer/utils/plugins/plugin.h>``, namespace
  ``dftracer::utils::plugins``): a plain ``Slice`` struct with ``step`` / ``merge`` /
  ``finalize`` methods, assembled with ``plugin(h, config).fold<Slice>().build()``
  (or ``make_plugin<Slice>(config)`` directly, what ``fold`` calls underneath).
  Typed ``Batch`` / ``Event`` views, host-owned ``Agg`` accumulators, and an
  RAII ``Host`` read like ordinary C++.
- **The raw C ABI** (``<dftracer/utils/plugins/abi.h>``): a small set of C
  structs you fill by hand. This is the stable boundary the SDK compiles down
  to, and the path for a C-only toolchain.

Both compile to the same loadable ``.so`` and link nothing from the internal
library, so a plugin stays compatible across releases. Every section below shows
the C++ and the C form side by side; the SDK is a header-only, zero-cost wrapper
over the ABI, so any SDK call maps to one ABI call underneath.

.. contents:: On this page
   :local:
   :depth: 1

Overview
--------

- **A plugin is a Fold**: it sees each batch of parsed events and folds them
  into its own state. There is no per-event callback overhead and no threading
  code - the host runs one fold slice per worker and merges the results.
- **State is host-owned where it can be**: a mergeable aggregation accumulator
  is owned and merged by the host, so parallelism and the final materialization
  are free and ``merge`` stays empty.
- **The fold is columnar**: each batch arrives as one ``dftu_dataframe`` (N
  rows in scan order is N events); the ``Batch``/``Event`` row cursor resolves
  every fixed column once per batch, so a string field is a zero-copy
  ``string_view`` into the frame's own buffer with nothing copied on the hot
  path.
- **Services are optional extension groups**: aggregation, async I/O, the query DSL,
  Arrow, writers, sketches, and inter-plugin channels are fetched by id; a
  missing group degrades gracefully to a null/no-op.

1. Entry point and lifecycle
----------------------------

A plugin is a shared library exporting one symbol, ``dftracer_plugin`` (the
``DFTRACER_PLUGIN_FACTORY_SYMBOL`` the loader resolves via ``dlsym``). It
receives the parsed config tree (NULL when none given) and returns a
``dftu_plugin``. The host then drives that plugin as a data-parallel fold:

.. mermaid::

   graph LR
       make["make_slice /<br/>Slice(Config)<br/>(per worker)"] --> step["on_batch / step<br/>(per batch)"]
       step --> step
       step --> merge["merge<br/>(fold slices together)"]
       merge --> fin["on_finalize / finalize<br/>(emit output)"]
       fin --> destroy["destroy_slice /<br/>destroy"]

Each stage maps one-to-one between the SDK and the ABI:

.. list-table::
   :header-rows: 1
   :widths: 34 34 32

   * - ``dftracer::utils::plugins`` Slice
     - ``dftu_plugin`` callback
     - When
   * - ``Slice(const Config&)``
     - ``make_slice``
     - once per worker slice
   * - ``step`` / ``on_batch``
     - ``on_batch``
     - once per event batch
   * - ``merge``
     - ``merge``
     - to fold two slices into one
   * - ``finalize`` / ``on_finalize``
     - ``on_finalize``
     - once on the merged slice, to emit
   * - destructor
     - ``destroy_slice`` / ``destroy``
     - teardown
   * - ``static ... reads()``
     - ``reads``
     - queried before the scan (section 2)
   * - config ``"query"`` key
     - ``plan_query``
     - queried before the scan (section 3)
   * - ``transform``
     - ``transform``
     - in place of ``step``: rewrites the batch for every later plugin
       (:doc:`guides/plugins/inter-plugin-comms`)
   * - ``bytes() const`` / ``reclaim(want)``
     - ``bytes`` / ``reclaim``
     - after a step or merge past the memory budget
       (:doc:`guides/runtime/memory-budget`)
   * - ``static ... provides()`` / ``consumes()``
     - ``provides`` / ``consumes``
     - queried before the scan, settles the fold order (section 8)
   * - ``static ... config_keys()``
     - ``config_keys``
     - queried at load, validates the config (section 4)

``on_batch`` returns ``NULL`` (or nothing, for the sync SDK ``step``) since it
is always synchronous; ``on_finalize`` returns ``NULL`` when handled
synchronously, or a ``dftu_task`` the host awaits for async work (section 9).
The delivered ``dftu_dataframe`` is owned by the host and valid only for the
call - N rows in scan order is N events, never retain the pointer.

``DFTRACER_PLUGIN_ABI_VERSION`` is not a number anyone bumps: it is a hash
over the ABI headers, computed when the host is built. A plugin compiled
against any other version of the headers is refused at load with a version
message, never called into. Rebuild the plugin against the host you run.

.. tab-set::

   .. tab-item:: C++ (SDK)

      A ``Slice`` provides the constructor and methods; ``plugin(h,
      config).fold<Slice>().build()`` fills the ``dftu_plugin`` struct and
      adapts exceptions at the boundary (every callback catches and logs).
      ``abi_version`` is set for you.

      .. code-block:: cpp

         #include <dftracer/utils/plugins/plugin.h>

         using namespace dftracer::utils::plugins;

         struct MyPlugin {
             explicit MyPlugin(const Config&) {}   // per-worker state

             void step(const Batch& b, Host h) {   // fold one batch
                 (void)b; (void)h;
             }
             void merge(MyPlugin&) {}               // combine two slices
             void finalize(Host) {}                 // emit output
         };

         extern "C" dftu_plugin* dftracer_plugin(dftu_plugin_host* h,
                                                 const dftu_value* config) {
             return plugin(h, config).fold<MyPlugin>().build();
         }

   .. tab-item:: C (raw ABI)

      Fill the ``dftu_plugin`` struct by hand. ``abi_version`` must be set to
      ``DFTRACER_PLUGIN_ABI_VERSION``; ``self`` is the read-only config shared
      across slices.

      .. code-block:: c

         #include <dftracer/utils/plugins/abi.h>
         #include <stdlib.h>

         static const char* plan_query(void* self) { (void)self; return NULL; }
         static void* make_slice(void* self) { (void)self; return calloc(1, 1); }
         static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
                                   const dftu_plugin_host* host) {
             (void)slice; (void)df; (void)host; return NULL;  /* synchronous */
         }
         static void merge(void* into, void* other) { (void)into; (void)other; }
         static dftu_task* on_finalize(void* s, const dftu_plugin_host* h) {
             (void)s; (void)h; return NULL;
         }
         static void destroy_slice(void* slice) { free(slice); }
         static void destroy(void* self) { (void)self; }

         static dftu_plugin g_plugin;

         dftu_plugin* dftracer_plugin(dftu_plugin_host* h, const dftu_value* config) {
             (void)h; (void)config;
             g_plugin.abi_version   = DFTRACER_PLUGIN_ABI_VERSION;
             g_plugin.self          = NULL;
             g_plugin.plan_query    = plan_query;
             g_plugin.make_slice    = make_slice;
             g_plugin.on_batch      = on_batch;
             g_plugin.merge         = merge;
             g_plugin.on_finalize   = on_finalize;
             g_plugin.destroy_slice = destroy_slice;
             g_plugin.destroy       = destroy;
             return &g_plugin;
         }

2. Declaring the batch columns you read (reads)
------------------------------------------------

Undeclared, the host materializes the whole batch for every plugin: the seven
fixed columns, ``fhash`` / ``hhash``, and one column per distinct arg key seen
in the batch - unbounded, since a trace with fifty arg keys builds fifty
columns even for a plugin that only reads ``dur``. A plugin that declares
``reads`` gets only those columns; every other column is simply absent from the
frame, so a lookup for it returns NULL. Names are batch column names as
``on_batch`` sees them: ``"dur"``, ``"cat"``, ``"fhash"``, an arg as
``"args.<key>"``, a virtual field as ``"resolved.fpath"``. A plugin that
registers a state (section 6) is exempt - its state is handed the same frame
regardless, so it always keeps every column.

.. tab-set::

   .. tab-item:: C++ (SDK)

      Add a ``static ... reads()`` returning a range of ``const char*`` column
      names outliving the plugin; the SDK wires it to ``dftu_plugin::reads``.
      Omit it to keep every column.

      .. code-block:: cpp

         struct MyPlugin {
             static auto reads() {
                 return std::array<const char*, 2>{"dur", "cat"};
             }

             explicit MyPlugin(const Config&) {}
             void step(const Batch& b, Host) {
                 for (const Event& e : b) { /* e.dur(), e.cat() are populated */ }
             }
             void merge(MyPlugin&) {}
             void finalize(Host) {}
         };

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         static const char* const g_reads[] = {"dur", "cat", NULL};
         static const char* const* reads(void* self) { (void)self; return g_reads; }

         /* g_plugin.reads = reads; */

3. Coarse predicate pushdown (plan_query)
-----------------------------------------

A plugin can narrow the shared scan to the chunks that can possibly match, using
the same query DSL the analytics use (see :doc:`guides/core/query-dsl`). The
host uses the returned predicate to prune index chunks before the fold ever sees
them; it is a coarse filter, so a plugin still checks per event if it needs
exactness. Returning nothing scans everything.

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``make_plugin`` reads the pushdown predicate from the plugin's ``"query"``
      config value, so set it with ``--parg query=...`` (section 4) or a config
      file. There is no separate ``Slice`` method for it.

      .. code-block:: bash

         dftracer_run --plugin ./p.so --parg query='cat == "POSIX"' \
                      --files trace.pfw.gz

   .. tab-item:: C (raw ABI)

      Implement ``plan_query`` to return any DSL string (scan-lifetime storage),
      or NULL for no filter.

      .. code-block:: c

         static const char* plan_query(void* self) {
             (void)self;
             return "cat == \"POSIX\"";
         }

4. Configuration and arguments
------------------------------

``dftracer_run`` layers a JSON config tree per plugin block and passes it to the
factory. On the command line, arguments attach to the ``--plugin`` they follow:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Flag
     - Effect
   * - ``--parg key=value``
     - Set one config key for the preceding ``--plugin`` block.
   * - ``--pconfig file.json``
     - Merge a JSON config file into the preceding block.
   * - ``--shared-parg key=value``
     - Set a key on every plugin block.
   * - ``--shared-pconfig file.json``
     - Merge a JSON file into every block.

Layering is low to high: shared config file, shared args, block config file,
block args (later wins). A plugin reads the resulting tree from its factory.

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``Config`` (``plugin.h``) is a typed view over the tree; a ``Slice`` may
      keep returned ``string_view``s since the host-owned tree outlives it.
      Accessors: ``get`` (string), ``get_int``, ``get_double``, ``get_bool``,
      ``child`` (nested object), ``array`` / ``get_int_array`` /
      ``get_double_array`` / ``get_string_array``.

      .. code-block:: cpp

         struct MyPlugin {
             std::int64_t threshold_;
             std::string label_;

             explicit MyPlugin(const Config& cfg)
                 : threshold_(cfg.get_int("threshold", 0)),
                   label_(cfg.get("label", "events")) {}

             void step(const Batch&, Host) {}
             void merge(MyPlugin&) {}
             void finalize(Host) {}
         };

   .. tab-item:: C (raw ABI)

      The tree is a ``dftu_value`` passed to the factory; ``self`` should carry
      whatever the slices need. Read it with the inline coercions
      ``dftu_obj_get`` / ``dftu_as_i64`` / ``dftu_as_f64`` / ``dftu_as_bool`` /
      ``dftu_as_str``.

      .. code-block:: c

         dftu_plugin* dftracer_plugin(dftu_plugin_host* h, const dftu_value* config) {
             (void)h;
             int64_t threshold = dftu_as_i64(dftu_obj_get(config, "threshold"), 0);
             /* stash `threshold` in a static/self the slices can read */
             (void)threshold;
             /* ... fill g_plugin ... */
             return &g_plugin;
         }

      Declaring ``config_keys`` (a ``dftu_config_key`` array terminated by a
      NULL-name entry, or a Slice's ``static ... config_keys()`` in the SDK)
      makes the host validate the config before the plugin runs: an undeclared
      key, a key of the wrong kind, or a missing required key each fail the
      load, instead of a typo silently doing nothing. An undeclared plugin's
      config is not validated at all.

5. Events and batches
---------------------

``on_batch`` / ``step`` receives one batch as a ``dftu_dataframe`` - N rows in
scan order is N events, one column per field. The SDK's ``Batch`` and ``Event``
are a zero-copy row cursor over that frame, valid only for the call that
delivered it; string fields resolve to ``std::string_view``\ s straight from the
frame's own buffers, no interned-id round trip needed at the row level. Every
``Event`` accessor:

.. list-table::
   :header-rows: 1
   :widths: 30 15 55

   * - ``Event`` (SDK)
     - Batch column
     - Meaning
   * - ``pid()`` / ``tid()``
     - ``pid`` / ``tid``
     - process / thread id (``uint64``)
   * - ``ts()`` / ``dur()``
     - ``ts`` / ``dur``
     - start timestamp / duration
   * - ``has_dur()``
     - -
     - ``phase() == Phase::Complete`` (the row-fold engine tracks no per-row
       null for ``dur``)
   * - ``phase()``
     - ``ph``
     - ``Phase``: ``Unknown`` / ``Complete`` / ``Counter`` / ``Aggregated`` /
       ``Metadata``
   * - ``has_cat()`` / ``has_name()`` / ``has_fhash()`` / ``has_hhash()``
     - ``cat`` / ``name`` / ``fhash`` / ``hhash``
     - whether the column is present in this batch for this row
   * - ``cat()`` / ``name()`` / ``fhash()`` / ``hhash()``
     - ``cat`` / ``name`` / ``fhash`` / ``hhash``
     - zero-copy ``string_view`` into the frame; empty when absent
   * - ``has_arg(key)`` / ``arg_is_i64/f64/str(key)``
     - ``args.<key>``
     - whether a dyn arg column exists and its type, by dotted key
   * - ``arg_i64/f64/str(key)``
     - ``args.<key>``
     - the arg value for this row

A plugin working straight off the columns (no per-row cursor) reads the
``dftu_dataframe`` with the dataframe C ABI
(``dftracer/utils/dataframe/abi.h``) - ``dftu_dataframe_column`` by name,
``dftu_series_type`` / ``dftu_series_data``.

.. tab-set::

   .. tab-item:: C++ (SDK)

      .. code-block:: cpp

         void step(const Batch& b, Host) {
             for (const Event& e : b) {
                 if (!e.has_name()) continue;
                 std::string_view cat = e.cat();          // zero-copy view
                 std::uint64_t dur = e.has_dur() ? e.dur() : 0;
                 if (e.arg_is_i64("ret")) { /* e.arg_i64("ret") */ }
             }
         }

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
                                   const dftu_plugin_host* host) {
             (void)slice;
             int64_t n = dftu_dataframe_num_rows(df);
             dftu_series* ph_col = dftu_dataframe_column(df, "ph");
             const int64_t* ph = (ph_col && dftu_series_type(ph_col) == DFTU_TYPE_INT64)
                 ? (const int64_t*)dftu_series_data(ph_col) : NULL;
             int64_t i;
             if (ph)
                 for (i = 0; i < n; ++i)
                     if (ph[i] == DFTU_PH_COMPLETE) { /* row i has a duration */ }
             if (ph_col) dftu_series_free(ph_col);
             (void)host;
             return NULL;
         }

6. Mergeable aggregation (DFTU_SVC_AGG)
---------------------------------------

A host-owned accumulator is the usual way a plugin accumulates a result. It
wraps the dataframe engine's ``AggState``: you name the key columns to group by
and the aggregates to compute, then fold whole column batches into it. The host
merges same-named accumulators across all worker slices and finalizes each at
scan end to a dataframe (the key columns, then one column per aggregate),
returned under the accumulator name. Because the accumulator is write-only and
host-merged, ``merge`` stays empty and there is nothing to free.

This is the one accumulator surface: a keyed map is an accumulator **with** key
columns, and a scalar handle is one with **zero** key columns.

**The seam.** ``on_batch`` already hands each batch across as a
``dftu_dataframe``, so an accumulator that eats columns is fed straight from
it - no separate per-event callback to opt into. The batch carries ``name``,
``cat``, ``pid``, ``tid``, ``ts``, ``dur``, ``ph``, ``fhash`` / ``hhash`` when
any event has one, and one ``args.<key>`` column per arg key present (or only
the columns a plugin declared via ``reads``, section 2).

**The spec.** One aggregate is a ``dftu_agg_col``: an op code (a ``DFTU_AGG_*``
value of ``dftu_agg_op``), the ``value`` column read in each batch (NULL for
``DFTU_AGG_COUNT``, the group row count), the ``out`` result column name, a
scalar ``param``, and a second input column ``by``. ``param`` carries the
quantile for ``PCT``, k for ``TOPK`` / ``BOTTOMK`` / ``APPROX_TOPK`` /
``SAMPLE`` / ``DISTINCT``, and the endpoint-snap tolerance for the occupancy
ops. ``by`` carries the ordering column for ``ARGMAX`` / ``ARGMIN`` /
``LIST_SORTED`` / ``TOPK`` / ``BOTTOMK``, x for the co-moment ops, and dur for
the occupancy ops.

**The op table** (``dftracer/utils/dataframe/agg_op_codes.h``):

- Counting: ``COUNT`` (group rows), ``COUNT_VALID`` (non-null values of the
  value column), ``DISTINCT`` (approximate distinct count, a KMV sketch).
- Numeric: ``SUM``, ``SUMSQ``, ``MIN``, ``MAX``, ``MEAN``, ``VAR``, ``STD``,
  ``SKEW``, ``KURT``, ``BIT_OR``, ``PROD`` (Float64; 1 for a group with no
  present value).
- Positional: ``FIRST``, ``LAST``, ``ARGMAX`` / ``ARGMIN`` (the value at the
  row maximizing / minimizing ``by``).
- Distributional: ``PCT`` (a mergeable DDSketch quantile), ``HIST`` (the whole
  DDSketch histogram as a ``list<struct{lo, hi, count}>`` column).
- Collections: ``SET_UNION`` (distinct values, sorted and joined),
  ``LIST_SORTED`` (a ``list<string>`` ordered by ``by``), ``TOPK`` /
  ``BOTTOMK`` (bounded ``list<string>``), ``APPROX_TOPK`` (heavy hitters, a
  ``list<struct{value, count}>``), ``SAMPLE`` (a deterministic
  bottom-k-by-hash ``list<string>``).
- Co-moments over ``(x = by, y = value)``: ``CORR``, ``COVAR_POP`` /
  ``COVAR_SAMP``, ``REGR_SLOPE`` / ``REGR_INTERCEPT`` / ``REGR_R2``.
- Occupancy over ``(ts = value, dur = by)``: ``BUSY`` (interval-union length),
  ``CONCURRENCY``, ``UTILIZATION``, ``ACTIVE`` (peak overlap depth).

Keyed accumulator
~~~~~~~~~~~~~~~~~

Count events and total their duration per ``(pid, event-name)``:

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``Host::agg`` takes the name, the key column names and the aggregates,
      and returns a non-owning ``Agg``; the ``agg::`` factories build each
      ``AggCol`` with exactly the fields its op uses, so a bad combination does
      not compile.

      .. code-block:: cpp

         void step(const Batch& b, Host h) {
             const auto edges = h.agg("name_edges", {"pid", "name"},
                                      {agg::count("edges"),
                                       agg::sum("dur", "dur_sum")});
             if (edges) edges.accumulate(b.raw());
         }

   .. tab-item:: C (raw ABI)

      Fetch ``DFTU_SVC_AGG``, ``agg_new`` with the key names and the spec
      array, then ``agg_accumulate`` the batch. ``agg_new`` is get-or-create,
      so calling it every batch is the normal shape.

      .. code-block:: c

         static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
                                   const dftu_plugin_host* host) {
             (void)slice;
             const dftu_svc_agg* agg =
                 (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);
             static const char* const keys[2] = {"pid", "name"};
             static const dftu_agg_col specs[2] = {
                 {DFTU_AGG_COUNT, NULL, "edges", 0.0, NULL},
                 {DFTU_AGG_SUM, "dur", "dur_sum", 0.0, NULL}};
             dftu_agg* a;
             if (!agg || !agg->agg_new) return NULL;
             a = agg->agg_new(host->h, "name_edges", keys, 2, specs, 2);
             if (a) agg->agg_accumulate(host->h, a, df);
             return NULL;
         }

A batch missing any referenced key or value column is skipped whole rather than
folded under a partial key.

Scalar accumulator
~~~~~~~~~~~~~~~~~~

Pass zero key columns and the whole scan is one group, which is the AggState
form of a scalar handle:

.. code-block:: c

   static const dftu_agg_col specs[3] = {
       {DFTU_AGG_COUNT, NULL, "count", 0.0, NULL},
       {DFTU_AGG_PCT, "dur", "p50", 0.5, NULL},
       {DFTU_AGG_MAX, "dur", "max", 0.0, NULL}};
   dftu_agg* a = agg->agg_new(host->h, "com.example.dur_stats", NULL, 0, specs, 3);

Reading another plugin's result
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``agg_result(name)`` returns the cross-worker-merged, finalized result of any
plugin's accumulator as a new owned dataframe (free it with
``dftu_dataframe_free``; the SDK's ``Host::agg_result`` returns an
``OwnedDataFrame`` that does it for you). Call it at ``on_finalize``. Name the
accumulator in the reading plugin's ``consumes`` (section 8): the host orders
the fold so every producer of a consumed name finalizes first, and refuses to
run at all if no loaded plugin provides it - there is no manual ordering to get
right.

.. code-block:: c

   dftu_dataframe* res = agg->agg_result(host->h, "com.example.dur_stats");
   if (res) { /* read columns, then: */ dftu_dataframe_free(res); }

Reading the columns of that frame needs the dataframe C ABI
(``dftracer/utils/dataframe/abi.h``), the one part of the plugin surface that
links the engine rather than headers alone. ``examples/plugins/dur_stats_producer.c``
and ``dur_stats_consumer.c`` are the pair end to end.

Your own mergeable state (register_state)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When no op in the table fits (a graph, a session tracker, a custom sketch),
register a state **type** instead of writing the fold by hand. The host then
drives it exactly like an accumulator: one instance per worker slice,
``update`` per batch, ``merge`` at the fan-in, ``finalize`` once, and the
result published under the state's name for ``agg_result`` and
``Plugins::run``. Register from the factory (the build phase); by run time
the registry is settled.

.. tab-set::

   .. tab-item:: C++ (SDK)

      .. code-block:: cpp

         struct Graph {
             void update(const dftu_dataframe* batch);
             void merge(Graph& other);
             dftu_dataframe* finalize();               // or a bytes result
             std::uint64_t bytes() const;              // optional: the budget measure
             std::string serialize() const;            // optional pair: spill
             static Graph* deserialize(std::string_view);
         };

         extern "C" dftu_plugin* dftracer_plugin(dftu_plugin_host* h,
                                                 const dftu_value* config) {
             return plugin(h, config).state<Graph>("me.graph").build();
         }

      A plugin may have a fold and several states; ``plugin(h, config)`` is
      the builder for all of them (``fold<Slice>()``, ``state<T>(name)``,
      ``op(desc)``), and ``build()`` fails the load when any step failed.

   .. tab-item:: C (raw ABI)

      Fill a ``dftu_state_desc`` (``init`` / ``update`` / ``merge`` /
      ``finalize`` / ``destroy``, optional ``bytes`` and a ``serialize`` /
      ``deserialize`` pair) and call ``agg->register_state(h->h, &desc,
      self)`` from the factory.

A state that declares ``bytes`` is measured against the scan's memory budget;
one that also has the ``serialize`` pair is spilled to sorted runs past it and
merged back at finalize. A state with ``bytes`` but no pair is refused a
spill, once, out loud (``state_spill_refused``), and kept whole.


7. Quantile sketches (DFTU_SVC_SKETCH)
--------------------------------------

A plugin-owned DDSketch accumulates values and reports mergeable quantiles
(``dftu_quantiles``: ``count``, ``min``, ``max``, ``mean``, ``p50``, ``p90``,
``p95``, ``p99``). Unlike a map, the plugin owns the handle and merges it itself
across slices.

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``Host::make_sketch()`` returns an RAII ``Sketch`` that frees itself.
      Keep one per slice, ``add`` during ``step``, ``merge`` in ``merge``, read
      ``result`` at ``finalize``.

      .. code-block:: cpp

         #include <optional>

         struct Durations {
             std::optional<Sketch> sk;      // built lazily from the first Host
             explicit Durations(const Config&) {}
             void step(const Batch& b, Host h) {
                 if (!sk) sk.emplace(h);    // Sketch{h}
                 for (const Event& e : b)
                     if (e.has_dur()) sk->add(double(e.dur()));
             }
             void merge(Durations& o) { if (sk && o.sk) sk->merge(*o.sk); }
             void finalize(Host) {
                 if (sk) { dftu_quantiles q = sk->result(); (void)q; }
             }
         };

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         const dftu_svc_sketch* sk =
             (const dftu_svc_sketch*)host->get_service(host->h, DFTU_SVC_SKETCH);
         dftu_sketch* s = sk->sketch_create(host->h);
         sk->sketch_add(host->h, s, (double)e->dur, 1.0);
         /* sk->sketch_merge(host->h, into, other) in merge */
         dftu_quantiles q = sk->sketch_result(host->h, s);
         sk->sketch_free(host->h, s);   /* plugin owns it */

For a per-key quantile table instead of one global sketch, use an accumulator
with key columns and a ``DFTU_AGG_PCT`` (or ``DFTU_AGG_HIST``) aggregate from
section 6.

8. Inter-plugin communication
-----------------------------

Within the one shared scan, one plugin can hand values to another. Two
mechanisms, each its own extension group. See
:doc:`guides/plugins/inter-plugin-comms` for the full treatment.

**Ports (DFTU_SVC_PORTS)** are a batch-scoped slot keyed by a port name: a
producer publishes during a batch, a consumer reads it back during the same
batch (NULL if the producer has not published, or runs after the consumer). The
bus resets between batches. Name the port in the consumer's ``consumes`` and
the producer's ``provides`` (both a single NULL-terminated name array, one
namespace shared with accumulator names): the host orders the fold so a
producer always runs before its consumers, and refuses to run if no loaded
plugin provides a consumed name.

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``publish_port<T>`` / ``consume_port<T>`` give a typed ``OutPort`` /
      ``InPort`` (``T`` trivially copyable). ``send`` publishes; ``recv``
      returns ``std::optional<T>`` (a copy, safe to keep).

      .. code-block:: cpp

         constexpr const char* CAP = "com.example.perbatch";
         // producer:
         void step(const Batch& b, Host h) {
             std::uint64_t n = 0;
             for (const Event& e : b) if (e.has_dur()) ++n;
             h.publish_port<std::uint64_t>(CAP).send(n);
         }
         // consumer:
         void step(const Batch&, Host h) {
             if (auto n = h.consume_port<std::uint64_t>(CAP).recv()) { /* *n */ }
         }

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         const dftu_svc_ports* p =
             (const dftu_svc_ports*)host->get_service(host->h, DFTU_SVC_PORTS);
         uint64_t key = p->port_key(host->h, "com.example.perbatch");
         uint64_t n = 42;
         p->publish(host->h, key, &n, sizeof(n));         /* producer */
         uint32_t len = 0;
         const void* got = p->consume(host->h, key, &len); /* consumer */

**Cross-worker accumulators (DFTU_SVC_AGG)** are the whole-scan channel: a
producer accumulates into a named accumulator (section 6) and any plugin reads
the cross-worker-merged, finalized result by that name at ``on_finalize``.

.. tab-set::

   .. tab-item:: C++ (SDK)

      .. code-block:: cpp

         // producer, in step (or on_batch):
         const auto a = h.agg("com.example.total", {}, {agg::count("count")});
         if (a) a.accumulate(b.raw());

         // consumer, in on_finalize:
         const OwnedDataFrame res = h.agg_result("com.example.total");
         if (res.handle) { /* read the "count" column */ }

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         const dftu_svc_agg* agg =
             (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);
         static const dftu_agg_col specs[1] =
             {{DFTU_AGG_COUNT, NULL, "count", 0.0, NULL}};
         /* during on_batch */
         dftu_agg* a = agg->agg_new(host->h, "com.example.total", NULL, 0, specs, 1);
         agg->agg_accumulate(host->h, a, df);
         /* at on_finalize, in a plugin naming "com.example.total" in consumes */
         dftu_dataframe* res = agg->agg_result(host->h, "com.example.total");
         if (res) dftu_dataframe_free(res);

**Named results (DFTU_SVC_RESULT)** emit an opaque blob or a user-schema Arrow
array under a name; both surface from ``Plugins::run`` keyed by that name.
Use ``Host::emit_result`` / ``emit_result_arrow`` (C: ``emit`` / ``emit_arrow``
on ``dftu_svc_result``), best called at finalize.

9. Async work and I/O
---------------------

``on_batch`` / ``step`` is always synchronous - it must return ``NULL`` (or
nothing, for ``step``) and stay CPU-bound. Only ``on_finalize`` may return a
task for overlapped I/O or a custom fan-out; the host awaits it before
considering the fold done. Offload real blocking work through the async path
or ``run_blocking``, never inside ``on_batch``.

.. tab-set::

   .. tab-item:: C++ (SDK)

      Give the Slice an ``on_finalize(Host) -> Task`` method (instead of
      ``finalize``) and the SDK detects the coroutine signature. Inside it,
      ``co_await`` the async accessors. ``Io`` (from ``Host::io()``)
      wraps the full backend surface: ``open`` / ``close`` / ``read`` /
      ``write`` / ``pread`` / ``pwrite`` / ``fsync`` / ``ftruncate`` /
      ``fstat``, the vectored ``readv`` / ``writev`` / ``preadv`` / ``pwritev``,
      ``lseek``, ``sendfile``, and the socket ops ``accept`` / ``recv`` /
      ``send``. ``Host::spawn`` runs a callable on a pool worker, ``Host::then``
      sequences one after a task, ``Host::all`` / ``Host::any`` are ``when_all``
      / ``when_any`` over tasks, and ``Host::run_blocking`` runs a synchronous
      call while releasing the worker's run-permit.

      .. code-block:: cpp

         #include <dftracer/utils/plugins/plugin.h>
         #include <fcntl.h>

         using namespace dftracer::utils::plugins;

         struct Writer {
             explicit Writer(const Config&) {}
             void step(const Batch&, Host) {}
             void merge(Writer&) {}

             Task on_finalize(Host h) {
                 int fd = -1, rc = 0;
                 co_await h.io().open("/tmp/out.bin",
                                      O_WRONLY | O_CREAT | O_TRUNC, 0644, &fd);
                 if (fd >= 0) {
                     std::int64_t n = 0;
                     const char msg[] = "done";
                     co_await h.io().write(fd, msg, sizeof(msg) - 1, &n);
                     co_await h.io().close(fd, &rc);
                 }
             }
         };

         extern "C" dftu_plugin* dftracer_plugin(dftu_plugin_host* h,
                                                 const dftu_value* config) {
             return plugin(h, config).fold<Writer>().build();
         }

   .. tab-item:: C (raw ABI)

      Fetch ``DFTU_SVC_IO`` for the ``dftu_io`` calls (each returns a ``dftu_task``
      to compose or await; the out-slot must outlive it) and ``DFTU_SVC_CORO``
      for the combinators ``spawn`` / ``when_all`` / ``when_any`` / ``then`` /
      ``run_blocking``, plus ``drive`` (which the SDK's coroutine adapter uses).
      Return the root task from ``on_finalize`` only - ``on_batch`` must return
      NULL.

      .. code-block:: c

         static dftu_task* on_finalize(void* slice, const dftu_plugin_host* host) {
             (void)slice;
             const dftu_io* io =
                 (const dftu_io*)host->get_service(host->h, DFTU_SVC_IO);
             if (!io) return NULL;
             static int fd = -1;
             /* returns a task the host awaits; compose with dftu_svc_coro */
             return io->open(host->h, "/tmp/out.bin",
                             O_WRONLY | O_CREAT | O_TRUNC, 0644, &fd);
         }

A host utility is reached as a named op instead
(``Host::run_op("dftu.hash.fnv1a", {column})``, ``DFTU_SVC_OPS``). Every
``dftu_svc_coro`` combinator has an SDK method (``spawn`` / ``then`` / ``all``
/ ``any`` / ``run_blocking``), so async composition needs no raw ABI.
See :doc:`guides/plugins/compose-ops` for composing reusable typed ops inside a
plugin.

10. Query DSL against events (DFTU_SVC_QUERY)
---------------------------------------------

Beyond coarse pushdown (section 3), a plugin can compile a query once and test
it against individual events. The compiled ``dftu_query`` is valid for the whole
scan; do not free it.

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``query_compile`` accepts either a raw DSL string or an expression built
      with the ergonomic ``F`` / ``Field`` builder. The SDK re-exports the
      builder into ``dftracer::utils::plugins``, so ``plugin.h`` is the only include and no
      ``query::`` qualifier is needed; the host parses the rendered string, so
      the plugin still links nothing from the query library.

      .. code-block:: cpp

         struct Filtered {
             dftu_query* q_ = nullptr;
             explicit Filtered(const Config&) {}
             void step(const Batch& b, Host h) {
                 if (!q_) q_ = h.query_compile(F("dur") > 1000);
                 // Equivalent: h.query_compile("dur > 1000");
                 for (std::int64_t row = 0; row < b.size(); ++row)
                     if (h.query_matches(q_, b.raw(), row)) { /* ... */ }
             }
             void merge(Filtered&) {}
             void finalize(Host) {}
         };

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         const dftu_svc_query* Q =
             (const dftu_svc_query*)host->get_service(host->h, DFTU_SVC_QUERY);
         dftu_query* q = Q->query_compile(host->h, "dur > 1000", 10);
         if (Q->query_matches(host->h, q, df, row)) { /* ... */ }

11. Arrow interchange (DFTU_SVC_ARROW)
--------------------------------------

Read and write Arrow IPC files for an ``ArrowArray`` / ``ArrowSchema`` pair the
plugin already holds (built with the dataframe engine's own Arrow bridge, or
read back from a previous IPC file). This group is file I/O only - it does not
convert a batch to Arrow for you. The caller owns the exported array/schema and
must release them; the SDK's ``OwnedArrow`` does that in its destructor.

.. tab-set::

   .. tab-item:: C++ (SDK)

      .. code-block:: cpp

         void finalize(Host h) {
             OwnedArrow a = h.arrow_read_ipc("/tmp/in.arrow");
             if (a) h.arrow_write_ipc(a, "/tmp/out.arrow");
         }

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         const dftu_svc_arrow* A =
             (const dftu_svc_arrow*)host->get_service(host->h, DFTU_SVC_ARROW);
         struct ArrowArray arr; struct ArrowSchema sch;
         if (A->arrow_read_ipc(host->h, "/tmp/in.arrow", &arr, &sch) == 0) {
             A->arrow_write_ipc(host->h, &arr, &sch, "/tmp/out.arrow");
             arr.release(&arr); sch.release(&sch);   /* caller owns */
         }

12. Output writers
------------------

Two writer groups exist. The **trace writer (DFTU_SVC_TRACE)** appends events to
a gzip ``.pfw.gz`` and has an SDK wrapper on ``Host``; the index is built lazily
on first read, not at close.

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``Host::trace_open_write`` / ``trace_write`` / ``trace_close``, plus
      ``trace_read`` to scan an existing (auto-indexed) trace.

      .. code-block:: cpp

         void step(const Batch& b, Host h) {
             dftu_trace_writer* w = h.trace_open_write("/tmp/out.pfw.gz");
             if (w) { h.trace_write(w, b.raw()); h.trace_close(w); }
         }

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         const dftu_svc_trace* T =
             (const dftu_svc_trace*)host->get_service(host->h, DFTU_SVC_TRACE);
         dftu_trace_writer* w = T->trace_open_write(host->h, "/tmp/out.pfw.gz");
         T->trace_write(host->h, w, df);   /* every row of df, by column name */
         T->trace_close(host->h, w);

The **parallel writer (DFTU_SVC_WRITER)** is a sharded, multi-worker gzip-member
output (``writer_create`` / ``writer_open`` / ``writer_chunk`` / ``writer_close``
/ ``merge_shards``).

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``Host::writer`` returns an ergonomic ``Writer`` (host-owned, nothing to
      free); ``open`` / ``chunk`` / ``close`` are ``co_await``-able AsyncOps, and
      ``Host::merge_shards`` concatenates shard files into a target.

      .. code-block:: cpp

         Task on_finalize(Host h) {
             Writer w = h.writer("/tmp/out.pfw.gz", /*num_workers=*/4,
                                 /*gzip=*/true);
             if (!w) co_return;
             co_await w.open();
             co_await w.chunk(/*worker=*/0, std::string_view{"payload"});
             co_await w.close();
         }

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         const dftu_svc_writer* W =
             (const dftu_svc_writer*)host->get_service(host->h, DFTU_SVC_WRITER);
         dftu_writer* w = W->writer_create(host->h, "/tmp/out.pfw.gz",
                                          /*num_workers=*/4, /*gzip=*/1);
         /* co-await W->writer_open / writer_chunk / writer_close (tasks) */

13. Getting results back
------------------------

Each host-owned accumulator is finalized at scan end to a table keyed by the
accumulator's name, with the key columns first and one column per aggregate;
interned string ids are resolved to their labels once, so a string key column is
a real ``string``. Named results (section 8) surface the same way, keyed by their
emit name.

**From Python**, build a fixed set of plugins and run them over a single fused
scan; each result comes back shaped by its emit kind (a native
:class:`~dftracer.utils.DataFrame`, a ``pyarrow.Table``, a
:class:`~dftracer.utils.LazyFrame`, or ``bytes``):

.. code-block:: python

   from dftracer.utils.plugins import Plugins

   plugins = Plugins(["./name_edges.so"])   # .so paths, or @jit.plugin classes
   run = plugins.run("./traces")            # a directory or list of .pfw.gz traces
   table = run.results["name_edges"]        # DataFrame[pid, name, edges, dur_sum]
   df = table.to_pandas()
   print(run.stats["events_scanned"])

Construction (``Plugins(...)``) builds the set immediately - dlopen, the ABI
gate, and the fold order are all resolved in ``__init__``, so a bad path or an
ABI mismatch raises there rather than from ``run()``. The output tables cross
to NumPy or pandas cheaply (zero-copy where the dtype allows); the fold itself
runs in compiled code, so aggregate in the native scan and do NumPy / pandas
analysis on the (much smaller) result tables:

.. code-block:: python

   values = table.to_arrow().column("edges").to_numpy(zero_copy_only=False)

**From C++**, ``Plugins`` is the same immutable set: ``builder()`` loads each
``.so`` with its config, ``build()`` settles the ABI gate, the fold order and
the union prune once, and ``run(view)`` folds the set over one scan of a
``View``. ``attach(session)`` instead joins the plugins to a ``ViewSession``
as one more fused branch, so they co-scan with the session's own queries
(the union prune is then only an offer the session may drop, since the
shared scan must stay wide enough for every branch):

.. code-block:: cpp

   using dftracer::utils::plugins::Plugins;

   auto set = Plugins::builder()
                  .add("./name_edges.so", R"({"query": "cat == \"POSIX\""})")
                  .add("./dur_stats.so")
                  .build();                      // Result<Plugins>: a bad path,
   if (!set) throw std::runtime_error(set.error().message);   // ABI or graph fault
   PluginRun run = co_await set->run(View::from_files(files));
   // NamedResult is a variant: bytes, OwnedArrow, OwnedDataFrame, OwnedLazyFrame.
   auto& table = std::get<OwnedDataFrame>(run.results.results().at("name_edges"));

**From the command line**, ``dftracer_run`` folds every ``--plugin`` over one
shared, index-pruned scan and reports the scan on stderr (it does not print
result tables - read those through :class:`~dftracer.utils.plugins.Plugins`):

.. code-block:: bash

   dftracer_run --plugin ./name_edges.so --files trace.pfw.gz
   dftracer_run --plugin ./a.so --plugin ./b.so -d ./traces      # one shared scan

Use ``-d <directory>`` for a whole tree instead of ``--files``.

14. Extending the engine: ops, sources, plan nodes
--------------------------------------------------

A plugin is not only a fold over the scan. Three registries let it add to the
engine itself, each name-keyed and process-wide, each refusing a duplicate
name and the host's own namespaces (bare names and ``dftu.`` are the host's; a
plugin's names are ``<plugin>.<name>``). Register from the factory; by run
time the registries are settled.

**An op** (``DFTU_SVC_OPS``). The same registry the engine's built-ins live
in: a ``dftu_op_desc`` is a name, a packed signature (``DFTU_OP_SIG(SERIES,
SERIES, NONE, NONE)`` for ``series -> series``) and a function pointer. Once
registered it is reachable from anywhere: ``run`` / ``run_frame`` /
``run_aggregate`` / ``run_lazy`` in another plugin, ``dftu_op_run`` from C,
``op_run`` from Python, and a ``LazyFrame`` plan through
``dftu_lazyframe_op``. The same service runs any built-in on handles the
plugin already holds (``Host::run_op("dftu.hash.fnv1a", {column})``), so a
plugin never links the dataframe ABI to use the engine.

.. code-block:: cpp

   static dftu_series* zscore(const dftu_series* v);   // series -> series
   static const dftu_op_desc ZSCORE = {"me.zscore",
       DFTU_OP_SIG(SERIES, SERIES, NONE, NONE), (const void*)&zscore};
   return plugin(h, config).op(ZSCORE).fold<MySlice>().build();

**A source** (``DFTU_SVC_PROVIDERS``). A ``dftu_source_vt`` is the C mirror
of the engine's ``Source`` / ``Cursor`` pair: ``schema`` names the columns
without a scan (``schema_types`` optionally types them, nested types
included), ``scan`` opens a cursor honoring a ``dftu_scan_request``
(projection, filters, limit, budget) and reports per filter how completely it
applied it (``DFTU_PUSHED_NO`` / ``INEXACT`` / ``EXACT``; the host re-applies
anything short of exact), and the cursor's ``next`` pulls one frame at a
time, synchronously or as a task. ``dftu_lazyframe_from_provider(name)``
then scans it like a file, from any language.

**A plan node** (``dftu_node_register``, in the dataframe ABI). A
``dftu_node_vt`` is a cursor that takes an input cursor and returns an
output cursor: a streaming op (one morsel in, one out, state kept between
morsels: sessionize, coalesce, a GPU kernel) or a breaker (drain the input
on the first ``next``, emit one frame). ``output_schema`` declares its
columns from the input's, so the optimizer can type the plan around it; it
may not push a filter or projection through the node, and ``explain`` names
it. ``LazyFrame::op(name, args)`` / ``dftu_lazyframe_op`` stacks it on a
plan like any built-in step. The cursor's optional slots let it take part in
the plan's machinery: ``narrow`` forwards a join's key set to its input,
``bytes`` / ``reclaim`` hold it to the memory budget
(:doc:`guides/runtime/memory-budget`).

**Plans inside a plugin.** ``OwnedLazyFrame`` (``result_registry.h``) is the
fluent facade over ``dftu.lazy.*``: build a plan from a frame
(``dftu_dataframe_lazy``) or a registered source, chain ``filter`` /
``with_column`` / ``group_by`` / ``join`` / ``sort_by`` / ``topk`` /
``pivot`` / ``group_by_dynamic`` / ... (every step goes through
``dftu_svc_ops::run_lazy``, so the plugin links nothing), and hand it out
with ``emit_result_lazyframe``: the caller gets a ``LazyFrame`` it can keep
composing, and nothing runs until they collect. A failed step yields an empty
handle every later step short-circuits on, so a chain fails once, at the
end, not with a crash in the middle. ``emit_result_frame`` emits an eager
frame the same way.

``tests/utilities/plugins/reference_plugin.c`` is the conformance plugin: it
touches every service slot, registers an op, a source, a state and a node,
and is the one file to read for the exact C of each.

Host services index
--------------------

Every service is an optional extension group fetched by id
(``host->get_service``); the SDK ``Host`` memoizes each group and degrades a
missing one to a null/no-op. The map from SDK accessor to group to the section
that covers it:

.. list-table::
   :header-rows: 1
   :widths: 30 22 48

   * - C++ SDK accessor
     - Extension group
     - Covered in
   * - ``Host::agg`` / ``agg_result`` (``Agg``, the ``agg::`` col factories)
     - ``DFTU_SVC_AGG``
     - `6. Mergeable aggregation (DFTU_SVC_AGG)`_
   * - ``Host::make_sketch`` (RAII ``Sketch``)
     - ``DFTU_SVC_SKETCH``
     - `7. Quantile sketches (DFTU_SVC_SKETCH)`_
   * - ``Host::publish_port`` / ``consume_port`` (``OutPort`` / ``InPort``)
     - ``DFTU_SVC_PORTS``
     - `8. Inter-plugin communication`_
   * - ``Host::emit_result`` / ``emit_result_arrow``
     - ``DFTU_SVC_RESULT``
     - `8. Inter-plugin communication`_
   * - ``Host::all`` / ``any`` / ``run_blocking``
     - ``DFTU_SVC_CORO``
     - `9. Async work and I/O`_
   * - ``Host::io()`` (typed ``Io``)
     - ``DFTU_SVC_IO``
     - `9. Async work and I/O`_
   * - typed compose ops (``dftracer::utils::plugins::make_op`` / ``run``)
     - ``DFTU_SVC_COMPOSE``
     - :doc:`guides/plugins/compose-ops`
   * - ``Host::query_compile`` / ``query_matches``
     - ``DFTU_SVC_QUERY``
     - `10. Query DSL against events (DFTU_SVC_QUERY)`_
   * - ``Host::arrow_read_ipc`` / ``arrow_write_ipc``
     - ``DFTU_SVC_ARROW``
     - `11. Arrow interchange (DFTU_SVC_ARROW)`_
   * - ``Host::trace_open_write`` / ``trace_write`` / ``trace_read``
     - ``DFTU_SVC_TRACE``
     - `12. Output writers`_
   * - none (raw ``get_service``)
     - ``DFTU_SVC_WRITER``
     - `12. Output writers`_
   * - ``Host::run_op`` / ``run_op_frame`` / ``run_op_aggregate``,
       ``PluginBuilder::op``, ``OwnedLazyFrame``
     - ``DFTU_SVC_OPS``
     - `14. Extending the engine: ops, sources, plan nodes`_
   * - none (raw ``get_service``)
     - ``DFTU_SVC_PROVIDERS``
     - `14. Extending the engine: ops, sources, plan nodes`_
   * - ``Host::emit_result_frame`` / ``emit_result_lazyframe``
     - ``DFTU_SVC_RESULT``
     - `14. Extending the engine: ops, sources, plan nodes`_
   * - ``PluginBuilder::state<T>``
     - ``DFTU_SVC_AGG`` (``register_state``)
     - `6. Mergeable aggregation (DFTU_SVC_AGG)`_

Building and scaffolding
------------------------

The ``dftracer_plugin`` console script (installed with the Python package,
``python/dftracer/utils/plugin_cli.py``) scaffolds a template source and builds
it, so you need neither the lifecycle boilerplate nor the compiler flags by
hand. This is distinct from the ``@jit.plugin`` DSL (:doc:`jit`), which writes
and compiles a plugin from Python source at call time.

``dftracer_plugin new`` writes a template plugin (a per-``pid`` counter into a
mergeable map). The default is the raw-C template; ``--cpp`` emits the SDK
template against ``plugin.h``:

.. code-block:: bash

   dftracer_plugin new name_edges --cpp          # -> ./name_edges.cpp (SDK)
   dftracer_plugin new name_edges                # -> ./name_edges.c (raw ABI)
   dftracer_plugin new name_edges --cpp -o ./plugins   # into a chosen directory

``dftracer_plugin build`` compiles a source straight to a loadable ``.so``,
resolving the plugin include directory for you (bundled with an installed wheel,
or walked up from a source checkout; override with the ``DFTRACER_PLUGIN_INCLUDE``
environment variable):

.. code-block:: bash

   dftracer_plugin build name_edges.cpp                  # -> ./name_edges.so
   dftracer_plugin build name_edges.cpp -o out/edges.so   # explicit output path

``dftracer_plugin cflags`` prints the flags ``build`` uses
(``-std=c++20 -fPIC -shared -I<plugin-include-dir>``, plus
``-undefined dynamic_lookup`` on macOS, since a plugin ``.so`` loads into a host
process rather than pre-linking against one), for wiring a plugin into your own
build system. ``build`` and ``cflags`` always target a C++ compiler (``$CXX``,
else ``c++`` / ``clang++``) even for a ``.c`` template, since the flags are
C++20 flags:

.. code-block:: bash

   c++ $(dftracer_plugin cflags) -o name_edges.so name_edges.cpp

For a full compiled-and-run C++ walkthrough (scaffold, edit, build, run), see
:doc:`tutorials/write-a-c-plugin`.
