:description: Install dftracer-utils as a Python wheel or build the C++ libraries and C ABI from source with the CMake presets.

Installation
============

dftracer-utils ships as a Python package and as C++ libraries plus a stable C
ABI. Install whichever surface you need; they come from the same source tree.

Python
------

The fastest path. Wheels bundle the native engine, so no compiler is needed.

.. code-block:: bash

   pip install dftracer-utils

Verify it:

.. code-block:: bash

   python -c "import dftracer.utils as d; print(d.__version__)"

From source (Python)
~~~~~~~~~~~~~~~~~~~~~

You need a C++20 compiler, CMake 3.23+, and Ninja. The build is driven by
scikit-build-core, so a plain ``pip install`` compiles the extension:

.. code-block:: bash

   git clone https://github.com/llnl-asr/dftracer-utils
   cd dftracer-utils
   pip install -e ".[dev]"     # editable, with test/lint/type tooling

C and C++
---------

The C++ libraries and the C ABI build with CMake **presets** (Ninja generator;
build dirs land under ``build/build-<preset>/``).

.. code-block:: bash

   git clone https://github.com/llnl-asr/dftracer-utils
   cd dftracer-utils

   cmake --preset dev            # RelWithDebInfo, shared + static, no Python
   cmake --build --preset dev

The build produces five layered component libraries, each as shared and static:

- ``dftracer_utils_core``, the async runtime (coroutines, tasks, I/O backend,
  pipelines, RocksDB wrappers, common primitives).
- ``dftracer_utils_json``, JSON parsing (simdjson-backed).
- ``dftracer_utils_duql``, the duql filter language (predicate IR, string codec, evaluator).
- ``dftracer_utils_dataframe``, the columnar SIMD engine (``Series`` /
  ``DataFrame``, Highway kernels, the Arrow bridge, query execution / masking).
- ``dftracer_utils_utilities``, the domain layer (trace readers, indexer,
  aggregation, comparison, statistics, plugins, DLIO, replay).

You do not link these individually, ``find_package(dftracer_utils)`` and the
``dftracer::utils`` target pull the whole set transitively (see below).

Prerequisites
~~~~~~~~~~~~~

.. tab-set::

   .. tab-item:: Ubuntu / Debian

      .. code-block:: bash

         sudo apt-get install cmake ninja-build build-essential pkg-config zlib1g-dev

   .. tab-item:: macOS

      .. code-block:: bash

         brew install cmake ninja pkg-config

      The Apple SDK provides zlib. On Linux the I/O backend uses io_uring, on
      macOS it uses kqueue, selected automatically at configure time.

Presets
~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Preset
     - What it configures
   * - ``dev``
     - RelWithDebInfo, shared + static, no Python. The default.
   * - ``dev-python``
     - ``dev`` plus the Python bindings (``DFTRACER_UTILS_BUILD_PYTHON=ON``).
   * - ``debug``
     - Full Debug with verbose logging.
   * - ``tests``
     - Tests + debug logging + Python, for the C++ suite.
   * - ``asan`` / ``tsan`` / ``ubsan`` / ``asan-ubsan``
     - Sanitizer builds (each has matching build/test presets).

.. _build-options:

Build options
~~~~~~~~~~~~~

Pass with ``-D`` at configure time, for example
``cmake --preset dev -DDFTRACER_UTILS_BUILD_PYTHON=ON``.

**Build targets**

.. list-table::
   :header-rows: 1
   :widths: 42 12 46

   * - Option
     - Default
     - Effect
   * - ``DFTRACER_UTILS_BUILD_SHARED``
     - ``ON``
     - Build the shared libraries.
   * - ``DFTRACER_UTILS_BUILD_STATIC``
     - ``ON``
     - Build the static libraries.
   * - ``DFTRACER_UTILS_BUILD_BINARIES``
     - ``ON``
     - Build the ``dftracer_*`` command-line tools.
   * - ``DFTRACER_UTILS_BUILD_PYTHON``
     - ``OFF``
     - Build the CPython extension.
   * - ``DFTRACER_UTILS_BUILD_EXAMPLES``
     - ``ON``
     - Build the containment and plugin examples under ``examples/``.
   * - ``DFTRACER_UTILS_BUILD_BENCHMARKS``
     - ``OFF``
     - Build the standalone benchmarks.
   * - ``DFTRACER_UTILS_BUILD_WEB_UI``
     - ``OFF``
     - Rebuild the web UI with npm during the build (needs Node/npm).

**Features**

.. list-table::
   :header-rows: 1
   :widths: 42 12 46

   * - Option
     - Default
     - Effect
   * - ``DFTRACER_UTILS_ENABLE_ARROW``
     - ``ON``
     - Arrow C Data Interface via nanoarrow (zero-copy export).
   * - ``DFTRACER_UTILS_ENABLE_ARROW_IPC``
     - ``ON``
     - Arrow IPC file read/write via nanoarrow.
   * - ``DFTRACER_UTILS_ENABLE_VECTORSCAN``
     - ``ON``
     - Vectorscan for yes/no regex matches. ``OFF`` downloads
       nothing and runs every regex on PCRE2. See `Vectorscan build needs`_.
   * - ``DFTRACER_UTILS_ENABLE_ZSTD``
     - ``ON``
     - ZSTD compression for RocksDB.
   * - ``DFTRACER_UTILS_ENABLE_LZ4``
     - ``OFF``
     - LZ4 compression for RocksDB.
   * - ``DFTRACER_UTILS_ENABLE_MPI``
     - ``OFF``
     - MPI support (distributes ``dftracer_view --flamegraph`` across ranks).
   * - ``DFTRACER_UTILS_ENABLE_PCH``
     - ``OFF``
     - Precompiled headers (faster cold builds). ccache cannot cache GCC's, so leave it off when using ccache.
   * - ``DFTRACER_USE_ZLIB_NG``
     - ``ON``
     - Use zlib-ng instead of madler/zlib (falls back on failure).

