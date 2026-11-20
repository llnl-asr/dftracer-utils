:description: Scaffold, write, and run a C++ plugin by hand: build the fold against Host::agg and run it with dftracer_run.

Write a plugin by hand in C++
================================

:doc:`extending-the-engine` writes plugins in Python. This lesson is C++ and
goes one step further: scaffold the source
with the ``dftracer_plugin`` CLI, write the fold yourself against
``Host::agg`` and the ``agg::`` factories, and run it with ``dftracer_run``. The plugin model
(a fold over one fused scan, mergeable state, host-owned maps) is the one in
:doc:`extending-the-engine`.

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
               f'"ts":{1000 + i},"dur":10,"ph":1,"args":{{}}}}\n'
           )

That is 100 events for ``pid`` 2 (every third event) and 200 for ``pid`` 1.

2. Scaffold the source
--------------------------

``dftracer_plugin new <name> --cpp`` writes a template C++ plugin against
``plugin.h`` into the current directory (or ``-o DIR``):

.. code-block:: console

   $ dftracer_plugin new events_per_pid --cpp
   /path/to/events_per_pid.cpp

The generated file already compiles and links: a struct with a constructor
taking ``const Config&``, a ``dstd::Result<void> step(dataframe::FrameView,
Host)`` that counts events by ``pid`` into a host accumulator, an empty
``merge`` (the host owns and merges the accumulator for you), and a
``DFTU_PLUGIN`` body that registers the struct with ``p.fold<EventsPerPid>()``.
An error a ``step`` returns becomes the call's status and message. The rest of
this lesson rewrites its ``step`` with your own accumulator name and output
column.

3. Write the fold
---------------------

The scaffold already has the shape you want. Change its accumulator name to
``hits`` and its aggregate to ``events``, so the output table reads one row per
``pid`` with an ``events`` column, and ``events_per_pid.cpp`` holds:

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
               [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                   (void)config;
                   p.fold<EventsPerPid>();
                   return {};
               })

A ``step`` taking a ``dataframe::FrameView`` sees the whole batch the host
materialized from the scan, N rows in scan order being N events; the view is
borrowed for the call and copies nothing.
``h.agg(name, keys, cols)`` get-or-creates the host-owned mergeable accumulator
behind that name, grouping by the named key columns and computing one output
column per ``AggCol``; the ``agg::`` namespace has a factory per aggregate
(``count``, ``sum``, ``mean``, ``pct``, ``argmax``, ``busy``, ...), each taking
exactly the arguments its op consumes. The accumulator is write-only. The host
merges every worker's contributions and finalizes the result, so ``merge``
stays empty exactly as in the scaffold. ``plugin.h`` is header-only: a plugin
includes headers only, and the host process provides every function it calls.

4. Compile it
-----------------

``dftracer_plugin cflags`` prints the exact flags the CLI's own build step
uses. They select C++20, position-independent code and a shared library, and
add the plugin include directory it resolved. When that directory carries no
generated ``abi_version.h``, as in a source checkout, they add a second include
directory holding one, stamped with the plugin ABI version the host checks.
On macOS they add ``-undefined dynamic_lookup``, since a plugin
``.so`` is loaded into a host process rather than pre-linked against one:

.. code-block:: console

   $ dftracer_plugin cflags
   -std=c++20 -fPIC -shared -I/path/to/dftracer-utils/include -I/path/to/cache/abi_version_include

Either invoke your own compiler with those flags, or let the CLI do it:

.. code-block:: console

   $ dftracer_plugin build events_per_pid.cpp -o events_per_pid.so
   events_per_pid.so

5. Run it and read the result
---------------------------------

``dftracer_run`` folds the plugin over one shared, index-pruned parallel scan
and reports the scan on stderr:

.. code-block:: console

   $ dftracer_run --plugin ./events_per_pid.so plugin_trace.pfw.gz
   Run: plugins=1 | Files: 1 | Chunks: scanned=1 skipped=0 | Events: matched=300 scanned=300

The 300 scanned events are the fold's input; the per-``pid`` counts (200 for
``pid`` 1, 100 for ``pid`` 2) are merged in-process by the host, inside the
``hits`` accumulator. ``dftracer_run`` itself only reports the scan, not the
result. Use ``-d <directory>`` in place of ``--files`` to fold over a whole
tree instead of one file. To read the merged result programmatically, load the
same ``.so`` through Python's ``Plugins``. See :doc:`extending-the-engine`
and :doc:`../plugins`.

What you learned
-------------------

- ``dftracer_plugin new <name> --cpp`` scaffolds a compiling plugin source
  against ``plugin.h``; ``dftracer_plugin cflags`` prints the exact compile
  flags, and ``dftracer_plugin build <src>`` runs them for you.
- ``Host::agg`` and the ``agg::`` factories give a mergeable per-key
  accumulator over the same frame the scan produces, with no ``DFTU_AGG_*``
  codes to spell.
- ``dftracer_run --plugin <so> <trace>`` (or ``-d <dir>``) runs the
  compiled plugin over one shared scan with no Python involved.

For the plugin ABI in full (the ``reads`` column projection, the whole
aggregate op table, ports), see :doc:`../plugins`.
