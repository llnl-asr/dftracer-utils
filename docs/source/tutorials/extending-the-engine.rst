:description: Lesson 3: author your own plugin, a fold over the engine's single fused scan, in Python or C++, run it, and read its result.

Extending the engine
====================

The built-in aggregates in :doc:`first-analysis` and :doc:`analysis-in-depth`
cover common needs, but sometimes you need a computation the aggregator does not
offer. A **plugin** is your own fold over the single fused scan the engine
already runs: you see every event once, accumulate into a mergeable state, and
the host handles the parallelism, merging, and materialization for you.

In this lesson you author one plugin, in Python or C++ (your choice of tab),
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
worker, merges the accumulators, and materializes the result. You never write
reduce or threading code.

.. tab-set::

   .. tab-item:: Python

      A JIT plugin is a class decorated with ``@jit.plugin``. It declares one or
      more maps (a tuple key and an aggregate value) and a single
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

      A batch arrives as one borrowed ``dataframe::FrameView``, where N rows in
      scan order are N events. An accumulator eats it straight. Save this as
      ``events_per_pid.cpp``:

      .. code-block:: cpp

         #include <dftracer/utils/plugins/plugin.h>

         using namespace dftracer::utils;
         using namespace dftracer::utils::plugins;

         struct EventsPerPid {
             explicit EventsPerPid(const Config&) {}

             dstd::Result<void> step(dataframe::FrameView df, Host h) {
                 DFTRACER_UTILS_TRY(
                     Agg hits, h.agg("hits", {"pid"}, {agg::count("events")}));
                 return hits.accumulate(df);
             }

             void merge(EventsPerPid&) {}
             void finalize(Host) {}
         };

         DFTU_PLUGIN("events_per_pid", "1.0",
                     [](PluginBuilder& p, const Config& config)
                         -> dstd::Result<void> {
                         (void)config;
                         p.fold<EventsPerPid>();
                         return {};
                     })

      ``Host::agg`` names the accumulator, its key columns, and its aggregates;
      the ``agg::`` factories take exactly the fields each op uses, so you never
      spell a ``DFTU_AGG_*`` code or mis-fill a spec. ``Host::agg`` is
      get-or-create, so calling it every batch is the normal shape, and
      ``DFTRACER_UTILS_TRY(decl, expr)`` binds its ``Agg`` or returns its
      error, which the host reports as the step's message.
      ``accumulate(df)`` folds the batch's ``pid`` column into the host-owned
      state: ``merge`` stays empty and per-worker copies are combined by the
      host, not by your code. ``plugin.h`` is header-only, so a plugin links no
      library of the engine: the host process provides every function it calls.

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
         print(table.sort_values("k0").reset_index(drop=True))
         print("scanned:", run.stats["events_scanned"])

      Expected output. A JIT map names its key columns ``k0``, ``k1``, ...
      and a single aggregate ``value``:

      .. code-block:: text

             k0  value
          0   1    200
          1   2    100
          scanned: 300

      Pass a directory to ``run()`` and it folds over every ``.pfw.gz`` beneath
      it, exactly like ``TraceViewer``.

   .. tab-item:: C++

      Compile the plugin to a shared library, then fold it over the trace with
      the ``dftracer_run`` binary with no Python involved. Point ``-I`` at the
      installed headers:

      .. code-block:: console

         $ g++ -std=c++20 -shared -fPIC -I/path/to/dftracer-utils/include \
               events_per_pid.cpp -o events_per_pid.so
         $ dftracer_run --plugin ./events_per_pid.so plugin_trace.pfw.gz

      On macOS add ``-undefined dynamic_lookup`` to the ``g++`` line, since the
      plugin is loaded into the host process rather than pre-linked against it;
      ``dftracer_plugin build events_per_pid.cpp`` runs the exact flags for your
      platform.

      ``dftracer_run`` folds the plugin over one shared, index-pruned parallel
      scan and reports the scan on stderr:

      .. code-block:: text

         Run: plugins=1 | Files: 1 | Chunks: scanned=1 skipped=0 | Events: matched=300 scanned=300

      The 300 scanned events are the fold's input; the per-``pid`` accumulator
      is merged in-process by the host. Use ``-d <directory>`` in place of
      ``--files`` to fold over a whole tree. To read the merged values, load
      the same ``.so`` through Python's ``Plugins``. The key column keeps the
      name ``pid`` and the aggregate is named ``events``:

      .. code-block:: python

         from dftracer.utils.plugins import Plugins

         run = Plugins(["./events_per_pid.so"]).run("plugin_trace.pfw.gz")
         print(run.results["hits"].to_pandas().sort_values("pid").reset_index(drop=True))

      .. code-block:: text

            pid  events
         0    1     200
         1    2     100

      See :doc:`../plugins`.

What you learned
----------------

- A plugin is a **fold over the one fused scan**: see each event once, keep
  mergeable state.
- Author it in Python with ``@jit.plugin`` + ``jit.map`` + ``@jit.each_event``,
  or in C++ with a ``step`` over the batch's ``dataframe::FrameView`` and
  ``Host::agg``, registered by a ``DFTU_PLUGIN`` body with ``p.fold<Slice>()``.
  Both compile to the same ABI.
- Run it in Python with ``Plugins([...]).run(...)``, or compile the ``.so`` and
  fold it with ``dftracer_run --plugin``.

To go further, see the Extending the engine section of the
:doc:`../guides/index` and the plugin reference in :doc:`../plugins`.
