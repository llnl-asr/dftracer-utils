:description: The environment variables dftracer-utils reads at runtime from the core, CLI, plugin build backend, and Python bindings, with defaults and effects.

Environment Variables
======================

Environment variables dftracer-utils reads at runtime: from the C++ core, the
CLI binaries, the plugin/JIT build backend, and the Python bindings. Each row
gives the effect and the value when the variable is unset.

These are runtime knobs, read through ``Env::get`` (C++) or ``os.environ``
(Python) while a process runs. They are not the same as the ``DFTRACER_UTILS_*`` CMake configure
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

Memory and kernels
-------------------

.. list-table::
   :header-rows: 1
   :widths: 32 14 54

   * - Variable
     - Default
     - Effect
   * - ``DFTRACER_UTILS_SPILL_DIR``
     - a node-local disk
     - Directory for every spill file of a collect, sort, group-by, join,
       window or plugin state. When it is unset the writable node-local disk
       mount with the most free space is used (never RAM-backed or network),
       else the system temp directory or ``~/.cache/dftracer-utils/spill``,
       whichever is writable and not RAM-backed (a network file system
       qualifies). A RAM-backed directory is never chosen: with nothing else
       usable, or a set directory that cannot be created or written, the run
       fails. Each query makes a ``dftu_lazy_<host>_<pid>_<n>``
       directory in it and removes it when it ends; the first query of a
       process also removes such directories of the same host left by processes that no
       longer exist. See :doc:`guides/runtime/memory-budget`.
   * - ``DFTRACER_UTILS_SPILL_COMPRESS``
     - ``auto``
     - ``on``, ``1`` or ``zstd`` compresses every spilled morsel with zstd
       level 1 (a morsel that does not shrink is stored raw); ``off`` or ``0``
       stores them raw. ``auto``, unset or any other value compresses only
       when the spill file is on a network file system, where the bytes
       written cost more than the compression.
   * - ``DFTRACER_UTILS_MEMORY_BACKEND``
     - the build option
     - ``page_heap`` or ``system``. Picks the allocator backend, read once
       before the first allocation. Unset or empty uses the CMake option of the
       same name (``page_heap``, and ``system`` in the sanitizer presets). Any
       other value stops the process at the first allocation.
   * - ``DFTRACER_UTILS_MEM_DECAY_MS``
     - ``1000``
     - Milliseconds an idle page or run stays backed before the heap gives it
       back to the operating system. ``0`` gives it back at the first slow
       path. Read once before the first allocation. A value that is not a whole
       number up to ``4294967295`` stops the process at the first allocation.
   * - ``DFTRACER_UTILS_MEM_BUDGET``
     - a third of the available memory
     - The sum of reservations that the memory manager accepts, in bytes or
       with a unit such as ``4GB``. Unset or ``0`` uses a third of the
       available memory (the cgroup limit when there is one). A value that is
       not a byte size is an error that names the variable.
   * - ``DFTRACER_UTILS_MEM_HARD_LIMIT``
     - ``0``
     - Bytes of heap the process may hold from the operating system, in bytes
       or with a unit. An allocation that would pass it fails with
       ``std::bad_alloc``. ``0`` sets no cap. A bad value is an error that
       names the variable.
   * - ``DFTRACER_UTILS_MEM_LIMIT_HEADROOM``
     - 5% of the limit, at least 256 MB
     - How far below the process memory limit the measured memory may grow
       before holders are asked to give bytes back. Bytes or a unit. A bad
       value is an error that names the variable.
   * - ``DFTRACER_UTILS_MEM_SPILL_HEADROOM``
     - 2% of the budget, at least 32 MB
     - How far past the budget a pool that is spilling may reserve, so that
       the spill can allocate its buffers. Bytes or a unit. A bad value is an
       error that names the variable.
   * - ``DFTRACER_UTILS_MEMORY_DEBUG``
     - ``0``
     - ``1`` turns on the heap debug checks, read once before the first
       allocation: a record of the caller and size of each live block, a
       redzone behind each block, a fill pattern on freed blocks and double
       free detection. Each allocation is slower and freed blocks stay counted
       for a while. Any value other than ``0`` or ``1`` stops the process at
       the first allocation.
   * - ``DFTRACER_UTILS_MEMORY_EXIT_CHECK``
     - ``0``
     - For test runs. ``1`` makes the process print a leak report and exit
       with status ``70`` when blocks that no host buffer owns are still live
       at exit or a scratch pool leaked. Do not set it for a normal run. The
       Python extension turns it off at import. Any value other than ``0`` or
       ``1`` stops the process at the first allocation.
   * - ``DFTRACER_UTILS_STRING_PREDICATE``
     - ``auto``
     - Scan used by the string class predicates (``isalpha``, ``isdigit`` and
       so on): ``bits`` or ``rows``. Any other value picks per column from the
       mean string length. Both give the same result. A test and benchmark
       hook.
   * - ``DFTRACER_UTILS_STRING_SCALAR``
     - unset
     - Any value runs the byte-at-a-time reference instead of the vector
       string kernels. A test and benchmark hook.
   * - ``DFTRACER_UTILS_GROUPBY_STITCH``
     - unset
     - Any value makes ``group_by`` build its result through the stitch path
       instead of writing it in place. A test switch.

