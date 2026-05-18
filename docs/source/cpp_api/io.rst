Async I/O API
=============

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/io>`.


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

.. code-block:: cpp

   IoAwaitable read(int fd, void* buf, std::size_t len) noexcept;
   IoAwaitable write(int fd, const void* buf, std::size_t len) noexcept;

Positional I/O
~~~~~~~~~~~~~~

Positional variants that do not affect the file offset pointer (seekable files only):

.. code-block:: cpp

   IoAwaitable pread(int fd, void* buf, std::size_t len, off_t offset) noexcept;
   IoAwaitable pwrite(int fd, const void* buf, std::size_t len, off_t offset) noexcept;

File Management
~~~~~~~~~~~~~~~

Open, close, and introspection:

.. code-block:: cpp

   IoAwaitable open(const char* path, int flags, mode_t mode = 0644) noexcept;
   IoAwaitable close(int fd) noexcept;

Seek Operations
~~~~~~~~~~~~~~~

Reposition the file pointer:

.. code-block:: cpp

   IoAwaitable lseek(int fd, off_t offset, int whence) noexcept;

Scatter-Gather I/O
~~~~~~~~~~~~~~~~~~~

Efficient multi-buffer operations (readv/writev family):

.. code-block:: cpp

   IoAwaitable readv(int fd, const struct iovec* iov, int iovcnt) noexcept;
   IoAwaitable writev(int fd, const struct iovec* iov, int iovcnt) noexcept;
   IoAwaitable preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset) noexcept;
   IoAwaitable pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset) noexcept;

Zero-Copy Transfer
~~~~~~~~~~~~~~~~~~~

Efficient file-to-file/socket transfer without buffering in userspace:

.. code-block:: cpp

   IoAwaitable sendfile(int out_fd, int in_fd, off_t offset, std::size_t len) noexcept;

Socket Operations
~~~~~~~~~~~~~~~~~

Non-blocking accept and data transfer on connected sockets:

.. code-block:: cpp

   IoAwaitable accept(int listen_fd, struct sockaddr* addr = nullptr,
                      socklen_t* addrlen = nullptr) noexcept;
   IoAwaitable recv(int fd, void* buf, std::size_t len, int flags = 0) noexcept;
   IoAwaitable send(int fd, const void* buf, std::size_t len, int flags = 0) noexcept;

The Awaitable Type
-------------------

All I/O operations return an ``IoAwaitable`` object. It is a standard C++20 awaitable that suspends the coroutine until the operation completes:

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

Parallel File Writers
---------------------

The ``dftracer/utils/utilities/fileio/parallel/`` module provides
high-throughput multi-stream file writers used by the reorganization and
aggregation pipelines. The unified ``ParallelWriter`` class implements
three on-disk layouts (selected via ``FileLayout`` in
``parallel/layout.h``):

- **Striped** -- one output file split into Lustre-friendly stripes,
  each fed by an independent producer coroutine.
- **Padded striped** -- striped layout with per-stripe alignment padding
  for filesystems that prefer aligned writes.
- **Sharded** -- one output file per shard, used when downstream
  consumers want independent shards rather than a single concatenated
  file.

Sizing is Lustre-aware: ``LayoutInfo`` and ``WriterSizing`` derive stripe
size and per-stripe buffer counts from the detected ``FilesystemKind``
(Lustre vs generic POSIX). Internally, writes are coalesced via
``coro::Channel``-based queues so that producer coroutines can submit
small line-sized payloads without per-write ``write()`` syscalls.

.. code-block:: cpp

    #include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
    #include <dftracer/utils/utilities/fileio/parallel/layout.h>

    WriterConfig cfg;
    cfg.layout = FileLayout::STRIPED;
    cfg.output_path = "merged.pfw";
    ParallelWriter writer(cfg);

Sync Fallback Behavior
~~~~~~~~~~~~~~~~~~~~~~

When an I/O operation is called outside an executor context (no active ``Executor`` on the current thread), it automatically falls back to synchronous blocking I/O. This allows the same code to work in both coroutine and non-coroutine contexts:

.. code-block:: cpp

   #include <dftracer/utils/core/io/io.h>

   // Works both inside and outside coroutines:
   auto result = co_await io::read(fd, buf, len);
   // Inside executor: async submission via io_uring/epoll/kqueue
   // Outside executor: direct blocking read() syscall
