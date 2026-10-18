:description: Express a computation as a DAG of coroutine tasks - source, parallel workers, tree reduce - with TaskGraph and run it on a Pipeline (C++).

Build a task graph
==================

.. admonition:: Goal
   :class: goal

   Express a computation as a DAG of coroutine tasks - a source that fans
   out into parallel workers, then a tree reduce that fans them back in - and run
   it on the runtime. ``TaskGraph`` builds the graph; a ``Pipeline`` executes it.

This is a C++ API. There is no Python binding for ``TaskGraph``; from Python use
the higher-level :doc:`../../trace-viewer` and DataFrame APIs, which build their
own graphs internally.

The builder
-----------

``TaskGraph::builder`` (in
``dftracer/utils/runtime/task_graph/task_graph.h``, namespace
``dftracer::utils::task_graph``) returns a graph you attach nodes to. The config
struct takes a ``name`` and an optional ``max_concurrency`` (0 = unlimited):

.. code-block:: cpp

   #include <dftracer/utils/runtime/task_graph/task_graph.h>
   #include <dftracer/utils/runtime/pipeline/pipeline.h>
   using namespace dftracer::utils;
   using namespace dftracer::utils::task_graph;

   auto graph = TaskGraph::builder({.name = "MapReduce", .max_concurrency = 128});

Every task function takes a ``CoroScope&`` and returns a ``coro::CoroTask<T>``.
A ``parallel`` worker also receives its ``std::size_t`` index.

Nodes
-----

The graph methods wire dependencies for you and return a ``TaskGroup<T>`` you
pass to the next stage.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Method
     - Shape
   * - ``source<T>(func, opts)``
     - A single entry task producing a ``T``.
   * - ``parallel<T>(count, func, opts)``
     - ``count`` independent tasks; each receives its index.
   * - ``map<U>(group, mapper, opts)``
     - 1:1 transform of every task in ``group``.
   * - ``fan_out<U>(source, num_outputs{n}, mapper, opts)``
     - One task into ``n`` outputs.
   * - ``reduce<U>(group, split_every{k}, reducer, opts)``
     - Tree reduce, ``k``-way, O(log n) depth.
   * - ``fold<T>(group, init, split_every{k}, op, opts)``
     - Tree reduce with an initial value and a binary op.
   * - ``aggregate<U, Intermediate>(group, map_fn, split_every{k}, reduce_fn, opts)``
     - Map then reduce in one node.
   * - ``partition<T>(data, num_partitions{n}, opts)``
     - Split a ``std::vector<T>`` into ``n`` chunks.

``split_every``, ``num_outputs``, and ``num_partitions`` are explicit
strong-typed wrappers (from ``task_graph/types.h``) - pass ``split_every{2}``,
not a bare ``2``. Each method takes a trailing per-node config struct with a
``name`` (and, where it applies, a ``max_concurrency``).

Run it
------

The graph only builds the DAG. A ``Pipeline`` runs it: set the source from the
graph's entry tasks, call ``execute()`` (blocking), then read results off the
terminal group's task with ``get<T>()``.

.. code-block:: cpp

   auto workers = graph.parallel<int>(
       8,
       [](CoroScope&, std::size_t id) -> coro::CoroTask<int> {
           co_return static_cast<int>(id + 1);
       },
       {.name = "Worker"});

   auto reduced = graph.reduce<int>(
       workers, split_every{2},
       [](CoroScope&, std::vector<int> items) -> coro::CoroTask<int> {
           int sum = 0;
           for (int x : items) sum += x;
           co_return sum;
       },
       {.name = "Sum"});

   Pipeline pipeline(PipelineConfig::parallel(4));
   pipeline.set_source(workers.tasks());
   pipeline.execute();

   int total = reduced.task()->get<int>();   // 1 + 2 + ... + 8 == 36

``TaskGroup<T>`` is a handle to the tasks in a stage: ``task()`` returns the sole
task (throwing if the group holds more than one), ``tasks()`` returns them all,
and ``size()`` / ``at(i)`` iterate. Bring an externally-built task in with
``graph.wrap<T>(task)``.

See also
--------

- :doc:`../pipelines/patterns` - the lower-level fan-out/fan-in, racing, and
  channel patterns the graph is built on.
- :doc:`../../pipeline` - the ``Pipeline`` executor in more depth.
- :doc:`../../concepts/coroutine-caveats` - the coroutine rules the runtime
  enforces; read these before sharing state across tasks.
