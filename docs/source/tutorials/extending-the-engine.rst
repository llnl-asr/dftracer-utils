:description: Lesson 3: author your own plugin, a fold over the engine's single fused scan, in Python or C++, run it, and read its result.

Extending the engine
====================

The built-in aggregates in :doc:`first-analysis` and :doc:`analysis-in-depth`
cover common needs, but sometimes you need a computation the aggregator does not
offer. A **plugin** is your own fold over the single fused scan the engine
already runs: you see every event once, accumulate into a mergeable state, and
the host handles the parallelism, merging, and materialization for you.

In this lesson you author one plugin - in Python or C++, your choice of tab -
run it, and read its result. This lesson assumes you have finished lessons 1
and 2.

Both languages need a C++ compiler on your PATH: the Python plugin is
AST-compiled to a cached native ``.so`` the first time you load it, and the C++
plugin you compile yourself.

1. Build a trace with two processes
-----------------------------------

Shared setup. The plugin will count events per process, so write a trace that
has more than one ``pid``:

.. code-block:: python

   import gzip

   with gzip.open("plugin_trace.pfw.gz", "wt") as f:
       for i in range(300):
           pid = 2 if i % 3 == 0 else 1
           f.write(
               f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
               f'"ts":{1000 + i},"dur":10,"ph":"X","args":{{}}}}\n'
           )

That is 100 events for ``pid`` 2 (every third event) and 200 for ``pid`` 1.

2. Author the plugin
--------------------

The state is the same in both languages: an accumulator grouped by ``pid``
counting the events in each group. The host runs a copy of the plugin per
worker, merges the accumulators, and materializes the result - you never write
reduce or threading code.

.. tab-set::

   .. tab-item:: Python

      A JIT plugin is a class decorated with ``@jit.plugin``. It declares one or
      more maps - a tuple key and an aggregate value - and a single
      ``@jit.each_event`` method that updates them per event. The key is a
      1-tuple ``(jit.i64,)`` because ``pid`` is one integer:

      .. code-block:: python

         from dftracer.utils import jit

         @jit.plugin
         class EventsPerPid:
             hits = jit.map(key=(jit.i64,), value=jit.count())

             @jit.each_event
             def step(self, e):
                 self.hits[(e.pid,)] += 1

      The ``count()`` aggregate is what lets the host merge per-worker copies
      for you.

   .. tab-item:: C++

      A batch arrives as one ``dftu_dataframe`` - N rows in scan order is N
      events - and an accumulator eats columns straight from it. Save this as
      ``events_per_pid.cpp``:

      .. code-block:: cpp

         #include <dftracer/utils/plugins/plugin.h>

         using namespace dftracer::utils::plugins;

         struct EventsPerPid {
             explicit EventsPerPid(const Config&) {}

             void step(const dftu_dataframe* df, Host h) {
                 const auto hits = h.agg("hits", {"pid"}, {agg::count("hits")});
                 if (hits) hits.accumulate(df);
             }

             void merge(EventsPerPid&) {}
             void finalize(Host) {}
         };

         extern "C" dftu_plugin* dftracer_plugin(dftu_plugin_host* h,
                                                 const dftu_value* config) {
             (void)h;
             return make_plugin<EventsPerPid>(config);
         }

      ``Host::agg`` names the accumulator, its key columns, and its aggregates;
      the ``agg::`` factories take exactly the fields each op uses, so you never
      spell a ``DFTU_AGG_*`` code or mis-fill a spec. ``agg_new`` is
      get-or-create, so calling it every batch is the normal shape.
      ``accumulate(df)`` folds the batch's ``pid`` column into the host-owned
      state: ``merge`` stays empty and per-worker copies are combined by the
      host, not by your code. ``plugin.h`` is header-only, so nothing links the
      library.

3. Run it and read the result
-----------------------------

.. tab-set::

   .. tab-item:: Python

      ``Plugins`` compiles the class to a cached ``.so`` and loads it in
      ``__init__``; ``run()`` folds it over one scan of your traces and
      returns a ``PluginRun`` holding the emitted results keyed by accumulator
      name plus the scan counters:

      .. code-block:: python

         from dftracer.utils.plugins import Plugins

         plugins = Plugins([EventsPerPid])
         run = plugins.run("plugin_trace.pfw.gz")

         table = run.results["hits"].to_pandas()
         print(table.sort_values("pid").reset_index(drop=True))
         print("scanned:", run.stats["events_scanned"])

      Expected output. The key column is ``pid`` and the aggregate column is
      ``hits`` (the count):

      .. code-block:: text

              pid  hits
           0    1   200
           1    2   100
           scanned: 300

      Pass a directory to ``run()`` and it folds over every ``.pfw.gz`` beneath
      it, exactly like ``TraceViewer``.

   .. tab-item:: C++

      Compile the plugin to a shared library, then fold it over the trace with
      the ``dftracer_run`` binary - no Python involved. Point ``-I`` at the
      installed headers:

      .. code-block:: console

         $ g++ -std=c++20 -shared -fPIC -I/path/to/dftracer-utils/include \
               events_per_pid.cpp -o events_per_pid.so
         $ dftracer_run --plugin ./events_per_pid.so --files plugin_trace.pfw.gz

      ``dftracer_run`` folds the plugin over one shared, index-pruned parallel
      scan and reports the scan on stderr:

      .. code-block:: text

         Run: plugins=1 | Files: 1 | Chunks: scanned=1 skipped=0 | Events: matched=300 scanned=300

      The 300 scanned events are the fold's input; the per-``pid`` accumulator
      is merged in-process by the host. Use ``-d <directory>`` in place of
      ``--files`` to fold over a whole tree. To consume the merged values
      programmatically (as the Python tab does), load the same ``.so`` through
      Python's ``Plugins``; see :doc:`../plugins`.

What you learned
----------------

- A plugin is a **fold over the one fused scan**: see each event once, keep
  mergeable state.
- Author it in Python with ``@jit.plugin`` + ``jit.map`` + ``@jit.each_event``,
  or in C++ against the ABI with a ``step``/``on_batch`` over the batch's
  ``dftu_dataframe`` and ``Host::agg``, exported by the ``dftracer_plugin``
  factory - both compile to the same ABI.
- Run it in Python with ``Plugins([...]).run(...)``, or compile the ``.so`` and
  fold it with ``dftracer_run --plugin``.

To go further, see the Extending the engine section of the
:doc:`../guides/index` and the plugin reference in :doc:`../plugins`.
