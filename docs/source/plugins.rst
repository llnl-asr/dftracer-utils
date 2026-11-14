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
  writers, sketches, and inter-plugin channels are fetched by id; a
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
   * - ``Host::memory()`` (``Reservation::set_reclaimer``)
     - ``DFTU_SVC_MEMORY``
     - a grow past the memory budget runs the reclaimer
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
      adapts errors at the boundary. The SDK throws nothing and catches
      nothing, and its headers compile with ``-fno-exceptions``. A call that
      can fail returns a ``dstd::Result``, and a callback returns a
      ``dstd::Result<void>`` (or ``void`` where it cannot fail); the SDK turns
      the error into the callback's status and message. A callback that throws
      terminates the process. A Slice that can fail at creation, for a bad
      config, provides ``static dstd::Result<Slice> from_config(const
      Config&)``, which the SDK uses in place of the constructor.
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
             return *out ? DFTU_OK : dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
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
(``dftracer/utils/dataframe/c_api.h``) - ``dftu_dataframe_column`` by name,
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

         dstd::Result<void> step(const Batch& b, Host h) {
             DFTRACER_UTILS_TRY(Agg edges,
                                h.agg("name_edges", {"pid", "name"},
                                      {agg::count("edges"),
                                       agg::sum("dur", "dur_sum")}));
             return edges.accumulate(b.raw());
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
(``dftracer/utils/dataframe/c_api.h``), the one part of the plugin surface that
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
         DFTRACER_UTILS_TRY(Agg a, h.agg("com.example.total", {},
                                         {agg::count("count")}));
         DFTRACER_UTILS_TRY_VOID(a.accumulate(b.raw()));

         // consumer, in on_finalize:
         DFTRACER_UTILS_TRY(OwnedDataFrame res,
                            h.agg_result("com.example.total"));
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

**Named results (DFTU_SVC_RESULT)** emit an opaque blob, a native frame or a
lazy plan under a name; all surface from ``Plugins::run`` keyed by that name.
Use ``Host::emit_result`` / ``emit_result_frame`` / ``emit_result_lazyframe``
(C: ``emit`` on ``dftu_svc_result``), best called at finalize.

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
      refuses, or a task that fails, ends the ``Task`` with its error: the
      ``co_await`` does not return, the host reports the status and message,
      and the frame is destroyed, which gives back every task its locals
      still own. ``co_await op.expected()`` returns the failure as a
      ``dstd::Result`` instead and the ``Task`` goes on. A ``Task`` ends when its body ends, or
      fails with ``co_await fail(error)``. ``co_await`` on a ``dstd::Result`` gives its value or ends
      the ``Task`` the same way; in a plain function, use
      ``DFTRACER_UTILS_TRY(decl, expr)`` and ``DFTRACER_UTILS_TRY_VOID(expr)``
      from ``dstd/result.h``, which return the error.

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

``DFTU_SVC_IO`` also has the calls for ``unlink``, ``rmdir``, ``rename``,
``mkdir``, ``linkat``, ``symlinkat``, ``fadvise``, ``socket``, ``connect``,
``bind``, ``listen``, ``shutdown``, ``sendmsg``, ``recvmsg``, ``poll_add``,
``statx``, ``fallocate``, ``sync_file_range``, ``splice``, ``openat2``, the four
``xattr`` calls, ``tee`` and ``pipe``. The host copies each path, name and
socket address when called; a buffer, message header or result slot stays valid
until the task finishes. ``statx``, ``fallocate``, ``sync_file_range``,
``splice``, ``openat2``, the ``xattr`` calls, ``tee`` and ``pipe`` need Linux;
elsewhere the call returns ``DFTU_E_UNSUPPORTED`` and starts no task.

``DFTU_SVC_FS`` (``Host::fs()``) is the runtime file system layer: ``read_file``,
``write_file``, ``read_at``, ``write_at``, ``list_dir``, ``find_all``,
``exists``, ``create_directories``, ``remove`` and ``stat_batch``. Each call
copies its paths and starts a task. ``read_file`` gives host-owned bytes that
the plugin frees through ``free_fn``. ``list_dir`` and ``find_all`` give a
host-owned ``dftu_path_list``, read with ``list_count`` and ``list_entry`` and
freed with ``list_free`` (the SDK ``PathList`` does that). ``stat_batch`` gives
one stat and one status per path, and a missing path is
``DFTU_E_NOT_FOUND`` in its slot, not a failed task.

``DFTU_SVC_JSON`` and ``DFTU_SVC_YAML`` (``Host::json()``, ``Host::yaml()``)
serve one table type, ``dftu_svc_format``, so a plugin reads and writes either
format with the same calls. The calls are synchronous. ``parse`` copies the text
and gives a ``dftu_doc``; ``root`` gives its ``dftu_value`` tree, built once and
valid until ``doc_free`` (the SDK ``Doc`` frees it). A parse error is
``DFTU_E_PARSE`` with the message ``line:column: reason``. A JSON number is
``I64`` or ``F64``; a YAML scalar is untyped text, so it is ``STR``, and ``NULL``
for an empty value, ``~`` or ``null``. The writer streams ``begin_object``,
``end_object``, ``begin_array``, ``end_array``, ``key``, ``i64``, ``f64``,
``str``, ``boolean``, ``null`` and ``raw``, and ``finish`` gives the text as
``dftu_bytes`` that the plugin frees through ``free_fn``. A call that breaks the
grammar, such as a value in an object without a key, returns ``DFTU_E_INVALID``
and a message, and leaves the writer as it was. Per-event JSON is not in this
service; the host columns read it.

``DFTU_SVC_MEMORY`` (``Host::memory()``, ``dftu_svc_memory``) is the host's
memory manager lent to a plugin. Every loaded plugin has one pool, named after
the plugin, so the bytes of its blocks and reservations show in the pool gauges
under that name. ``alloc``, ``realloc`` and ``free`` take the size and the
alignment and keep no header; a block still live when the plugin unloads is
reported as a leak. A slice that holds growing state reserves it instead of
reporting a size: ``reserve`` opens a reservation, ``try_grow`` adds bytes and
``shrink`` and ``release`` give them back (the SDK ``Reservation`` releases in
its destructor). ``try_grow`` is all or nothing. Past the scan's memory budget
or the process budget it returns ``DFTU_E_NO_MEMORY`` with a message and leaves
the reservation as it was. Before it fails, the host calls the reclaimer set
with ``set_reclaimer`` once with the number of bytes missing; the reclaimer
frees that much of the slice (writes part of it out, compacts, drops a cache)
and returns how many bytes it freed, which the host takes off the reservation,
and the grow then succeeds when that made room. A reservation the plugin leaves
at unload is released by the host, which logs an error. The service is served by
the run-time host that a fold callback receives and by the host that a
provider's ``scan`` or a node's ``open`` receives (see "The cursor host" under
section 13), not by the build-phase host. ``dftu_ctx`` scratch stays the tool
for short per-call buffers.

A host utility is reached as a named op instead
(``Host::run_op("dftu.hash.fnv1a", {column})``, ``DFTU_SVC_OPS``). Every
``dftu_svc_task`` call has an SDK form (``Host::spawn``, ``Host::tasks()``,
and the typed ``Job`` in ``job.h``), so async composition needs no raw ABI.
``dftu_svc_task::run`` blocks until a task ends, frees it and returns its
status; a node's input cursor uses it, since a node has no host to await with.
It uses no host state, so a plugin may keep the service pointer after the
factory returns.

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
             dstd::Result<void> step(const Batch& b, Host h) {
                 if (!q_) {
                     DFTRACER_UTILS_TRY(q_, h.duql_compile(F("dur") > 1000));
                     // Equivalent: h.duql_compile("dur > 1000");
                 }
                 for (std::int64_t row = 0; row < b.size(); ++row)
                     if (h.duql_matches(q_, b.raw(), row)) { /* ... */ }
                 return {};
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

11. Output writers
------------------

Two writer groups exist. The **trace writer (DFTU_SVC_TRACE)** appends events to
a gzip ``.pfw.gz`` and has an SDK wrapper on ``Host``; the index is built lazily
on first read, not at close.

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``Host::trace_open_write`` / ``trace_write`` / ``trace_close``, plus
      ``trace_read`` to scan an existing (auto-indexed) trace.

      .. code-block:: cpp

         dstd::Result<void> step(const Batch& b, Host h) {
             DFTRACER_UTILS_TRY(TraceWriter w,
                                h.trace_writer("/tmp/out.pfw.gz"));  // RAII
             DFTRACER_UTILS_TRY_VOID(w.write(b.raw()));
             return w.close();
         }

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         const dftu_svc_trace* T =
             (const dftu_svc_trace*)host->get_service(host->h, DFTU_SVC_TRACE);
         dftu_trace_writer* w = NULL;
         T->trace_open_write(ctx, host->h, "/tmp/out.pfw.gz", &w);
         T->trace_write(ctx, host->h, w, df);   /* every row of df, by column name */
         T->trace_close(ctx, host->h, w);

The **file service (DFTU_SVC_FILEIO)** gathers the host's file readers and
writers in one table. A reader or writer handle is owned by the host: a close
call ends it, and the host frees one that the plugin did not close at the end of
the fold (a gzip writer that was not closed is aborted and its files are
removed). Calls on one handle must not overlap.

- ``lines_open`` / ``lines_next`` / ``lines_close``: lines of a plain or gzip
  file (the host detects the ``1f 8b`` magic), from ``start_line`` to
  ``end_line`` (1-based; 0 means the first and the last line). ``lines_next``
  gives a batch of borrowed ``dftu_bytes`` that stays valid until the next call;
  the task status is ``DFTU_END`` at the end of the file. A last line with no
  newline is a line. A missing file is ``DFTU_E_NOT_FOUND``.
- ``chunks_open`` / ``chunks_next`` / ``chunks_close``: the bytes of a file in
  chunks, borrowed until the next call, ``DFTU_END`` at the end.
- ``gz_open`` / ``gz_append`` / ``gz_close``: a gzip line writer.
  ``gz_append`` copies whole lines (data must end with ``'\n'``, else
  ``DFTU_E_INVALID``); ``gz_close`` gives a ``dftu_gz_summary``.
- ``compress`` / ``decompress``: whole-file gzip.
- ``logical_path`` / ``mtime`` / ``size`` / ``sampled_hash``: file helpers; a
  missing file is ``DFTU_E_NOT_FOUND``.
- The **parallel writer** is a sharded, multi-worker gzip-member output
  (``writer_create`` / ``writer_open`` / ``writer_chunk`` / ``writer_close`` /
  ``merge_shards``). It replaces the former ``dftu.svc.writer`` service.

.. tab-set::

   .. tab-item:: C++ (SDK)

      ``Host::fileio()`` returns a ``Fileio`` with a ``LineReader`` and a
      ``ChunkReader`` that close in their destructors, and a ``GzWriter`` that
      the plugin closes with ``co_await close()`` (one left open is aborted at
      the end of the fold and its files are removed);
      ``co_await reader.next()`` gives the next batch, or ``nullopt`` at the end.
      ``Host::writer`` returns the ``Writer`` of the parallel writer as a
      ``dstd::Result`` (host-owned, nothing to free); ``open`` / ``chunk`` / ``close`` are ``co_await``-able
      AsyncOps, and ``Host::merge_shards`` concatenates shard files into a
      target.
      ``examples/plugins/duration_report.cpp`` is a full plugin that writes its
      report with ``GzWriter``.

      .. code-block:: cpp

         Task on_finalize(Host h) {
             auto reader = co_await h.fileio().lines("/tmp/in.pfw");
             for (;;) {
                 auto batch = co_await reader.next(1024);
                 if (!batch) break;
                 for (std::string_view line : *batch) { /* ... */ }
             }

             auto gz = co_await h.fileio().gz("/tmp/out.pfw.gz");
             co_await gz.open();
             co_await gz.append("{\"name\":\"x\"}\n");
             dftu_gz_summary summary = co_await gz.close();

             Writer w = co_await h.writer("/tmp/out2.pfw.gz", /*worker_count=*/4,
                                          /*gzip=*/true);
             co_await w.open();
             co_await w.chunk(/*worker=*/0, std::string_view{"payload"});
             co_await w.close();
         }

   .. tab-item:: C (raw ABI)

      .. code-block:: c

         const dftu_svc_fileio* F =
             (const dftu_svc_fileio*)host->get_service(host->h, DFTU_SVC_FILEIO);
         dftu_line_reader* r = NULL;
         F->lines_open(ctx, host->h, "/tmp/in.pfw", 0, 0, &r);
         /* await F->lines_next(ctx, host->h, r, 1024, &batch, &task) until
            its status is DFTU_END, then: */
         F->lines_close(host->h, r);

         dftu_writer* w = NULL;
         F->writer_create(ctx, host->h, "/tmp/out.pfw.gz",
                          /*num_workers=*/4, /*gzip=*/1, &w);
         /* co-await F->writer_open / writer_chunk / writer_close (tasks) */

12. Getting results back
------------------------

Each host-owned accumulator is finalized at scan end to a table keyed by the
accumulator's name, with the key columns first and one column per aggregate;
interned string ids are resolved to their labels once, so a string key column is
a real ``string``. Named results (section 8) surface the same way, keyed by their
emit name.

**From Python**, build a fixed set of plugins and run them over a single fused
scan; each result comes back shaped by its emit kind (a native
:class:`~dftracer.utils.DataFrame`, a
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

   using dftracer::utils::plugins::host::Plugins;

   auto set = Plugins::builder()
                  .add("./name_edges.so", R"({"duql": "cat == \"POSIX\""})")
                  .add("./dur_stats.so")
                  .build();                      // dstd::Result<Plugins>: a bad path,
   if (!set) throw std::runtime_error(set.error().message);   // ABI or graph fault
   PluginRun run = co_await set->run(View::from_files(files));
   // NamedResult is a variant: bytes, OwnedDataFrame, OwnedLazyFrame.
   auto& table = std::get<OwnedDataFrame>(run.results.results().at("name_edges"));

**From the command line**, ``dftracer_run`` folds every ``--plugin`` over one
shared, index-pruned scan and reports the scan on stderr (it does not print
result tables - read those through :class:`~dftracer.utils.plugins.Plugins`):

.. code-block:: bash

   dftracer_run --plugin ./name_edges.so trace.pfw.gz
   dftracer_run --plugin ./a.so --plugin ./b.so ./traces      # one shared scan

Use ``-d <directory>`` for a whole tree instead of ``--files``.

13. Extending the engine: ops, sources, plan nodes
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
plugin does not link the dataframe ABI to use the engine. A scalar UDF plugin
is the exception: it builds its result column with ``dftu_series_new_*``, so it
links the dataframe library, and the process must hold exactly one copy of that
library, the one the host runs on, because the host frees the column.

.. code-block:: cpp

   static dftu_series* zscore(const dftu_series* v);   // series -> series
   static const dftu_op_desc ZSCORE = {"me.zscore",
       DFTU_OP_SIG(SERIES, SERIES, NONE, NONE), (dftu_op_generic_fn)&zscore};
   return plugin(ctx, h, config).op(ZSCORE).fold<MySlice>().build(out);

**A scalar UDF** (``dftu_svc_ops::register_udf``). A function that duql
calls by name, such as ``to_ms(dur)`` or ``is_slow(dur, 1000)``, in a ``derive``,
``select``, ``where``, join key or any other expression. Unlike an op, its name
is the duql name as the query spells it, with no ``<plugin>.`` prefix. A name
that a built-in function or another UDF holds fails the load with a message.
A ``dftu_udf_desc`` (``dataframe/c_api_udf.h``) has the name, the argument
counts ``min_args`` and ``max_args``, flags and three callbacks. ``bind`` runs
once per call site with the argument types (``dftu_type_info``) and, for each
argument the query writes as a constant, its ``dftu_scalar``; it picks the result
type and may keep state in ``bound``. ``exec`` runs on one batch: the argument
columns, borrowed, and the row count in, one new column of that many rows and
the bound type out. It never runs per row. ``free_bound`` releases ``bound``.
A UDF takes and gives Bool, the integer and float types, String and Binary.
Bool columns are bit-packed, one bit per row, least significant bit first.
The registration serves every surface. Python calls the UDF as
``dft.udf["to_ms"](dft.col("dur"))`` or ``dft.call("to_ms", dft.col("dur"))``,
C++ as ``dataframe::udf("to_ms")->operator()(col(0))``, and ``dftu_op_find("to_ms")``
finds a record of it in the op registry. That record has no function to run:
the UDF runs through duql and expressions, which bind it.

``exec`` may run on several batches at once, so it reads ``bound`` and writes
only what it owns. By default the UDF is deterministic and a row with a null
argument gives a null result: the host nulls those rows after ``exec``, which
may compute anything for them. ``DFTU_FN_NULL_AWARE`` passes the columns as they
are and keeps the result's validity. ``DFTU_FN_VOLATILE`` marks a result that
can differ for the same arguments; the flag is advisory. A UDF call is never
folded to a constant either way, and a ``lookup`` side that calls a UDF is not
stored in the lookup cache. A ``bind`` that fails owns its own cleanup: the host
ignores ``*bound`` and calls ``free_bound`` only for a ``bind`` that succeeded.

A long ``exec`` polls ``dftu_check`` once per few thousand rows and returns its
status. A cancelled query, such as one past a deadline, then stops within one
batch; a UDF that never polls delays the stop until its batch is done. An error
status with a message from ``exec`` fails the query with that message.
duql has no join predicate, so a UDF joins as a key function, and a Bool UDF as
a key too: ``lookup flags on is_slow(dur, 1000) == slow``.

.. code-block:: cpp

   struct ToMs {                                  // plugins/plugin/udf.h
       dftu_dtype type;
       static dstd::Result<dstd::UniquePtr<ToMs>> bind(pl::UdfBind& bind);
       dstd::Result<dftu_series*> exec(pl::UdfBatch& batch) const;
   };
   return plugin(ctx, h, config).udf<ToMs>("to_ms").build(out);

``examples/plugins/duration_udfs.cpp`` is a full plugin with two UDFs.

**A UDF in the same process.** Code that is not a plugin registers the same
class with ``dataframe::make_udf<T>()`` (``dataframe/udf.h``). ``T`` is the
class a plugin passes to ``PluginBuilder::udf``, plus a static ``NAME`` and
optional ``MIN_ARGS``, ``MAX_ARGS`` and ``FLAGS``; both paths build the
``dftu_udf_desc`` through one adapter. The result is a
``dstd::Result<dataframe::Udf>``: a clash with a built-in function, another UDF
or an op is an error with a message.

.. code-block:: cpp

   struct PlusOne {
       static constexpr const char* NAME = "plus_one";
       static dstd::Result<dstd::UniquePtr<PlusOne>> bind(dataframe::UdfBind&);
       dstd::Result<dftu_series*> exec(dataframe::UdfBatch&) const;
   };
   auto plus_one = dataframe::make_udf<PlusOne>();   // owns the registration
   Expr e = (*plus_one)(col(0));                     // Udf is callable

The ``Udf`` that ``make_udf`` returns owns the registration: when it is
destroyed the name leaves the registry, so a later lookup, query or
``dftu_op_find`` finds nothing. A plan or expression that took the UDF before
keeps it and runs. A ``Udf`` is move only, and moving it keeps one
registration. ``dataframe::udf(name)`` gives a ``Udf`` that only looks the name
up, so destroying it never unregisters, and fails with NotFound for an unknown
name.

**A user-defined aggregate** (``dftu_svc_ops::register_udaf``). A function
that keeps one state per group, such as ``p95(dur)``. Its name follows the
rules of a UDF name, and a name that a built-in function or aggregate, an op, a
UDF or another aggregate holds fails the load with a message. A
``dftu_udaf_desc`` (``dataframe/c_api_udaf.h``) has the name, the argument
counts, flags, ``state_size`` and ``state_align``, and the callbacks. The host
owns the memory of every state, in blocks that never move while the table
grows. ``bind`` runs for a group-by and again for each state set the host
builds apart from the first (a partial fed on another thread, states read back
from a spill). ``init`` makes new states
ready, ``update`` feeds a batch of rows to the states of their groups
(``states[i]`` is the state of row ``i``), ``combine`` merges states, and
``finalize`` gives one result row per state. They run over arrays and columns,
never row by row. ``free_bound`` releases ``bound``, after the last state.

Without ``DFTU_AGG_NULL_AWARE`` the host passes a NULL state pointer for a row
where any argument is null, so ``update`` skips it; with the flag the plugin
sees every row. ``DFTU_AGG_STATE_POD`` marks a state that is plain bytes: the
host zero-fills it, and may copy it to spill or move it, so it needs no
``destroy``, ``serialize`` or ``deserialize``. Any other state needs all
three: ``destroy`` runs on every state that ``init`` made, and ``serialize``
writes a state as bytes with a two-call size query (a NULL ``out``, or a
``cap`` that is too small, only sets ``*size``) for a spill or a merge across
processes. A UDAF with neither a POD state nor the serialize pair fails the
load. ``DFTU_AGG_PARALLEL_SAFE`` lets the host feed several state sets on
different threads and merge them with ``combine``; one set is never used by
two threads at once. Without the flag no callback of the UDAF runs on two
threads at once (``bind``, ``init``, ``update``, ``combine``, ``finalize``,
``destroy``, ``serialize``, ``deserialize`` and ``free_bound``), so a group-by
with such a UDAF runs on one state set. ``DFTU_AGG_HAS_RETRACT`` needs ``retract`` and ``value``
for a sliding window.

A UDAF is also a window function: a ``window`` block of duql calls it with
``over`` (``usum(x) over 10 rows``, ``usum(x) over 5s``), and the C ABI runs it
as ``DFTU_WINDOW_FRAME_UDAF`` with ``udaf``, ``args`` and ``arg_count`` in the
frame of a ``dftu_window_spec``. With ``DFTU_AGG_HAS_RETRACT`` the host slides
the frame. It cuts a partition into slices of at least 1024 rows wherever the
frame is no longer than the slice so far (a running frame stays one slice),
keeps one state per slice and moves every slice one row per step, so ``update``
takes the
rows that join the frames of all slices in one call, ``retract`` takes the rows
that leave them in one call (with the same null pointers as ``update``) and
``value`` gives the result of each frame. A row costs O(1) rows of ``update``
and ``retract`` and one ``value`` call, whatever the frame size. A UDAF without
them is merged from a tree: the host feeds each row to a state of its own with
``update``, merges the states of two neighbors into a state of the next level
with ``combine``, and gives each frame a fresh state that ``combine`` merges
the O(log w) tree states tiling the frame into, from left to right, so
``combine`` must keep the order of the rows if the result depends on it. A
``combine`` leaves its ``from`` state unchanged, because a tree state feeds
many frames. ``finalize`` gives the results of a chunk of frames at once. A
frame of at most 8 rows of a ``ROWS`` frame is not worth a tree: each such frame
gets a state of its own, fed its rows by ``update``. The tree holds about two
states per row of a partition, so a state that grows with its rows (a list of
values) holds O(n log n) values of it. A frame with no fed row, or fewer than the
``min_count`` the query sets, gives null without calling ``value``. A ``window``
entry with no ``over`` runs the UDAF over each whole partition as ``group``
does. A window over a stream is cut at partition boundaries, as for the
built-in frame functions that need the whole partition.

``DataFrame::group_by``, ``LazyFrame::group_by`` and ``group_by_dynamic`` run a
UDAF as a ``GroupAgg`` with ``op = Agg::Udaf``, ``fn`` the registered name,
``column`` the first argument and ``args`` the others. The C ABI does the same:
an ``op`` name of ``dftu_dataframe_group_by`` that no built-in aggregate has
resolves to the registered UDAF of that name, with ``column`` as its one
argument. The argument types are fixed by the first batch, and a later batch
with another type fails. A group-by that spills writes each group state with
``serialize`` and merges the runs with ``combine``. A duql ``group`` or ``agg``
block calls a UDAF by name, as ``n = usum(size)`` (see
:ref:`duql-plugin-udafs`). A duql ``window`` block runs a UDAF over a frame,
as described above.

Python calls a UDAF from ``group_by(...).agg``. ``dft.udaf`` is the read-only
registry, like ``dft.udf``: ``dft.udaf["p95"]`` is a ``Udaf`` with ``name``,
``min_args``, ``max_args`` and ``flags``, and an unknown name raises
``KeyError``. Calling it with column expressions gives an ``Agg`` that takes
``.alias(...)`` or a keyword name, and the output column is ``<name>_<column>``
by default. Each argument must be a bare column. A wrong argument count raises
``TypeError``, and a failure in the UDAF raises ``ValueError``. The group-by finds
the UDAF by name when it runs, so the plugin must still be loaded then.
``LazyFrame.group_by`` and ``group_by_dynamic`` (lazy and eager) take the same
``Agg``; their arguments must be bare columns, or they raise ``ValueError``.

.. code-block:: python

   df.group_by("pid").agg(dft.udaf["p95"](dft.col("dur")).alias("dur_p95"))

.. code-block:: cpp

   struct P95 {                                   // plugins/plugin/udaf.h
       struct State { Sketch sketch; };           // not trivially copyable
       static dstd::Result<dstd::UniquePtr<P95>> bind(pl::UdfBind& bind);
       dstd::Result<void> update(pl::UdafStateSet<State>& states,
                                 const pl::UdfBatch& batch) const;
       dstd::Result<void> combine(State& into, const State& from) const;
       dstd::Result<dftu_series*> finalize(pl::UdafStateSet<State>& states) const;
       void serialize(const State& state, dstd::String& out) const;
       dstd::Result<void> deserialize(State& state, dstd::StringView in) const;
   };
   return plugin(ctx, h, config).udaf<P95>("p95").build(out);

A ``State`` that is trivially copyable is a POD state and needs no
``serialize`` pair. Code that is not a plugin registers the same class, plus a
static ``NAME``, with ``dataframe::make_udaf<A>()`` (``dataframe/udaf.h``);
the ``dataframe::Udaf`` it returns owns the registration, as ``Udf`` does, and
``dataframe::udaf(name)`` only looks a name up.

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
then scans it like a file, from any language. The vtables live in
``plugins/abi/providers.h``; the host wraps one as a C++ provider in the
dataframe registry, so the dataframe library itself knows no plugin type. An optional ``apply`` callback lets the source
absorb whole ops at plan time (filter, projection, aggregation, sort,
top-N, limit, tail, join); see the planning hooks in
:doc:`guides/data/lazyframe`.

In C++, write the source as a class and let ``plugins/plugin/source.h``
build the vtables. The class names its columns and scans; each planning hook
is optional, and only the ones it defines are wired up. Requests arrive as
views (``ExprView`` with ``as_compare`` / ``as_logical`` / ``as_not`` /
``as_str_pred`` / ``as_is_in``, ``SortView``, ``AggregationView``,
``JoinView<S>`` with the other side as ``S``), and results are owned C++
values (``OwnedFrame``, ``Applied<S>``). ``scan``, ``next`` and the planning
hooks return a ``dstd::Result``, and an error fails the call with its status
and message. A failed cursor fails the scan with it; for a failed planning hook
the host logs the message and applies the op itself. ``scan`` also receives the
cursor's ``Host``; a cursor whose ``next`` takes ``(std::int64_t, const Host&)``
in place of ``(std::int64_t)`` gets a ``Host`` that is good for that call.

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
       pl::ScanResult<Cursor> scan(const pl::ScanView&, const pl::Host&) const;
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

**The cursor host.** A provider's ``scan`` and a node's ``open`` receive a
``const dftu_plugin_host*`` that stays valid until the cursor's ``destroy`` has
returned. It serves the services of a fold callback's host that need no scan:
``DFTU_SVC_MEMORY``, ``DFTU_SVC_IO``, ``DFTU_SVC_FS``, ``DFTU_SVC_JSON``,
``DFTU_SVC_YAML``, ``DFTU_SVC_SKETCH``, ``DFTU_SVC_OPS`` and
the registration groups, which refuse at run time. It returns NULL for the
services that belong to one scan: ``DFTU_SVC_TASK``, ``DFTU_SVC_FILEIO``,
``DFTU_SVC_AGG``, ``DFTU_SVC_RESULT``, ``DFTU_SVC_PORTS``, ``DFTU_SVC_DUQL`` and
``DFTU_SVC_TRACE``, and its ``resolve`` and ``intern`` find nothing, since a
cursor has no intern table. A cursor reserves memory through it like a fold
slice, and the reservations are charged to the pool of the plugin, one budget
shared with its folds. When another holder of the process budget asks the pool
for bytes, the host runs the reclaimers of the cursor's reservations between two
of its ``next()`` calls, on the thread that drives it. A reservation the cursor
still holds when its ``destroy`` returns is released by the host, which logs an
error.

**Library lifetime.** The host keeps a plugin's shared library loaded while
anything made from it is alive: a registered node, provider, index
extension, op, UDF or UDAF, and the plans and cursors that hold one. Unloading the plugin
removes its names at once, so no new lookup finds them. The plugin's own
``destroy`` runs after the last holder is gone, and the library is closed
after it; a node or provider calls its own ``destroy(self)`` before that, so it
may use the plugin's global state. A plugin never counts
references itself, since the host takes them. An op registered by a plugin's
factory is covered the same way; an op registered with ``dftu_op_register``
from plain C code is not tied to any library.

**A plan node** (``DFTU_SVC_NODES``; the ``dftu_node_vt`` is in
``plugins/abi/nodes.h``). Register it from the factory through
the service: the host gates the name to ``<plugin>.<name>`` and unregisters
the node when the plugin unloads, so no new plan finds it. A
``dftu_node_vt`` is a cursor that takes an input cursor and returns an
output cursor: a streaming op (one morsel in, one out, state kept between
morsels: sessionize, coalesce, a GPU kernel) or a breaker (drain the input
on the first ``next``, emit one frame). ``output_schema`` declares its
columns from the input's, so the optimizer can type the plan around it; it
may not push a filter or projection through the node, and ``explain`` names
it. ``LazyFrame::op(name, args)`` / ``dftu_lazyframe_op`` stacks it on a
plan like any built-in step. The cursor's optional slots let it take part in
the plan's machinery: ``narrow`` forwards a join's key set to its input,
and ``open`` receives the plan's memory budget in bytes and the cursor host,
through which the cursor reserves memory
(:doc:`guides/runtime/memory-budget`).

In C++, ``plugins/plugin/node.h`` does the same for a node class:
``output_schema(SchemaView in, OpArgs args, SchemaBuilder& out)`` and
``open(InputCursor in, OpArgs args, std::uint64_t memory_budget, const Host& host)``
returning a cursor like a source's. ``memory_budget`` is the plan's budget in bytes,
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
                                    std::uint64_t, const pl::Host&) const {
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

``tests/plugins/reference_plugin.c`` is the conformance plugin: it
touches every service slot, registers an op, a source, a state and a node,
and is the one file to read for the exact C of each.

14. Index extensions (DFTU_SVC_INDEX)
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
accept it. ``tests/plugins/index_minmax_plugin.c`` is a complete
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
   * - ``Host::emit_result``
     - ``DFTU_SVC_RESULT``
     - `8. Inter-plugin communication`_
   * - ``Host::spawn`` / ``all`` / ``tasks()``, typed ``Job``
     - ``DFTU_SVC_TASK``
     - `9. Async work and I/O`_
   * - ``Host::io()`` (typed ``Io``: file, directory, link, socket, poll,
       xattr, splice and pipe calls; the Linux-only ones answer
       ``DFTU_E_UNSUPPORTED`` elsewhere)
     - ``DFTU_SVC_IO``
     - `9. Async work and I/O`_
   * - ``Host::fs()`` (typed ``Fs``, RAII ``PathList``)
     - ``DFTU_SVC_FS``
     - `9. Async work and I/O`_
   * - ``Host::json()`` / ``Host::yaml()`` (typed ``Json``, ``Yaml``, RAII
       ``Doc``)
     - ``DFTU_SVC_JSON``, ``DFTU_SVC_YAML``
     - `9. Async work and I/O`_
   * - ``Host::memory()`` (typed ``Memory``, RAII ``Reservation``)
     - ``DFTU_SVC_MEMORY``
     - `9. Async work and I/O`_
   * - ``Host::duql_compile`` / ``duql_matches``
     - ``DFTU_SVC_DUQL``
     - `10. duql filters against events (DFTU_SVC_DUQL)`_
   * - ``Host::trace_open_write`` / ``trace_write`` / ``trace_read``
     - ``DFTU_SVC_TRACE``
     - `11. Output writers`_
   * - ``Host::fileio()`` (typed ``Fileio``), ``Host::writer``,
       ``Host::merge_shards``
     - ``DFTU_SVC_FILEIO``
     - `11. Output writers`_
   * - ``Host::run_op`` / ``run_op_frame`` / ``run_op_aggregate``,
       ``PluginBuilder::op``, ``OwnedLazyFrame``
     - ``DFTU_SVC_OPS``
     - `13. Extending the engine: ops, sources, plan nodes`_
   * - ``PluginBuilder::source<S>``
     - ``DFTU_SVC_PROVIDERS``
     - `13. Extending the engine: ops, sources, plan nodes`_
   * - ``PluginBuilder::node<N>``
     - ``DFTU_SVC_NODES``
     - `13. Extending the engine: ops, sources, plan nodes`_
   * - ``Host::emit_result_frame`` / ``emit_result_lazyframe``
     - ``DFTU_SVC_RESULT``
     - `13. Extending the engine: ops, sources, plan nodes`_
   * - ``PluginBuilder::state<T>``
     - ``DFTU_SVC_AGG`` (``register_state``)
     - `6. Mergeable aggregation (DFTU_SVC_AGG)`_
   * - none (raw ``get_service``)
     - ``DFTU_SVC_INDEX``
     - `14. Index extensions (DFTU_SVC_INDEX)`_

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
