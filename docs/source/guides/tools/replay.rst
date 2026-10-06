:description: Re-execute the I/O recorded in a trace, doing real I/O or sleeping per op, with dftracer_replay or the C++ ReplayEngine.

Replay a trace
==============

.. admonition:: Goal
   :class: goal

   Re-execute the I/O operations recorded in a trace - either doing the real
   I/O or sleeping for each operation's duration - to reproduce a workload's timing
   and load. Use the ``dftracer_replay`` binary for the common case, or the C++
   ``ReplayEngine`` when you want to drive it from your own code.

This is C++ and CLI. There is no Python binding.

Command line
------------

.. code-block:: bash

   # Replay a directory, honoring the original timing
   dftracer_replay ./traces -r

   # Sleep-based replay (no real I/O), as fast as possible
   dftracer_replay ./traces -r --dftracer-mode --no-sleep

   # Analyze only, do no I/O
   dftracer_replay ./traces --dry-run

The inputs are one or more ``.pfw`` or ``.pfw.gz`` files or directories.

Selected flags
~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 34 16 50

   * - Flag
     - Default
     - Meaning
   * - ``inputs``
     - (required)
     - Trace files or directories.
   * - ``-r``, ``--recursive``
     - off
     - Recurse into directories.
   * - ``--no-timing``
     - off
     - Ignore original timing; run as fast as possible.
   * - ``--dry-run``
     - off
     - Parse and analyze without executing operations.
   * - ``--dftracer-mode``
     - off
     - Sleep for each operation's duration instead of doing real I/O.
   * - ``--no-sleep``
     - off
     - With ``--dftracer-mode``, skip the sleeps (max speed).
   * - ``--filter-function`` / ``--exclude-function``
     - ``""``
     - Comma-separated function names to keep or drop (e.g. ``read,write``).
   * - ``--filter-category`` / ``--exclude-category``
     - ``""``
     - Comma-separated categories (e.g. ``POSIX,storage``).
   * - ``--filter-pid`` / ``--filter-tid``
     - ``""``
     - Comma-separated PIDs / TIDs to keep (matching ``--exclude-*`` drop).
   * - ``--start-timestamp`` / ``--end-timestamp``
     - ``0`` / max
     - Time window to replay.
   * - ``--min-size`` / ``--max-size``
     - ``-1``
     - Operation size bounds in bytes.
   * - ``--sample-rate``
     - ``1.0``
     - Fraction of events to replay (``0.1`` = 10%).
   * - ``--max-events``
     - ``0``
     - Cap on events replayed (0 = unlimited).

Some flags require others: ``--no-sleep`` needs ``--dftracer-mode``;
``--hierarchical-replay`` needs ``--use-call-tree``; ``--respect-call-hierarchy``
needs ``--hierarchical-replay``. The process exits ``0`` when at least one event
executed and none failed, ``1`` when nothing executed, and ``2`` when any event
failed.

The ReplayEngine C++ API
------------------------

``ReplayEngine`` (header ``dftracer/utils/utilities/replay/replay.h``, namespace
``dftracer::utils::utilities::replay``) is configured with a ``ReplayConfig`` and
returns a ``ReplayResult``.

.. code-block:: cpp

   #include <dftracer/utils/utilities/replay/replay.h>
   using namespace dftracer::utils::utilities::replay;

   ReplayConfig cfg;
   cfg.maintain_timing = true;          // honor recorded gaps
   cfg.dftracer_mode   = true;          // sleep instead of real I/O
   cfg.filter_categories = {"POSIX"};
   cfg.sampling_rate   = 0.1;

   ReplayEngine engine(cfg);
   ReplayResult result = engine.replay("trace-0.pfw.gz");
   result.print_summary();

``ReplayConfig`` exposes the same knobs as the CLI and more, including
``timing_scale``, ``start_time_offset``, ``max_open_files``, ``min_level`` /
``max_level``, and an ``on_dispatch`` callback invoked per event. ``ReplayResult``
reports ``total_events``, ``executed_events``, ``filtered_events``,
``failed_events``, per-function and per-category counts, and byte totals.

Replay dispatches through pluggable executors: a ``PosixExecutor`` does real I/O
and a ``DFTracerExecutor`` does the sleep-based replay. Add your own with
``engine.add_executor(...)``. To replay respecting the call hierarchy, use
``replay_with_call_tree(trace_dir, pattern)``.

See also
--------

- :doc:`dlio-config` - model a workload's timing instead of replaying it.