duql
-----

.. list-table::
   :header-rows: 1
   :widths: 32 14 54

   * - Variable
     - Default
     - Effect
   * - ``DFTRACER_UTILS_DUQL_PATH``
     - unset
     - ``.duql`` macro files or directories, ``:``-separated. Loaded after the
       query's own macros and ``--duql-path``. See :doc:`guides/core/duql`.
   * - ``DUQL_LOOKUP_MAX_ROWS``
     - ``1000000``
     - Row cap of an in-memory lookup side. A larger side fails the query.
   * - ``DUQL_LOOKUP_MAX_BYTES``
     - ``268435456``
     - Byte cap of an in-memory lookup side.
   * - ``DUQL_FILL_MAX_ROWS``
     - ``10000000``
     - Row cap of the grid a ``fill`` builds. See :doc:`reference/duql`.
   * - ``DUQL_PIVOT_MAX_COLUMNS``
     - ``1024``
     - Column cap of a ``pivot`` with no ``in`` list.

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
   * - ``DFTRACER_UTILS_SCHEMA_PATH``
     - unset
     - Record schema spec files or directories, ``:``-separated, registered
       on first use of the schema registry. See
       :doc:`reference/record-schema`.
   * - ``DFTRACER_UTILS_INDEX_SIZE_FACTOR``
     - ``3``
     - Multiplier applied to total input bytes to size the scratch reservation
       when staging an index build to local scratch. Non-positive values are
       treated as ``1``.
   * - ``DFTRACER_UTILS_INDEX_SCRATCH``
     - auto-detected best local mount
     - Forces the scratch staging root. A path is used directly (validated
       writable and roomy); ``off``, ``none``, ``0``, or ``false`` disables
       scratch staging entirely.
   * - ``DFTRACER_UTILS_INDEX_STAGE``
     - stage only for network destinations
     - Controls when an index build stages to local scratch before publishing
       to its final destination. ``never``, ``off``, or ``0`` disables
       staging; ``always`` forces staging even for local destinations.
   * - ``DFTRACER_UTILS_CACHE_MAX_BYTES``
     - ``2G``
     - Size cap for each query cache store (rollups and materialized views
       in ``.dftindex-cache``). A size such as ``512M`` or ``4G``. When a
       store grows past the cap, the least recently used entries are
       removed.

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
   * - ``DFTRACER_UTILS_PLUGIN_INCLUDE``
     - bundled package headers, then a source-tree parent walk
     - Overrides the directory containing ``dftracer/utils/plugins/abi.h``,
       the plugin C ABI header. Takes priority over the other two lookups.
   * - ``CXX``
     - ``c++``
     - Compiler used to build plugins and JIT-compiled bodies. Falls back to
       ``shutil.which("c++")`` then ``shutil.which("clang++")`` then
       ``"c++"`` when unset; it always selects a C++ compiler regardless of
       what ``CXX`` names.
   * - ``DFTRACER_UTILS_JIT_CACHE``
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
