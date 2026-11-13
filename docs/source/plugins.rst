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
  ``finalize`` methods, assembled with
  ``plugin(ctx, h, config).fold<Slice>().build(out)``
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
- **Services are optional extension groups**: aggregation, async I/O, duql filters,
  Arrow, writers, sketches, and inter-plugin channels are fetched by id; a
  missing group degrades gracefully to a null/no-op.

1. Entry point and lifecycle
----------------------------

A plugin is a shared library exporting two symbols: ``dftracer_plugin_info``
(the ``DFTRACER_UTILS_PLUGIN_INFO_SYMBOL``, defined with
``DFTU_PLUGIN_INFO(name, version)``) and ``dftracer_plugin`` (the
``DFTRACER_UTILS_PLUGIN_FACTORY_SYMBOL`` the loader resolves via ``dlsym``). It
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
   * - config ``"duql"`` key
     - ``plan_duql``
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

``on_batch`` returns a ``dftu_status`` (or nothing, for the sync SDK ``step``)
since it is always synchronous; ``on_finalize`` leaves ``*pending`` NULL when
handled synchronously, or sets a ``dftu_task`` the host awaits for async work
(section 9). Every callback takes the host's ``dftu_ctx*`` first. A negative
status is an error: report it with ``dftu_fail(ctx, code, message)``, and the
host names the plugin and the callback in the error the caller sees. A long
loop polls ``dftu_check(ctx)`` and returns what it gives, so a cancelled scan
stops.
The delivered ``dftu_dataframe`` is owned by the host and valid only for the
call - N rows in scan order is N events, never retain the pointer.

``DFTRACER_UTILS_PLUGIN_ABI_VERSION`` comes from the ``abi`` line of the
``VERSION`` file, packed as ``(major << 16) | (minor << 8) | patch``.
``DFTRACER_UTILS_PLUGIN_ABI_HASH`` is a hash of the plugin ABI headers. The
loader reads ``dftracer_plugin_info`` before it runs any plugin code. A plugin
that exports no ``dftracer_plugin_info``, or was built against another ABI
version or other headers, is refused at load with a message that names it and
is never called into. Rebuild the plugin against the host you run.

