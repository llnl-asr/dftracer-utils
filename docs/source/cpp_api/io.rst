Async I/O API
=============

High-performance, platform-optimized asynchronous file and socket I/O. All classes and functions are in the ``dftracer::utils::io`` namespace.

Overview
--------

The async I/O system provides a lightweight, coroutine-based interface to file and socket operations. It automatically selects the best backend available on the host platform:

- **Linux**: io_uring (preferred) or epoll + thread pool
- **macOS/BSD**: kqueue + thread pool
- **Fallback**: Pure thread pool (all platforms)

Operations are submitted via ``co_await`` and transparently fall back to blocking I/O when called outside an executor context. The system handles scatter-gather I/O, positional reads/writes, and socket operations.

Backend Selection
-----------------

The I/O backend is selected at runtime (or forced via configuration). Available backends are exposed via the ``IoBackendType`` enum:

.. doxygenenum:: dftracer::utils::io::IoBackendType
   :project: dftracer-utils

Platform Support
~~~~~~~~~~~~~~~~

- **IO_URING**: Linux only. Provides lowest latency and highest throughput. Requires Linux 5.1+ kernel.
- **EPOLL_THREADPOOL**: Linux only. Uses epoll for edge-triggered notifications with a thread pool for actual I/O.
- **KQUEUE_THREADPOOL**: macOS and BSD only. Uses kqueue + thread pool model.
- **THREADPOOL**: All platforms. Pure thread pool backend; always available as fallback.
- **AUTO**: Runtime detection. Tries io_uring first, falls back to platform-specific epoll/kqueue, finally to thread pool.

.. mermaid::

   graph TB
       IoBackend["IoBackend<br/>(abstract)"]
       IoAwaitable["IoAwaitable<br/>(co_await result)"]

       IoBackend --> IoAwaitable

       subgraph Backends["Platform Backends"]
           IoUring["IoUringBackend<br/>(Linux 5.1+)"]
           Epoll["EpollThreadPoolBackend<br/>(Linux)"]
           Kqueue["KqueueThreadPoolBackend<br/>(macOS/BSD)"]
           ThreadPool["ThreadPoolBackend<br/>(all platforms)"]
       end

       IoUring -.-> |implements| IoBackend
       Epoll -.-> |implements| IoBackend
       Kqueue -.-> |implements| IoBackend
       ThreadPool -.-> |implements| IoBackend

       Executor["Executor"] --> |owns| IoBackend
       CoroTask["CoroTask"] --> |co_await| IoAwaitable

Core Async Operations
---------------------

All operations return an ``IoAwaitable`` that can be awaited in a coroutine. Outside a coroutine context (no executor), operations fall back to blocking behavior.

Sequential I/O
~~~~~~~~~~~~~~

Read and write operations that respect file position:

.. doxygenfunction:: dftracer::utils::io::read
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::io::write
   :project: dftracer-utils

Positional I/O
~~~~~~~~~~~~~~

Positional variants that do not affect the file offset pointer (seekable files only):

.. doxygenfunction:: dftracer::utils::io::pread
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::io::pwrite
   :project: dftracer-utils

File Management
~~~~~~~~~~~~~~~

Open, close, and introspection:

.. doxygenfunction:: dftracer::utils::io::open
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::io::close
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::io::fsync
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::io::ftruncate
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::io::fstat
   :project: dftracer-utils

Seek Operations
~~~~~~~~~~~~~~~

Reposition the file pointer:

.. doxygenfunction:: dftracer::utils::io::lseek
   :project: dftracer-utils

Scatter-Gather I/O
~~~~~~~~~~~~~~~~~~~

Efficient multi-buffer operations (readv/writev family):

.. doxygenfunction:: dftracer::utils::io::readv
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::io::writev
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::io::preadv
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::io::pwritev
   :project: dftracer-utils

Zero-Copy Transfer
~~~~~~~~~~~~~~~~~~~

Efficient file-to-file/socket transfer without buffering in userspace:

.. doxygenfunction:: dftracer::utils::io::sendfile
   :project: dftracer-utils

Socket Operations
~~~~~~~~~~~~~~~~~

Non-blocking accept and data transfer on connected sockets:

.. doxygenfunction:: dftracer::utils::io::accept
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::io::recv
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::io::send
   :project: dftracer-utils

The Awaitable Type
-------------------

All I/O operations return an ``IoAwaitable`` object. It is a standard C++20 awaitable that suspends the coroutine until the operation completes:

.. doxygenstruct:: dftracer::utils::io::IoAwaitable
   :project: dftracer-utils
   :members:

Result Handling
~~~~~~~~~~~~~~~

The awaited result is typically ``ssize_t``:

- **Positive values**: Number of bytes read/written, or file descriptor (for open), or offset (for lseek).
- **Zero**: Operation completed with zero bytes (e.g., EOF on read), or success with no data (e.g., fsync, close).
- **Negative values**: Negative errno on error. Check ``-result`` against standard ``errno`` codes (ENOENT, EPERM, etc.).

Example
^^^^^^^

.. code-block:: cpp

   #include <dftracer/utils/core/io/io.h>
   #include <dftracer/utils/core/coro/task.h>
   #include <dftracer/utils/core/pipeline/pipeline.h>
   #include <dftracer/utils/core/pipeline/pipeline_config.h>
   #include <iostream>

   using namespace dftracer::utils;

   CoroTask<void> read_file(const std::string& path) {
       // Open file async
       auto fd_result = co_await io::open(path.c_str(), O_RDONLY);
       if (fd_result < 0) {
           std::cerr << "open failed: " << -fd_result << std::endl;
           co_return;
       }
       int fd = static_cast<int>(fd_result);

       // Read 1024 bytes
       char buf[1024] = {};
       auto read_result = co_await io::read(fd, buf, sizeof(buf));
       if (read_result < 0) {
           std::cerr << "read failed: " << -read_result << std::endl;
       } else {
           std::cout << "Read " << read_result << " bytes" << std::endl;
       }

       // Close file
       co_await io::close(fd);
   }

   int main() {
       auto config = PipelineConfig()
           .with_name("ReadExample")
           .with_compute_threads(1);

       auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
           co_await read_file("test.txt");
       }, "ReadFile");

       Pipeline pipeline(config);
       pipeline.set_source({task});
       pipeline.execute();
       return 0;
   }

Sync Fallback Behavior
~~~~~~~~~~~~~~~~~~~~~~

When an I/O operation is called outside an executor context (e.g., in a regular synchronous function or test), it automatically falls back to blocking I/O:

.. code-block:: cpp

   #include <dftracer/utils/core/io/io.h>
