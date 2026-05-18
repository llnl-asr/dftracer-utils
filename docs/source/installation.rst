Installation
============

This guide covers installation of dftracer utilities for both Python and C++ users.

Python Installation
-------------------

Using pip
~~~~~~~~~

The easiest way to install dftracer utilities is via pip:

.. code-block:: bash

   pip install dftracer-utils

From Source
~~~~~~~~~~~

To install from source:

.. code-block:: bash

   git clone https://github.com/LLNL/dftracer-utils.git
   cd dftracer-utils
   pip install .

For development installation with optional dependencies:

.. code-block:: bash

   pip install -e ".[dev]"

C++ Installation
----------------

Prerequisites
~~~~~~~~~~~~~

Before building dftracer utilities, ensure you have:

- CMake 3.5 or higher
- C++20 compatible compiler (GCC 11+, Clang 14+)
- zlib development library
- pkg-config

On Ubuntu/Debian:

.. code-block:: bash

   sudo apt-get install cmake build-essential zlib1g-dev pkg-config

On macOS:

.. code-block:: bash

   brew install cmake zlib pkg-config

Building from Source
~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   git clone https://github.com/LLNL/dftracer-utils.git
   cd dftracer-utils
   mkdir build && cd build
   cmake ..
   make
   sudo make install

Custom Installation Location
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

To install to a custom location:

.. code-block:: bash

   mkdir build && cd build
   cmake .. -DCMAKE_INSTALL_PREFIX=/path/to/install
   make
   make install

Build Options
~~~~~~~~~~~~~

The following CMake options control optional features and dependencies. All
options default to ``ON`` unless noted otherwise:

- ``DFTRACER_UTILS_TESTS`` (default ``OFF``) - Build the test suite.
- ``DFTRACER_UTILS_COVERAGE`` (default ``OFF``) - Enable coverage reporting.
- ``DFTRACER_UTILS_DEBUG`` (default ``OFF``) - Enable debug mode with verbose
  logging.
- ``DFTRACER_UTILS_BUILD_SHARED`` (default ``ON``) - Build the shared library.
- ``DFTRACER_UTILS_BUILD_STATIC`` (default ``ON``) - Build the static library.
- ``DFTRACER_UTILS_BUILD_BINARIES`` (default ``ON``) - Build command-line
  binaries.
- ``DFTRACER_UTILS_BUILD_PYTHON`` (default ``OFF``) - Build Python bindings.
- ``DFTRACER_UTILS_ENABLE_PCH`` (default ``ON``) - Enable precompiled
  headers.
- ``DFTRACER_UTILS_ENABLE_ASAN`` / ``_UBSAN`` / ``_TSAN`` (default ``OFF``) -
  Address / undefined-behavior / thread sanitizers.
- ``DFTRACER_UTILS_ENABLE_MPI`` (default ``OFF``) - Enable MPI support;
  required to build ``dftracer_aggregator_mpi`` and
  ``dftracer_call_tree_mpi``.
- ``DFTRACER_USE_ZLIB_NG`` (default ``ON``) - Use ``zlib-ng`` (compat ABI)
  for faster compression and decompression. Falls back to ``madler/zlib``
  if zlib-ng fetch or build fails.
- ``DFTRACER_UTILS_ENABLE_ARROW`` (default ``ON``) - Enable the Arrow C Data
  Interface via nanoarrow (required for Python Arrow output).
- ``DFTRACER_UTILS_ENABLE_ARROW_IPC`` (default ``ON``) - Enable Arrow IPC
  file read/write via nanoarrow. Required for ``dftracer_aggregator
  --format arrow`` output and for the ``save_arrow`` / ``load_arrow`` call-
  tree serialization paths.
- ``DFTRACER_UTILS_ENABLE_ZSTD`` (default ``ON``) - Enable ZSTD compression
  for RocksDB SST blocks.
- ``DFTRACER_UTILS_ENABLE_LZ4`` (default ``OFF``) - Enable LZ4 compression
  for RocksDB SST blocks.

Example:

.. code-block:: bash

   cmake .. \
       -DDFTRACER_UTILS_ENABLE_MPI=ON \
       -DDFTRACER_UTILS_ENABLE_ARROW_IPC=ON \
       -DDFTRACER_USE_ZLIB_NG=ON

Verifying Installation
----------------------

Python
~~~~~~

To verify your Python installation:

.. code-block:: python

   import dftracer.utils
   print(dftracer.utils.__version__)

C++
~~~

To verify your C++ installation, try compiling a simple example that
opens a trace through the public ``TraceReader`` API:

.. code-block:: cpp

   #include <dftracer/utils/utilities/reader/trace_reader.h>
   #include <iostream>

   int main() {
       using dftracer::utils::utilities::reader::TraceReader;

       TraceReader reader("test.pfw.gz");
       std::cout << "Library installed successfully!" << std::endl;
       std::cout << "Has index: " << std::boolalpha
                 << reader.has_index() << std::endl;
       return 0;
   }

Compile with:

.. code-block:: bash

    g++ -std=c++20 example.cpp -ldftracer_utils -o example
    ./example

Platform-Specific I/O Backends
------------------------------

dftracer utilities uses an async I/O backend to efficiently handle file operations in coroutine-based pipelines.
The backend is automatically detected at build time based on your platform and kernel version.

Supported Backends
~~~~~~~~~~~~~~~~~~

**io_uring (Linux 5.1+, optimal performance)**

- Native Linux kernel async I/O mechanism with minimal overhead
- Requires Linux kernel 5.1 or later with `io_uring` support
- No external dependencies; kernel headers only
- Auto-enabled if available, provides the highest performance
- Fallback to thread pool if kernel does not support io_uring

**kqueue (macOS, FreeBSD, optimal for BSD systems)**

- Native BSD kernel async I/O via `sys/event.h`
- Automatically available on macOS and FreeBSD
- Provides efficient event multiplexing for file, socket, and timer operations
- Preferred over thread pool for BSD-based systems

**Thread Pool (All platforms, universal fallback)**

- Portable fallback using a thread pool for async operations
- Available on all platforms (Linux, macOS, BSD, Windows with appropriate compiler)
- Trades performance for maximum compatibility
- Default if neither io_uring nor kqueue is available

Backend Selection and Build Configuration
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

By default, the build system automatically detects your platform and enables the best available backend:

1. **Linux**: Auto-detects io_uring, falls back to thread pool if unavailable
2. **macOS/FreeBSD**: Auto-detects kqueue, falls back to thread pool if needed
3. **Other platforms**: Uses thread pool

To verify which backend was built, check the CMake output during configuration:

.. code-block:: bash

    cmake --preset dev
    # Look for output like:
    # -- io_uring support: enabled (kernel header found)
    # -- kqueue support: disabled (sys/event.h not found)

.. note::
   Backend selection is automatic based on platform detection during CMake configuration.
   You can override the default backend at runtime via ``PipelineConfig::with_io_backend()``.

Minimum Requirements by Backend
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **io_uring**: Linux kernel 5.1 or later (check with ``uname -r``)
- **kqueue**: macOS 10.6+ or FreeBSD 4.1+
- **Thread Pool**: No special requirements, available on all platforms

If you encounter I/O-related issues, verify your kernel version or check the CMake output
to confirm which backend was built.
