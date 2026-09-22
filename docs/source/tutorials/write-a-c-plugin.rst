:description: Scaffold, write, and run a C++ plugin by hand: build the fold against the Batch/Event/Map wrappers and run it with dftracer_run.

Write a plugin by hand in C++
================================

:doc:`extending-the-engine` shows a plugin as one tab of a two-language
lesson. This one is C++ only, and goes one step further: scaffold the source
with the ``dftracer_plugin`` CLI, write the fold yourself against the
ergonomic ``Batch``/``Event``/``Map`` wrappers, and run it with
``dftracer_run``. It assumes :doc:`extending-the-engine`, so the plugin model
(a fold over one fused scan, mergeable state, host-owned maps) is not
re-explained here.

You need a C++ compiler on your ``PATH``; this lesson compiles the plugin to a
``.so`` once, ahead of time, rather than JIT-compiling it on first load like
the Python path does.

1. Build a trace with two processes
--------------------------------------

The plugin counts events per process, so the trace needs more than one
``pid``:

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

2. Scaffold the source
--------------------------

``dftracer_plugin new <name> --cpp`` writes a template C++ plugin against
``plugin.h`` into the current directory (or ``-o DIR``):

.. code-block:: console

   $ dftracer_plugin new events_per_pid --cpp
   events_per_pid.cpp

The generated file already compiles and links: a struct with a constructor
taking ``const Config&``, a ``step(const dftu_dataframe*, Host)`` that counts
events by ``pid`` into a host accumulator, an empty ``merge`` (the host owns
and merges the accumulator for you), and the ``dftracer_plugin`` factory that
``make_plugin<Slice>`` fills in. The rest of this lesson
replaces its body with the ergonomic ``Batch``/``Event`` row cursor instead of
reading the raw ``dftu_dataframe`` columns by hand.

3. Write the fold
---------------------

Replace ``events_per_pid.cpp`` with:

.. code-block:: cpp

   #include <dftracer/utils/plugins/plugin.h>

   #include <cstdint>

   using namespace dftracer::utils::plugins;
   // Prefer a short alias if you would rather qualify the SDK names:
   // namespace dp = dftracer::utils::plugins;

   struct EventsPerPid {
       explicit EventsPerPid(const Config&) {}

       void step(const dftu_dataframe* df, Host h) {
           h.agg("hits", {"pid"}, {agg::count("events")}).accumulate(df);
       }

       void merge(EventsPerPid&) {}
       void finalize(Host) {}
   };

   extern "C" dftu_plugin* dftracer_plugin(dftu_plugin_host* h,
                                           const dftu_value* config) {
       (void)h;
       return make_plugin<EventsPerPid>(config);
   }

A ``step`` taking a ``const dftu_dataframe*`` reads straight off the columns:
the host materializes each scanned batch into columns and hands the whole
batch over at once, N rows in scan order being N events.
``h.agg(name, keys, cols)`` get-or-creates the host-owned mergeable accumulator
behind that name, grouping by the named key columns and computing one output
column per ``AggCol``; the ``agg::`` namespace has a factory per aggregate
(``count``, ``sum``, ``mean``, ``pct``, ``argmax``, ``busy``, ...), each taking
exactly the arguments its op consumes. The accumulator is write-only - the host
merges every worker's contributions and finalizes the result, so ``merge``
stays empty exactly as in the scaffold. ``plugin.h`` is header-only; nothing
links against it.

4. Compile it
-----------------

``dftracer_plugin cflags`` prints the exact flags the CLI's own build step
uses - C++20, position-independent, shared, and the include directory it
resolved (plus ``-undefined dynamic_lookup`` on macOS, since a plugin ``.so``
is loaded into a host process rather than pre-linked against one):

.. code-block:: console

   $ dftracer_plugin cflags
   -std=c++20 -fPIC -shared -I/path/to/dftracer-utils/include

Either invoke your own compiler with those flags, or let the CLI do it:

.. code-block:: console

   $ dftracer_plugin build events_per_pid.cpp -o events_per_pid.so
   events_per_pid.so

5. Run it and read the result
---------------------------------

``dftracer_run`` folds the plugin over one shared, index-pruned parallel scan
and reports the scan on stderr:

.. code-block:: console

   $ dftracer_run --plugin ./events_per_pid.so --files plugin_trace.pfw.gz
   Run: plugins=1 | Files: 1 | Chunks: scanned=1 skipped=0 | Events: matched=300 scanned=300

The 300 scanned events are the fold's input; the per-``pid`` counts (200 for
``pid`` 1, 100 for ``pid`` 2) are merged in-process by the host, inside the
``hits`` accumulator - ``dftracer_run`` itself only reports the scan, not the
result. Use ``-d <directory>`` in place of ``--files`` to fold over a whole
tree instead of one file. To read the merged result programmatically, load the
same ``.so`` through Python's ``Plugins`` - see :doc:`extending-the-engine`
and :doc:`../plugins`.

What you learned
-------------------

- ``dftracer_plugin new <name> --cpp`` scaffolds a compiling plugin source
  against ``plugin.h``; ``dftracer_plugin cflags`` prints the exact compile
  flags, and ``dftracer_plugin build <src>`` runs them for you.
- The ergonomic surface - ``Batch``/``Event`` for zero-copy typed row
  iteration, ``Host::agg``/``Agg``/``agg::`` for a mergeable per-key
  accumulator - reads like ordinary C++ over the same ``dftu_dataframe`` ABI
  the raw scaffold uses.
- ``dftracer_run --plugin <so> --files <trace>`` (or ``-d <dir>``) runs the
  compiled plugin over one shared scan with no Python involved.

For the plugin ABI in full (the ``reads`` column projection, the whole
aggregate op table, ports), see :doc:`../plugins`.
