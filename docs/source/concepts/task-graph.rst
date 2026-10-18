:description: Why the core offers a DAG builder instead of hand-spawned coroutines, and the source/parallel/reduce node vocabulary a TaskGraph is built from.

The task-graph model
======================

What this explains: why ``dftracer_utils_runtime`` gives computations a DAG
builder (``runtime/task_graph/``) instead of leaving callers to spawn and join
coroutines by hand, and what shape that DAG takes.

Why a DAG, not ad-hoc spawns
------------------------------

A :doc:`CoroScope <async-runtime>` lets any coroutine ``spawn`` children and
``co_await`` them, which is enough to build arbitrary concurrency by hand.
For a fixed computation shape - fan a source out to N workers, reduce them
back to one result, maybe do that more than once - hand-written spawns mean
re-deriving the same dependency wiring, the same "wait for my inputs before I
run" bookkeeping, and the same tree-reduce fan-in every time. ``TaskGraph``
(``runtime/task_graph/task_graph.h``) factors that wiring into a builder: nodes
declare their inputs, the graph derives the dependency edges, and a
``Pipeline`` (``runtime/pipeline/``) runs the resulting DAG on the runtime.

The shape
---------

A graph is built from a small vocabulary of node kinds, each returning a
``TaskGroup<T>`` handle that feeds the next stage:

- **source** - one entry task producing a value.
- **parallel** - N independent tasks, optionally capped by
  ``max_concurrency`` (a sliding-window dependency chain, not a thread-pool
  limit, so it holds even on an unbounded runtime).
- **map** - a 1:1 transform over every task in a group.
- **fan_out** - one task's result distributed to N downstream outputs.
- **reduce** / **fold** - a ``k``-way tree reduction, ``O(log n)`` deep
  rather than a single N-way join, so the merge itself parallelizes.
- **aggregate** - map then reduce as one node.
- **partition** - split a ``std::vector<T>`` into N chunks for downstream
  parallel nodes.

Each node is still, underneath, a coroutine of shape ``CoroScope&, Args... ->
CoroTask<T>`` - the same contract the :doc:`compose <compose>` model's ops
satisfy. ``TaskGraph`` does not replace that contract; it is a way to wire
many such coroutines into a dependency graph without writing the wiring by
hand. ``graph.wrap<T>(task)`` brings an externally-built task in as an entry
point, so a graph can sit downstream of a hand-spawned coroutine or another
subsystem's task.

Building versus running
--------------------------

A ``TaskGraph`` only builds the DAG: calling its node methods records tasks
and dependency edges, and does no work. A ``Pipeline`` is what runs it -
``pipeline.set_source(...)`` then ``pipeline.execute()`` drives the graph to
completion on the runtime, after which a terminal group's task holds the
result (``task()->get<T>()``). Separating "describe the graph" from "execute
the graph" is what lets a graph be built once, inspected, or wrapped inside a
larger op before anything runs.

See also
--------

- :doc:`../guides/runtime/task-graphs` for the how-to: node signatures, the
  builder API, and a worked fan-out/reduce example.
- :doc:`compose` for the op-composition model a graph node's callable
  satisfies.
- :doc:`async-runtime` for ``CoroTask``, ``CoroScope``, and the runtime a
  ``Pipeline`` executes on.
