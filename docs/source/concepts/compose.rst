:description: How async work is composed: stages, the | adaptors of coro/stage.h, and why they replaced the Op wrapper and the old Utility base class.

The compose model
==================

What this explains: how async work is authored and combined in
``dftracer_utils_coro`` - the stage concepts and the ``|`` adaptors in
``coro/stage.h`` - and why they replaced the ``Op<F>`` wrapper with its
``&&`` / ``||`` operators and, before that, the ``Utility`` base-class pattern.

The contract: a stage is a type with a ``run``
------------------------------------------------

A stage is a small type that names its input and output and has a ``run``.
Two shapes exist.

.. code-block:: cpp

   #include <dftracer/utils/coro/stage.h>
   namespace coro = dftracer::utils::coro;

   struct Doubler {                        // BatchStage: one value in, one value out
       using Input = int;
       using Output = int;
       static constexpr const char* NAME = "doubler";
       coro::CoroTask<int> run(int x) const { co_return x * 2; }
   };

   struct Positive {                       // StreamStage: a stream in, a stream out
       using Input = int;
       using Output = int;
       static constexpr const char* NAME = "positive";
       coro::AsyncGenerator<int> run(coro::AsyncGenerator<int> in) const;
   };

``BatchStage`` takes ``run(Input) const -> CoroTask<Output>``.
``StreamStage`` takes ``run(AsyncGenerator<Input>) const -> AsyncGenerator<Output>``.
The name is a static ``NAME`` or, for a wrapper with a runtime name, a member ``name()``.
``coro::stage_name(s)`` reads either.
``run`` is const, takes its input by value and is safe to call from several tasks at once.
A stage must outlive every run it started.

There is no base class, no registration step and no virtual dispatch.
A stage is a plain value, so a chain of stages is a nested type that the compiler can inline.

Adaptors: ``|`` applies an adaptor
-------------------------------------

``|`` always applies an adaptor, as it does in C++20 ranges and in ``std::execution``.
``x | a`` is ``a(x)``.
``a | b`` joins two adaptors into one, and ``x | (a | b)`` is ``(x | a) | b``.
There is no ``&&`` or ``||``: to run two things at once or to race them, use ``coro::when_all`` and ``coro::when_any``.

An adaptor works on a ``CoroTask``, on an ``AsyncGenerator`` and, where it says so, on a stage.

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Adaptor
     - Meaning
   * - ``then(next)``
     - Feed the result into ``next``, a stage or a callable that returns a value or a ``CoroTask``.
       On a task it gives a task.
       On a generator it runs a batch stage or callable per item, or a stream stage on the whole stream.
       On a stage it gives a composed stage named after the first.
   * - ``recover(f)``
     - Run ``f(dstd::Error)`` when a task or batch stage fails.
       A cancelled task stays cancelled and never reaches ``f``.
   * - ``named(label)``
     - Report ``label`` as the name of a stage.
   * - ``timeout(d)``
     - End a task or batch stage with a ``Timeout`` error when ``d`` passes.
       On a stream it bounds the wait for each item, and the source runs in the background and reads one item ahead.
   * - ``limit(n)``
     - At most ``n`` runs at once across every copy of the adaptor.
       A stream holds its slot until it ends, fails or is dropped.
   * - ``maybe(cond, a)``
     - Apply ``a`` when ``cond`` is true, else pass the value through.
       ``a`` must give back the type it is given.

.. code-block:: cpp

   auto stage = Doubler{} | coro::then(Tripler{}) | coro::named("scale")
                          | coro::timeout(5s) | coro::limit(4);
   int six_times = co_await stage.run(7);              // 42

   auto rescued = fetch_value() | coro::recover([](dstd::Error) { return 0; });

Wrappers keep the kind and the types of the stage they wrap.
The outermost wrapper decides the name.
``then`` on two stages gives batch for batch then batch, stream for stream then stream, and stream for stream then batch (per item).
Batch then stream does not compile.

Choosing between stages
-------------------------

``coro::either(cond, a, b)`` holds only the chosen side.
An ``Either`` of two stages of the same kind and input works wherever a stage is taken, through ``EitherStage``.
Its output is the common type when both agree and a ``dstd::Either`` of the two outputs when not.
``co_await`` on an ``Either`` of two tasks gives an ``Either`` of their values.

Type erasure, when you want it
--------------------------------

``coro::erase(stage)`` gives a ``DynStage<In, Out>`` or a ``DynStreamStage<In, Out>``.
It is move-only, models the same concept and forwards ``name()``.
Use it where one type must hold many different stages, such as a table of stages chosen at run time.
Nothing else in the model erases a type.

Context is optional, not structural
-------------------------------------

A stage that needs a :doc:`Scope <async-runtime>` to spawn children opens one inside its ``run`` with ``coro::with_scope``.
No base-class hook injects it.

Why this replaced ``Utility``
-------------------------------

The codebase previously organized domain logic as ``Utility<Input, Output, Tags...>`` subclasses: a base class with a ``process(const Input&)`` coroutine method, opt-in cross-cutting behavior via marker tags (``Parallelizable``, ``Cacheable``, ``Monitored``, ``Retryable``), and a behavior-chain wrapper (``UtilityExecutor``) that interpreted the tags at each call site.
That machinery has been deleted from the codebase.

The reason it was removed:

- **The tag/behavior chain was indirection with no matching payoff.**
  Every cross-cutting concern (retry, caching, monitoring) had to be threaded through a shared executor that inspected tags at runtime, when the same concern is just another adaptor a caller wraps around the stage it modifies.

The ``Op<F>`` wrapper that followed it composed bare callables and used ``&&`` and ``||`` to run two ops at once or to race them.
It is gone too.
A callable has no name and no declared input or output, so a pipeline of them could not be named in a progress report, wrapped by a timeout that keeps its kind, or erased.
A stage has all three, and the operators that hid a ``when_all`` or a ``when_any`` behind ``&&`` and ``||`` are now written as the calls they are.

Static submission
------------------

Because there is no virtual dispatch, a stage run (composed or not) can be submitted to the :doc:`Runtime <async-runtime>` directly - ``rt.submit(stage.run(x))`` - or awaited inline inside another coroutine.

See also
--------

- :doc:`async-runtime` for ``CoroTask``, ``Scope``, and the runtime that drives both.
- :doc:`../guides/plugins/compose-ops` for writing a stage, and for composing ops in a plugin.
