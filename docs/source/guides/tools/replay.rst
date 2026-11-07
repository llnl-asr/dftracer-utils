:description: Re-execute the I/O recorded in a trace, doing real I/O or sleeping per op, with dftracer_replay.

Replay a trace
==============

.. admonition:: Goal
   :class: goal

   Re-execute the I/O operations recorded in a trace - either doing the real
   I/O or sleeping for each operation's duration - to reproduce a workload's timing
   and load. Use the ``dftracer_replay`` binary.

This is a CLI tool. There is no Python binding.

Command line
------------

.. code-block:: bash

   # Replay a directory, honoring the original timing
   dftracer_replay ./traces

   # Sleep-based replay (no real I/O), as fast as possible
   dftracer_replay ./traces --dftracer-mode --no-sleep

   # Analyze only, do no I/O
   dftracer_replay ./traces --dry-run

The inputs are one or more ``.pfw`` or ``.pfw.gz`` files or directories; a directory is read with all its subfolders.
The files replay in parallel, as many at once as ``--executor-threads``, and each file keeps its own timeline.
A directory is read with all of its subfolders.

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

See also
--------

- :doc:`dlio-config` - model a workload's timing instead of replaying it.