**Packaging and dev**

.. list-table::
   :header-rows: 1
   :widths: 42 12 46

   * - Option
     - Default
     - Effect
   * - ``DFTRACER_UTILS_LOCAL_PACKAGES``
     - ``ON``
     - Prefer system installs of the dependencies over the vendored (CPM)
       copies. Turn ``OFF`` to build them from source for portable wheels.
   * - ``DFTRACER_UTILS_TESTS``
     - ``OFF``
     - Build the C++ test suite (the ``tests`` preset sets this).
   * - ``DFTRACER_UTILS_COVERAGE``
     - ``OFF``
     - Coverage instrumentation.
   * - ``DFTRACER_UTILS_DEBUG``
     - ``OFF``
     - Debug mode with verbose logging.
   * - ``DFTRACER_UTILS_LOGGER_LEVEL_TRACE``
     - ``ON``
     - Compile in TRACE logging + coroutine auto-tracing (still runtime-gated
       by ``DFTRACER_UTILS_LOG_LEVEL``); ``OFF`` strips it for a minimal build.
   * - ``DFTRACER_UTILS_ENABLE_ASAN`` / ``_UBSAN`` / ``_TSAN``
     - ``OFF``
     - Address / UndefinedBehavior / Thread sanitizer (also via the ``asan``,
       ``ubsan``, ``tsan`` presets).
   * - ``DFTRACER_UTILS_VALGRIND_MODE``
     - ``OFF``
     - Build for Valgrind (disables io_uring; Valgrind <3.23.0 bug #428364).
   * - ``DFTRACER_UTILS_HASH_SEED``
     - ``104723``
     - Hash seed, as a compiler define (``-DDFTRACER_UTILS_HASH_SEED=<n>`` in
       ``CXXFLAGS``), not a CMake cache variable.

The sanitizer, coverage, and test toggles all have matching presets (``asan``,
``ubsan``, ``tsan``, ``asan-ubsan``, ``tests``), prefer those over setting the
options by hand.

Dependencies (RocksDB, simdjson, nanoarrow, Highway, zstd, lz4, and others) are
vendored via CPM and cached under ``.cpmsource/``; see the
`THIRD-PARTY-NOTICES <https://github.com/llnl-asr/dftracer-utils/blob/develop/THIRD-PARTY-NOTICES.md>`_
for the full list and licenses.

Vectorscan build needs
~~~~~~~~~~~~~~~~~~~~~~

With ``DFTRACER_UTILS_ENABLE_VECTORSCAN=ON``, CMake gets three build-time
inputs and builds Vectorscan 5.4.12 as a static library:

- ``ragel`` from ``PATH``, or Ragel 6.10, which CMake downloads and builds at
  configure time.
- Boost headers 1.61 or later from the system, or the Boost 1.86 release,
  which CMake downloads through CPM.
- Vectorscan 5.4.12, which CMake downloads through CPM.

Ragel and Boost are needed at build time only; nothing new ships at run time.
A Linux x86_64 build uses Vectorscan's fat runtime, which picks SSE4.2, AVX2
or AVX-512 at run time.

For an offline build, fill ``CPM_SOURCE_CACHE`` (``.cpmsource/``) ahead of
time. The Ragel, Boost and Vectorscan sources join the other vendored
dependencies there. With the option ``OFF``, none of them is needed.

Link against dftracer-utils
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Install the libraries (``cmake --install build/build-dev``), or point
``CMAKE_PREFIX_PATH`` at the build tree, then consume them from your own CMake
project with ``find_package``:

.. code-block:: cmake

   find_package(dftracer_utils REQUIRED)

   add_executable(my_tool main.cpp)
   target_link_libraries(my_tool PRIVATE dftracer::utils)

The exported targets are ``dftracer::utils`` (the default alias: shared, falling
back to static), ``dftracer_utils::shared``, and ``dftracer_utils::static``.
Linking any of them pulls in all five component libraries and their include
directories transitively, so no manual ``-I`` / ``-l`` is needed. The stable C ABI (``dftu_dataframe_*``,
``dftu_duql_*``, and the plugin ``abi.h``) ships in the same libraries, a C
consumer links the same target and includes the C headers.

For non-CMake build systems, a pkg-config file is installed for each library
under ``<prefix>/lib/pkgconfig``:

.. code-block:: console

   $ pkg-config --cflags --libs dftracer_utils_utilities

Verify the build
----------------

Run the C++ test suite:

.. code-block:: bash

   cmake --preset tests
   cmake --build --preset tests
   ctest --preset tests --output-on-failure

Or the Python tests in a throwaway virtualenv:

.. code-block:: bash

   make test-py

Next
----

You have it installed. Continue with the :doc:`Tutorials <../tutorials/index>`,
starting with your :doc:`first analysis <../tutorials/first-analysis>`.