.. tab-set::

   .. tab-item:: C++ (SDK)

      A ``Slice`` provides the constructor and methods; ``plugin(ctx, h,
      config).fold<Slice>().build(out)`` fills the ``dftu_plugin`` struct and
      adapts exceptions at the boundary: every callback catches, and a thrown
      ``std::exception`` becomes an error carrying its ``what()`` text.
      ``struct_size`` is set for you.

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

         DFTU_PLUGIN_INFO("my_plugin", "1.0")

         extern "C" DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(
             dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
             dftu_plugin** out) {
             return plugin(ctx, h, config).fold<MyPlugin>().build(out);
         }

   .. tab-item:: C (raw ABI)

      Fill the ``dftu_plugin`` struct by hand. ``struct_size`` must be set to
      ``sizeof(dftu_plugin)``; ``self`` is the read-only config shared across
      slices.

      .. code-block:: c

         #include <dftracer/utils/plugins/abi.h>
         #include <stdlib.h>

         static const char* plan_duql(void* self) { (void)self; return NULL; }
         static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {
             (void)self;
             *out = calloc(1, 1);
             return *out ? DFTU_OK : dftu_fail(ctx, DFTU_E_INTERNAL, "out of memory");
         }
         static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                                     const dftu_dataframe* df,
                                     const dftu_plugin_host* host) {
             (void)ctx; (void)slice; (void)df; (void)host;
             return DFTU_OK;  /* synchronous */
         }
         static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {
             (void)ctx; (void)into; (void)other;
             return DFTU_OK;
         }
         static dftu_status on_finalize(dftu_ctx* ctx, void* s,
                                        const dftu_plugin_host* h,
                                        dftu_task** pending) {
             (void)ctx; (void)s; (void)h;
             *pending = NULL;
             return DFTU_OK;
         }
         static void destroy_slice(void* slice) { free(slice); }
         static void destroy(void* self) { (void)self; }

         static dftu_plugin g_plugin;

         DFTU_PLUGIN_INFO("my_plugin", "1.0")

         DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(
             dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
             dftu_plugin** out) {
             (void)ctx; (void)h; (void)config;
             g_plugin.struct_size   = sizeof(dftu_plugin);
             g_plugin.self          = NULL;
             g_plugin.plan_duql    = plan_duql;
             g_plugin.make_slice    = make_slice;
             g_plugin.on_batch      = on_batch;
             g_plugin.merge         = merge;
             g_plugin.on_finalize   = on_finalize;
             g_plugin.destroy_slice = destroy_slice;
             g_plugin.destroy       = destroy;
             *out = &g_plugin;
             return DFTU_OK;
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
``"args.<key>"``. A plugin that
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

3. Coarse predicate pushdown (plan_duql)
-----------------------------------------

A plugin can narrow the shared scan to the chunks that can possibly match, using
the same duql filter language the analytics use (see :doc:`guides/core/duql`). The
host uses the returned predicate to prune index chunks before the fold ever sees
them; it is a coarse filter, so a plugin still checks per event if it needs
exactness. Returning nothing scans everything.

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``make_plugin`` reads the pushdown predicate from the plugin's ``"duql"``
      config value, so set it with ``--parg duql=...`` (section 4) or a config
      file. There is no separate ``Slice`` method for it.

      .. code-block:: bash

         dftracer_run --plugin ./p.so --parg duql='cat == "POSIX"' \
                      trace.pfw.gz

   .. tab-item:: C (raw ABI)

      Implement ``plan_duql`` to return any duql string (scan-lifetime storage),
      or NULL for no filter.

      .. code-block:: c

         static const char* plan_duql(void* self) {
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

         DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(
             dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
             dftu_plugin** out) {
             (void)h;
             int64_t threshold = dftu_as_i64(dftu_obj_get(config, "threshold"), 0);
             if (threshold < 0)
                 return dftu_fail(ctx, DFTU_E_INVALID, "threshold must not be negative");
             /* stash `threshold` in a static/self the slices can read */
             /* ... fill g_plugin ... */
             *out = &g_plugin;
             return DFTU_OK;
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
``dftu_series_type`` / ``dftu_series_data``. A string column can be a
dictionary column, for which ``dftu_series_data`` and ``dftu_series_offsets``
are null: read its strings with ``dftu_series_string_at(col, row, &len)``.

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

         static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                                     const dftu_dataframe* df,
                                     const dftu_plugin_host* host) {
             (void)ctx; (void)slice;
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
             return DFTU_OK;
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

         static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                                     const dftu_dataframe* df,
                                     const dftu_plugin_host* host) {
             (void)slice;
             const dftu_svc_agg* agg =
                 (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);
             static const char* const keys[2] = {"pid", "name"};
             static const dftu_agg_col specs[2] = {
                 {DFTU_AGG_COUNT, NULL, "edges", 0.0, NULL},
                 {DFTU_AGG_SUM, "dur", "dur_sum", 0.0, NULL}};
             dftu_agg* a = NULL;
             dftu_status st;
             if (!agg || !agg->agg_new)
                 return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no aggregation service");
             st = agg->agg_new(ctx, host->h, "name_edges", keys, 2, specs, 2, &a);
             if (st < 0) return st;
             return agg->agg_accumulate(ctx, host->h, a, df);
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
   dftu_agg* a = NULL;
   dftu_status st = agg->agg_new(ctx, host->h, "com.example.dur_stats", NULL, 0,
                                 specs, 3, &a);

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

   dftu_dataframe* res = NULL;
   dftu_status st = agg->agg_result(ctx, host->h, "com.example.dur_stats", &res);
   if (st >= 0 && res) { /* read columns, then: */ dftu_dataframe_free(res); }

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
             dstd::String serialize() const;           // optional pair: spill
             static Graph* deserialize(dstd::StringView);
         };

         extern "C" DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(
             dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
             dftu_plugin** out) {
             return plugin(ctx, h, config).state<Graph>("me.graph").build(out);
         }

      A plugin may have a fold and several states; ``plugin(ctx, h, config)``
      is the builder for all of them (``fold<Slice>()``, ``state<T>(name)``,
      ``op(desc)``), and ``build(out)`` fails the load when any step failed.

   .. tab-item:: C (raw ABI)

      Fill a ``dftu_state_desc`` (``init`` / ``update`` / ``merge`` /
      ``finalize`` / ``destroy``, optional ``bytes`` and a ``serialize`` /
      ``deserialize`` pair) and call ``agg->register_state(ctx, h->h, &desc,
      self)`` from the factory. Every state callback takes the ``dftu_ctx*``
      first and returns a ``dftu_status``; an error from ``update``, ``merge``
      or ``finalize`` fails the scan with the plugin's message.

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
         /* the ports slots take no ctx: they cannot fail */
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
         dftu_agg* a = NULL;
         dftu_status st = agg->agg_new(ctx, host->h, "com.example.total", NULL, 0,
                                       specs, 1, &a);
         if (st >= 0) st = agg->agg_accumulate(ctx, host->h, a, df);
         /* at on_finalize, in a plugin naming "com.example.total" in consumes */
         dftu_dataframe* res = NULL;
         st = agg->agg_result(ctx, host->h, "com.example.total", &res);
         if (st >= 0 && res) dftu_dataframe_free(res);

**Named results (DFTU_SVC_RESULT)** emit an opaque blob or a user-schema Arrow
array under a name; both surface from ``Plugins::run`` keyed by that name.
Use ``Host::emit_result`` / ``emit_result_arrow`` (C: ``emit`` / ``emit_arrow``
on ``dftu_svc_result``), best called at finalize.

9. Async work and I/O
---------------------

``on_batch`` / ``step`` is always synchronous - it returns its status (or
nothing, for ``step``) and stays CPU-bound. Only ``on_finalize`` may set
``*pending`` to a task for overlapped I/O or a custom fan-out; the host awaits
it before considering the fold done. Offload real blocking work through the
task service (``DFTU_JOB_BLOCKING``), never inside ``on_batch``.

A task is owned by whoever awaits it: the host, once the task comes back
through ``*pending`` or a ``drive`` wait slot. A plugin must hand back every
task it starts, also on an error return. The first error of a combinator
cancels and awaits the tasks it no longer needs before it finishes, and a
cancelled task reports ``DFTU_E_CANCELLED``.

.. tab-set::

   .. tab-item:: C++ (SDK)

      Give the Slice an ``on_finalize(Host) -> Task`` method (instead of
      ``finalize``) and the SDK detects the coroutine signature. Inside it,
      ``co_await`` the async accessors. ``Io`` (from ``Host::io()``)
      wraps the full backend surface: ``open`` / ``close`` / ``read`` /
      ``write`` / ``pread`` / ``pwrite`` / ``fsync`` / ``ftruncate`` /
      ``fstat``, the vectored ``readv`` / ``writev`` / ``preadv`` / ``pwritev``,
      ``lseek``, ``sendfile``, and the socket ops ``accept`` / ``recv`` /
      ``send``. ``Host::spawn`` runs a callable on a pool worker (pass
      ``DFTU_JOB_BLOCKING`` for a call that blocks, so the worker gives back its
      run permit), and ``Host::all`` awaits several spawned tasks together. A
      step after a task is the next ``co_await``. Typed jobs (``job.h``,
      ``make_job<In, Out>``) pipe with ``|`` and fan out with ``when_all`` /
      ``when_any``; see :doc:`guides/plugins/compose-ops`. A call the host
      refuses, or a task that fails, throws ``Error`` out of the ``co_await``,
      and an uncaught one fails ``on_finalize`` with its message.

      .. code-block:: cpp

         #include <dftracer/utils/plugins/plugin.h>
         #include <fcntl.h>

         using namespace dftracer::utils::plugins;

         struct Writer {
             explicit Writer(const Config&) {}
             void step(const Batch&, Host) {}
             void merge(Writer&) {}

             Task on_finalize(Host h) {
                 int fd = -1;
                 co_await h.io().open("/tmp/out.bin",
                                      O_WRONLY | O_CREAT | O_TRUNC, 0644, &fd);
                 std::int64_t n = 0;
                 const char msg[] = "done";
                 co_await h.io().write(fd, msg, sizeof(msg) - 1, &n);
                 co_await h.io().close(fd);
             }
         };

         DFTU_PLUGIN_INFO("writer", "1.0")

         extern "C" DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(
             dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
             dftu_plugin** out) {
             return plugin(ctx, h, config).fold<Writer>().build(out);
         }

   .. tab-item:: C (raw ABI)

      Fetch ``DFTU_SVC_IO`` for the ``dftu_io`` calls (each starts a
      ``dftu_task`` and hands it back through an out-parameter; the out-slot
      must outlive it) and ``DFTU_SVC_TASK`` for jobs (``job`` / ``then`` /
      ``all`` / ``any`` / ``map`` / ``start``), ``spawn`` and ``drive`` (which
      the SDK's coroutine adapter uses). Set ``*pending`` from ``on_finalize``
      only; ``on_batch`` returns its status.

      .. code-block:: c

         static int fd = -1;

         static dftu_status on_finalize(dftu_ctx* ctx, void* slice,
                                        const dftu_plugin_host* host,
                                        dftu_task** pending) {
             (void)slice;
             const dftu_svc_io* io =
                 (const dftu_svc_io*)host->get_service(host->h, DFTU_SVC_IO);
             if (!io) return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no io service");
             /* the task the host awaits; its status is the open's status */
             return io->open(ctx, host->h, "/tmp/out.bin",
                             O_WRONLY | O_CREAT | O_TRUNC, 0644, &fd, pending);
         }

A host utility is reached as a named op instead
(``Host::run_op("dftu.hash.fnv1a", {column})``, ``DFTU_SVC_OPS``). Every
``dftu_svc_task`` call has an SDK form (``Host::spawn``, ``Host::tasks()``,
and the typed ``Job`` in ``job.h``), so async composition needs no raw ABI.

10. duql filters against events (DFTU_SVC_DUQL)
-----------------------------------------------

Beyond coarse pushdown (section 3), a plugin can compile a query once and test
it against individual events. The compiled ``dftu_duql`` is valid for the whole
scan; do not free it.

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``duql_compile`` accepts either a raw duql string or an expression built
      with the ergonomic ``F`` / ``Field`` builder. The SDK re-exports the
      builder into ``dftracer::utils::plugins``, so ``plugin.h`` is the only include and no
      ``duql::`` qualifier is needed; the host parses the rendered string, so
      the plugin still links nothing from the duql library.

      .. code-block:: cpp

         struct Filtered {
             dftu_duql* q_ = nullptr;
             explicit Filtered(const Config&) {}
             void step(const Batch& b, Host h) {
                 if (!q_) q_ = h.duql_compile(F("dur") > 1000);
                 // Equivalent: h.duql_compile("dur > 1000");
                 for (std::int64_t row = 0; row < b.size(); ++row)
                     if (h.duql_matches(q_, b.raw(), row)) { /* ... */ }
             }
             void merge(Filtered&) {}
             void finalize(Host) {}
         };

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         const dftu_svc_duql* Q =
             (const dftu_svc_duql*)host->get_service(host->h, DFTU_SVC_DUQL);
         dftu_duql* q = NULL;
         dftu_status st = Q->duql_compile(ctx, host->h, "dur > 1000", 10, &q);
         if (st >= 0 && Q->duql_matches(host->h, q, df, row)) { /* ... */ }

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
         if (A->arrow_read_ipc(ctx, host->h, "/tmp/in.arrow", &arr, &sch) >= 0) {
             A->arrow_write_ipc(ctx, host->h, &arr, &sch, "/tmp/out.arrow");
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
             TraceWriter w = h.trace_writer("/tmp/out.pfw.gz");  // RAII
             w.write(b.raw());
             w.close();
         }

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         const dftu_svc_trace* T =
             (const dftu_svc_trace*)host->get_service(host->h, DFTU_SVC_TRACE);
         dftu_trace_writer* w = NULL;
         T->trace_open_write(ctx, host->h, "/tmp/out.pfw.gz", &w);
         T->trace_write(ctx, host->h, w, df);   /* every row of df, by column name */
         T->trace_close(ctx, host->h, w);

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
             Writer w = h.writer("/tmp/out.pfw.gz", /*worker_count=*/4,
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
         dftu_writer* w = NULL;
         W->writer_create(ctx, host->h, "/tmp/out.pfw.gz",
                          /*num_workers=*/4, /*gzip=*/1, &w);
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

   using dftracer::utils::domains::plugins::Plugins;

   auto set = Plugins::builder()
                  .add("./name_edges.so", R"({"duql": "cat == \"POSIX\""})")
                  .add("./dur_stats.so")
                  .build();                      // dstd::Result<Plugins>: a bad path,
   if (!set) throw std::runtime_error(set.error().message);   // ABI or graph fault
   PluginRun run = co_await set->run(View::from_files(files));
   // NamedResult is a variant: bytes, OwnedArrow, OwnedDataFrame, OwnedLazyFrame.
   auto& table = std::get<OwnedDataFrame>(run.results.results().at("name_edges"));

**From the command line**, ``dftracer_run`` folds every ``--plugin`` over one
shared, index-pruned scan and reports the scan on stderr (it does not print
result tables - read those through :class:`~dftracer.utils.plugins.Plugins`):

.. code-block:: bash

   dftracer_run --plugin ./name_edges.so trace.pfw.gz
   dftracer_run --plugin ./a.so --plugin ./b.so ./traces      # one shared scan

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
       DFTU_OP_SIG(SERIES, SERIES, NONE, NONE), (dftu_op_generic_fn)&zscore};
   return plugin(ctx, h, config).op(ZSCORE).fold<MySlice>().build(out);

**A source** (``DFTU_SVC_PROVIDERS``). A ``dftu_source_vt`` is the C mirror
of the engine's ``Source`` / ``Cursor`` pair: ``schema`` names the columns
without a scan (``schema_types`` optionally types them, nested types
included), ``scan`` opens a cursor honoring a ``dftu_scan_request``
(projection, filters, limit, budget) and reports per filter how completely it
applied it (``DFTU_PUSHED_NO`` / ``INEXACT`` / ``EXACT``; the host re-applies
anything short of exact), and the cursor's ``next`` pulls one frame at a
time, synchronously or through a ``pending`` task (``DFTU_END`` ends the
stream). Every ``dftu_source_vt`` and ``dftu_cursor_vt`` callback takes the
``dftu_ctx*`` first and returns a ``dftu_status``; a negative status is the
error, with the message the callback set on the ctx.
``dftu_lazyframe_from_provider(name)``
then scans it like a file, from any language. An optional ``apply`` callback lets the source
absorb whole ops at plan time (filter, projection, aggregation, sort,
top-N, limit, tail, join); see the planning hooks in
:doc:`guides/data/lazyframe`.

In C++, write the source as a class and let ``plugins/plugin/source.h``
build the vtables. The class names its columns and scans; each planning hook
is optional, and only the ones it defines are wired up. Requests arrive as
views (``ExprView`` with ``as_compare`` / ``as_logical`` / ``as_not`` /
``as_str_pred`` / ``as_is_in``, ``SortView``, ``AggregationView``,
``JoinView<S>`` with the other side as ``S``), and results are owned C++
values (``OwnedFrame``, ``Applied<S>``). Exceptions never cross the ABI: a
thrown ``std::exception`` fails the call with its ``what()`` text as the
message. A failed cursor fails the scan with it; for a failed planning hook the
host logs the message and applies the op itself.

.. code-block:: cpp

   #include <dftracer/utils/plugins/plugin.h>
   namespace pl = dftracer::utils::plugins;

   class Range {                        // rows id = lo..hi-1
    public:
       dstd::Vector<dstd::String> names() const { return {"id"}; }
       struct Cursor {
           pl::OwnedFrame frame;
           dstd::Optional<pl::OwnedFrame> next(std::int64_t) {
               if (!frame) return dstd::nullopt;
               return std::move(frame);
           }
       };
       pl::ScanResult<Cursor> scan(const pl::ScanView&) const;
       // `id > k` becomes a narrower range; anything else stays with the engine.
       dstd::Optional<pl::Applied<Range>> apply_filter(pl::ExprView p) const {
           auto c = p.as_compare();
           if (!c || c->column != 0 || c->op != DFTU_CMP_GT) return dstd::nullopt;
           return pl::Applied<Range>{
               dstd::make_unique<Range>(c->rhs.value.i + 1, hi_)};
       }
       // ...
   };

   return pl::plugin(ctx, h, config)
       .source<Range>("me.range", dstd::make_unique<Range>(0, 1000))
       .build(out);

**A plan node** (``DFTU_SVC_NODES``; ``dftu_node_register`` in the
dataframe ABI for a non-plugin caller). Register it from the factory through
the service: the host gates the name to ``<plugin>.<name>`` and unregisters
the node before the plugin unloads, so no plan can reach code that is gone. A
``dftu_node_vt`` is a cursor that takes an input cursor and returns an
output cursor: a streaming op (one morsel in, one out, state kept between
morsels: sessionize, coalesce, a GPU kernel) or a breaker (drain the input
on the first ``next``, emit one frame). ``output_schema`` declares its
columns from the input's, so the optimizer can type the plan around it; it
may not push a filter or projection through the node, and ``explain`` names
it. ``LazyFrame::op(name, args)`` / ``dftu_lazyframe_op`` stacks it on a
plan like any built-in step. The cursor's optional slots let it take part in
the plan's machinery: ``narrow`` forwards a join's key set to its input,
``open`` receives the plan's memory budget in bytes, and ``bytes`` /
``reclaim`` hold it to that budget
(:doc:`guides/runtime/memory-budget`).

In C++, ``plugins/plugin/node.h`` does the same for a node class:
``output_schema(SchemaView in, OpArgs args, SchemaBuilder& out)`` and
``open(InputCursor in, OpArgs args, std::uint64_t memory_budget)`` returning
a cursor like a source's. ``memory_budget`` is the plan's budget in bytes,
never 0; a node that holds state sizes its spill threshold and its output
frames from it.
``InputCursor`` owns the upstream: ``next()`` pulls it (running an upstream
task to completion when it has to) and it is destroyed exactly once, by the
node when ``open`` succeeds and by the host when it fails.

.. code-block:: cpp

   struct Double {
       void output_schema(const pl::SchemaView& in, const pl::OpArgs&,
                          pl::SchemaBuilder& out) const { in.copy_all(out); }
       struct Cursor {
           pl::InputCursor in;
           dstd::Optional<pl::OwnedFrame> next(std::int64_t rows);  // pull, transform
       };
       dstd::UniquePtr<Cursor> open(pl::InputCursor in, const pl::OpArgs&,
                                    std::uint64_t) const {
           return dstd::make_unique<Cursor>(Cursor{std::move(in)});
       }
   };

   return pl::plugin(ctx, h, config).node<Double>(
       "me.double", dstd::make_unique<Double>()).build(out);

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

15. Index extensions (DFTU_SVC_INDEX)
-------------------------------------

An index extension adds pruning evidence to the index. While a plugin set
that registered it is loaded, every index build runs the extension's builder
over each file it indexes, in the same pass as the built-in extensions, and
stores the bytes the builder returns for each chunk and for the file. When a
query runs, the host asks the extension, for each filter leaf, which chunks'
bytes may hold a match, and reads only those chunks. The declarations are in
``dftracer/utils/plugins/abi/index.h``.

Register from the factory, which is the only place registration is allowed:

.. code-block:: c

   const dftu_svc_index* index =
       (const dftu_svc_index*)h->get_service(h->h, DFTU_SVC_INDEX);
   index->register_extension(ctx, h->h, "myplugin.range", &g_ext, NULL);

The name must be ``<plugin>.<name>``, and a name already registered in the
process fails the load. ``dftu_index_extension`` holds:

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Member
     - Contract
   * - ``version``, ``params_hash``
     - Data built with another version or params hash is not used, and the
       next build rebuilds it. ``params_hash`` identifies the configuration
       that shapes the bytes.
   * - ``max_payload``
     - The largest payload stored (0 means 1 MiB). A larger payload is
       dropped, so that chunk or file prunes nothing.
   * - ``make_builder`` / ``destroy_builder``
     - One builder per file, used by one thread. NULL stores nothing for the
       file.
   * - ``step``
     - The next record lines of the current chunk, borrowed for the call. A
       negative status stores nothing for the chunk or the file, and the host
       logs it once with the message set on the ctx.
   * - ``finish_granule`` / ``finish_file``
     - Append the chunk's or the file's payload through ``dftu_index_out``;
       the host owns the bytes. No bytes stores nothing, and a negative status
       stores nothing and is logged.
   * - ``compile`` / ``release``
     - Prepare a ``dftu_index_leaf`` (path, ``any``, operator, literal as a
       ``dftu_value``), or return NULL when the extension has no evidence for
       it.
   * - ``may_match`` / ``file_may_match``
     - 0 only when the payload proves that no record satisfies the leaf.
       ``file_may_match`` may be NULL.

Missing, stale or failed data, a chunk without a payload, and an extension
that is not registered all prune nothing, so an index built with a plugin
stays correct without it. Soundness of a 0 answer is the plugin's contract:
the host cannot check it, and a wrong 0 drops matching rows.

Load the plugin set for the build and for the queries that should use it,
and keep it loaded while they run: C++ ``Plugins::builder().add(path)``,
Python ``dftracer.utils.plugins.Plugins``, or
``dftracer_index --plugin path``. ``Indexer::manifest`` and ``explain`` list
the extension by name, and ``rebuild_extension`` and ``drop_extension``
accept it. ``tests/utilities/plugins/index_minmax_plugin.c`` is a complete
extension: per chunk min and max of one number.

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
   * - ``Host::spawn`` / ``all`` / ``tasks()``, typed ``Job``
     - ``DFTU_SVC_TASK``
     - `9. Async work and I/O`_
   * - ``Host::io()`` (typed ``Io``)
     - ``DFTU_SVC_IO``
     - `9. Async work and I/O`_
   * - ``Host::duql_compile`` / ``duql_matches``
     - ``DFTU_SVC_DUQL``
     - `10. duql filters against events (DFTU_SVC_DUQL)`_
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
   * - ``PluginBuilder::source<S>``
     - ``DFTU_SVC_PROVIDERS``
     - `14. Extending the engine: ops, sources, plan nodes`_
   * - ``PluginBuilder::node<N>``
     - ``DFTU_SVC_NODES``
     - `14. Extending the engine: ops, sources, plan nodes`_
   * - ``Host::emit_result_frame`` / ``emit_result_lazyframe``
     - ``DFTU_SVC_RESULT``
     - `14. Extending the engine: ops, sources, plan nodes`_
   * - ``PluginBuilder::state<T>``
     - ``DFTU_SVC_AGG`` (``register_state``)
     - `6. Mergeable aggregation (DFTU_SVC_AGG)`_
   * - none (raw ``get_service``)
     - ``DFTU_SVC_INDEX``
     - `15. Index extensions (DFTU_SVC_INDEX)`_

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
or walked up from a source checkout; override with the ``DFTRACER_UTILS_PLUGIN_INCLUDE``
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
