:description: Run your own I/O backend, or add a file system so that find, stat and read_file work on a remote store.

Add your own I/O backend or file system
=======================================

.. admonition:: Goal
   :class: goal

   Change how I/O operations run, or where the bytes live, with no change to the code that calls them.

Two interfaces do this.
``IoBackend`` in ``dftracer/utils/runtime/io/backend.h`` decides how one operation runs.
``FileSystem`` in ``dftracer/utils/runtime/fs/filesystem.h`` decides where a path lives.

Run your own I/O backend
------------------------

A backend takes an ``IoOp``, runs it, sets ``result`` and calls ``complete``.
The submitter owns the ``IoOp``, so a submit allocates nothing.
The backend must not touch the operation after ``complete``, which can destroy it.

.. code-block:: cpp

   #include <dftracer/utils/runtime/io/backend.h>
   #include <dftracer/utils/runtime/runtime.h>

   using namespace dftracer::utils;

   class MyBackend final : public runtime::io::IoBackend {
      public:
       void start() override {}
       void stop() override {}
       void submit(runtime::io::IoOp& op) override {
           // run_it is yours: read op.kind and the fields of that kind, make
           // the call and give its value or a negative errno.
           op.result = run_it(op);
           op.complete(op);
       }
       std::size_t poll(int) override { return 0; }
       dstd::String name() const override { return "mine"; }
   };

   dstd::UniquePtr<runtime::io::IoBackend> make_mine(void*) {
       return dstd::make_unique<MyBackend>();
   }

   Runtime rt(ExecutorConfig{.io_backend_factory = &make_mine});

A backend that holds operations must also do these three things:

- Start every held operation in ``flush``.
- Report what it holds in ``in_flight``, because the executor waits for zero before it stops.
- Complete with ``-ECANCELED`` after ``cancel_pending`` any operation that waits on a pipe or socket.

Add a file system
-----------------

A file system serves one path scheme.
Implement ``open``, ``stat`` and ``list``, and return ``dstd::Condition::NotFound`` for a path that does not exist.
``open`` returns a ``FileSystemFile``, which implements ``read_at``, ``write_at`` and ``close``.
The calls return tasks and must not block the worker.

.. code-block:: cpp

   #include <dftracer/utils/runtime/fs/filesystem.h>

   using namespace dftracer::utils;

   class MyFile final : public runtime::fs::FileSystemFile {
       // read_at, write_at and close.
   };

   class MyStore final : public runtime::fs::FileSystem {
       // open, which makes a MyFile, stat and list.
   };

   runtime::fs::register_file_system("mystore", dstd::make_shared<MyStore>());

Now ``runtime::fs::find``, ``walk``, ``stat``, ``stat_many``, ``read_file`` and ``write_file`` work on ``mystore://bucket/path``.
Override ``stat_batch`` when many stats in one call cost less than one call for each.
A path with no scheme and a ``file://`` path are local, and the scheme ``file`` cannot be replaced.

Limits
------

The rest of the engine, such as the index and the trace readers, reads local files only.
Only ``runtime::fs`` uses a registered file system today.
