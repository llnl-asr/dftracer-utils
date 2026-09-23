:description: The environment variables dftracer-utils reads at runtime from the core, CLI, plugin build backend, and Python bindings, with defaults and effects.

Environment Variables
======================

Environment variables dftracer-utils reads at runtime: from the C++ core, the
CLI binaries, the plugin/JIT build backend, and the Python bindings. Each row
gives the effect and the value when the variable is unset.

These are runtime knobs, read with ``getenv``/``os.environ`` while a process
runs. They are not the same as the ``DFTRACER_UTILS_*`` CMake configure
options (``-D...=ON/OFF``) baked into the binary at build time - see
:ref:`build-options` for those, including ``DFTRACER_UTILS_VALGRIND_MODE``,
which despite the name-space overlap is a compile-time macro, not something
you set in the environment.

Runtime and scheduler
----------------------

.. list-table::
   :header-rows: 1
   :widths: 32 14 54

   * - Variable
     - Default
     - Effect
   * - ``DFTRACER_UTILS_THREADS``
     - hardware concurrency
     - Fixes the compute worker-thread count of a new ``Runtime`` (e.g. ``1``
       for a single-threaded async loop). Any value ``> 0`` overrides the
       requested/detected count; non-positive or unparsable values are
       ignored.
   * - ``DFTRACER_UTILS_IO_THREADS``
     - hardware concurrency
     - Fixes the I/O backend (io_uring/kqueue) thread-pool size independently
       of the compute pool. Same parsing rule as ``DFTRACER_UTILS_THREADS``.
   * - ``DFTRACER_UTILS_HW_CONCURRENCY``
     - ``std::thread::hardware_concurrency()`` (or ``1`` if that is ``0``)
     - Overrides the detected core count used to size thread pools when the
       above are left at their defaults.
   * - ``DFTRACER_UTILS_ELASTIC``
     - enabled
     - Set to ``0``, ``false``, or ``off`` to disable elastic (work-stealing,
       load-scaled) scheduling for a default ``Runtime``. Any other value, or
       leaving it unset, keeps elastic scheduling on.

RocksDB and indexing
----------------------

.. list-table::
   :header-rows: 1
   :widths: 32 14 54

   * - Variable
     - Default
     - Effect
   * - ``DFTRACER_UTILS_ROCKSDB_CACHE``
     - ``64``
     - Cap on the process-wide LRU of retained read-only RocksDB handles, so
       sequential reads (plan phases, later queries, Dask tasks) reuse one
       open handle. Non-positive or unparsable values fall back to the
       default.
   * - ``DFTRACER_UTILS_ROCKSDB_MAX_OPEN_FILES``
     - ``32`` (write mode); unlimited (``-1``) for read-only databases
     - Sets RocksDB's ``max_open_files``. Accepts ``-1`` (unlimited) or a
       positive integer; any other value falls back to the default. Setting
       it explicitly also overrides the read-only fast path, which otherwise
       keeps every SST file open (``-1``) to avoid re-reading filter/index
       blocks on point lookups.
   * - ``DFTRACER_INDEX_SIZE_FACTOR``
     - ``3``
     - Multiplier applied to total input bytes to size the scratch reservation
       when staging an index build to local scratch. Non-positive values are
       treated as ``1``.
   * - ``DFTRACER_INDEX_SCRATCH``
     - auto-detected best local mount
     - Forces the scratch staging root. A path is used directly (validated
       writable and roomy); ``off``, ``none``, ``0``, or ``false`` disables
       scratch staging entirely.
   * - ``DFTRACER_INDEX_STAGE``
     - stage only for network destinations
     - Controls when an index build stages to local scratch before publishing
       to its final destination. ``never``, ``off``, or ``0`` disables
       staging; ``always`` forces staging even for local destinations.

Logging
--------

See :doc:`guides/tools/logging` for the full logging guide (Python API, CLI
flags, and the C++ ``logger`` API these variables initialize).

.. list-table::
   :header-rows: 1
   :widths: 32 14 54

   * - Variable
     - Default
     - Effect
   * - ``DFTRACER_UTILS_LOG_LEVEL``
     - ``info``
     - Initial log level: ``trace``, ``debug``, ``info``, ``warn`` /
       ``warning``, ``error``, or ``off`` / ``none``. An unrecognized value is
       ignored.
   * - ``DFTRACER_UTILS_LOG_COLOR``
     - ``auto``
     - Color mode: ``always`` / ``1`` / ``on``, ``never`` / ``0`` / ``off``, or
       ``auto`` (color when stderr is a TTY, honoring the variables below).
   * - ``DFTRACER_UTILS_LOG_FILE``
     - stderr
     - Path to append log output to instead of stderr.
   * - ``NO_COLOR``
     - unset
     - Any value disables color under ``auto`` mode (the `NO_COLOR
       <https://no-color.org/>`_ convention).
   * - ``FORCE_COLOR``, ``CLICOLOR_FORCE``
     - unset
     - Either set forces color under ``auto`` mode even when stderr is not a
       TTY.
   * - ``TERM``
     - n/a
     - ``dumb`` disables color under ``auto`` mode.

These are read once at logger initialization (process start, or Python
extension import); ``set_log_level`` / ``set_log_color`` and the CLI's
``--log-level`` override them afterward.

Monitor (coroutine/task instrumentation)
------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 32 14 54

   * - Variable
     - Default
     - Effect
   * - ``DFTRACER_UTILS_MONITOR``
     - off
     - Enables the coroutine/task monitor and selects its mode: ``tree``,
       ``deep``, ``trace``, or any other non-empty value for ``summary``.
       ``0`` or ``false`` disables it, unless ``DFTRACER_UTILS_MONITOR_FILE``
       is also set.
   * - ``DFTRACER_UTILS_MONITOR_FILE``
     - unset (no CSV)
     - Path to write a CSV of monitor events. Setting only this (without
       ``DFTRACER_UTILS_MONITOR``) still enables ``summary`` mode.
   * - ``DFTRACER_UTILS_MONITOR_MIN_US``
     - ``0`` (no filter)
     - Minimum event duration in microseconds for an entry to appear in
       monitor output.

Plugin and JIT build
----------------------

Read by the Python compile backend (``python/dftracer/utils/_plugin_build.py``)
shared by the ``@jit.plugin`` decorator and the ``dftracer_plugin``
console-script.

.. list-table::
   :header-rows: 1
   :widths: 32 14 54

   * - Variable
     - Default
     - Effect
   * - ``DFTRACER_PLUGIN_INCLUDE``
     - bundled package headers, then a source-tree parent walk
     - Overrides the directory containing ``dftracer/utils/plugins/abi.h``,
       the plugin C ABI header. Takes priority over the other two lookups.
   * - ``CXX``
     - ``c++``
     - Compiler used to build plugins and JIT-compiled bodies. Falls back to
       ``shutil.which("c++")`` then ``shutil.which("clang++")`` then
       ``"c++"`` when unset; it always selects a C++ compiler regardless of
       what ``CXX`` names.
   * - ``DFTRACER_JIT_CACHE``
     - ``~/.cache/dftracer-utils/jit``
     - Directory for content-hashed compiled plugin/JIT shared objects, so an
       unchanged source never recompiles.

See also
---------

- :ref:`build-options` in :doc:`getting-started/installation` - the
  ``DFTRACER_UTILS_*`` CMake configure options (build-time, not
  environment variables).
- :doc:`guides/tools/logging` - the logging guide (Python/CLI/C++ APIs on top
  of the log variables above).
