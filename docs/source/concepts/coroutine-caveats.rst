:description: Coroutine lifetime rules and compiler bugs the async core works around, and the deliberate patterns you must not simplify away.

Coroutine caveats
=================

The async core of dftracer-utils is built on C++20 coroutines: ``CoroTask``,
channels, ``when_all`` / ``when_any``, and the ``CoroScope`` runtime. Coroutines
are what make the pipeline, the task graph, and the plugin engine read like
straight-line code while running concurrently. They also come with lifetime
rules and compiler bugs that are easy to trip over and hard to debug, because
the failures are silent memory corruption rather than clean errors.

This page explains those traps and the patterns the codebase uses to avoid them.
It is written for anyone about to touch the ``core/coro`` code, or wondering why
some of it looks the way it does.

Why some of this code looks unusual
-----------------------------------

Read the coroutine-adjacent code and you will find patterns that look like they
are asking to be "cleaned up": heavy locals wrapped in ``std::make_unique`` for
no obvious reason, lambda bodies extracted into free ``static`` functions that
are called once, ``shared_ptr`` arguments ``.reset()`` halfway through a
function, raw pointers threaded into child coroutines, and version-guarded
shapes that keep coroutine frames small on older compilers.

None of that is carelessness, and it is not the house style leaking. **Each of
those shapes is a deliberate mitigation for a coroutine lifetime rule or a known
compiler bug.** Simplifying them back to the "obvious" version reintroduces a
crash that is intermittent, compiler-specific, and painful to bisect - a
segfault that moves when you add a local, or a double-free that only appears at
``-O2`` on one GCC version. So before you flatten one of these patterns, check
whether it is load-bearing; the sections below are the map.

The rest of this page is the reasoning behind those patterns.

Dangling references
-------------------

When a coroutine suspends, its stack frame is gone. A reference parameter or a
reference-semantic type (``string_view``, ``span``) that points at caller-owned
memory dangles after the first suspension point - and the first suspension point
is ``initial_suspend``, which runs *before* the body. With lazy coroutines
(``suspend_always`` at ``initial_suspend``), a reference parameter is never safe
unless the caller provably outlives the coroutine.

.. code-block:: cpp

   // DANGEROUS: data may dangle after the first co_await.
   CoroTask<void> process(const std::string& data) {
       co_await something();
       use(data);                      // undefined behavior
   }

The mitigation is to **pass by value** (move large objects in), so the parameter
lives in the coroutine frame:

.. code-block:: cpp

   CoroTask<void> process(std::string data) {          // copied into the frame
       co_await something();
       use(data);                                       // safe
   }

For read-only data shared across parallel children, use a **structural lifetime
guarantee** - the parent's frame outlives the children, so a raw pointer is
sound:

.. code-block:: cpp

   CoroTask<void> parent() {
       auto lookup = build_map();                       // on the parent frame
       co_await when_all(child(&lookup), child(&lookup));
       // lookup is destroyed here, after all children complete
   }

This is why child coroutines in the codebase often take raw pointers into
parent-owned state rather than references or copies: it is the documented,
lowest-cost way to share immutable data across a fan-out.

Lambda coroutines
-----------------

When a lambda is itself a coroutine, its closure object (holding the captures) is
**separate** from the coroutine frame; the frame only references the closure.
Destroy the closure and every capture dangles, even while the coroutine runs.
This is the "Lambda Coroutine Fiasco," and ``CppCoreGuidelines`` CP.51 states it
plainly: *do not use capturing lambdas that are coroutines.*

A named coroutine function copies its parameters into the frame and has no such
split. So the codebase's rule is: **anything non-trivial is a named function**,
and a lambda passed to ``scope.spawn`` stays trivial, just calling the named
coroutine:

.. code-block:: cpp

   // The heavy work is a named function with its own frame.
   static CoroTask<void> process_group(Params params) {
       auto result = co_await heavy_operation();
       result.merge_maps();
       co_return;
   }

   // The spawned lambda is trivial: it only forwards.
   scope.spawn([params](CoroScope&) -> CoroTask<void> {
       co_await process_group(params);
   });

GCC frame corruption
--------------------

GCC's coroutine frontend (versions 12-13 especially) has miscalculated the frame
layout for locals, causing silent memory corruption: a segfault in
``std::_Hash_bytes`` reading an ``unordered_map`` key, a crash in
``shared_ptr::_M_release`` during coroutine destruction, a bug that appears or
vanishes when you add or remove a single local. The codebase hit this in
``scope.spawn`` paths, which is the origin of several of the patterns above.

What triggers it: non-trivial types (containers with destructors) on the frame,
many locals alive across ``co_await``, lambda coroutines with many captures, and
heavy by-value parameters. What helps, and what you will see in the code:

- **Extract lambda bodies into named functions** (a different frame layout).
- **Heap-allocate heavy locals** (``std::make_unique``) so they are a pointer on
  the frame, not an inline object.
- **Pass heavy data as shared_ptr** rather than by value, and ``.reset()``
  it early once the needed data has been extracted, to shrink the live frame.
- **Keep frames small** and scope locals tightly so fewer of them are alive
  across a suspension point.

Consolidating can beat splitting. The usual advice is "split large coroutines,"
but more intermediate coroutines means more frames that can be corrupted. In the
worst spots, collapsing a chain of nested helper coroutines into a single
pipeline coroutine (with one minimal public entry point that just moves its
arguments and returns) was the fix. Which approach wins is code-dependent; the
guiding metric is *fewer live non-trivial locals across suspension points.*

Symmetric transfer and the stack
-------------------------------

Returning a coroutine handle from ``await_suspend`` (symmetric transfer) keeps
the stack flat only when the compiler makes the resume a tail call. GCC does
that only with sibling-call optimization, which ``-O0``, ``-Og`` and ``-O1``
leave off (GCC 11 to 15, bug 100897). Without it, each awaited task that ends
at once adds stack frames, and a loop of such awaits overflows the 8 MiB
thread stack: a spilling join in a GCC Debug build did that after about
50,000 frames.

So ``CoroTask``, ``AsyncGenerator::next()`` and ``YieldCheckAwaitable`` do not
return a handle to continue. The awaiter resumes the child on its own stack
and returns ``bool``. The child's final suspend (or ``co_yield``) and the
awaiter both exchange the promise's ``handoff_`` flag, and the second to arrive
continues the awaiter: the awaiter, by not suspending, when the child ended at
once, or the child, by resuming the awaiter, when it suspended first. Do not
change these back to ``return handle``. ``tests/coro/test_async_generator.cpp``
awaits a million tasks that end at once to catch it.

Thread safety
-------------

After ``await_suspend`` publishes the coroutine handle to another thread, the
coroutine may be resumed and destroyed **before** ``await_suspend`` returns. Any
access to awaiter members or coroutine state after publishing is a data race.
The rules the codebase follows:

- After publishing the handle, treat ``*this`` as already destroyed.
- Use release/acquire ordering across the suspend/resume boundary.
- Do not hold a lock across a ``co_await`` (CP.52); keep the mutex outside the
  frame and reach it through a pointer.

Frame allocation
----------------

Every coroutine call heap-allocates a frame (promise, parameter copies, locals
that span suspend points, bookkeeping). The Heap Allocation eLision Optimization
(HALO) can put that frame on the caller's stack, but in practice it rarely fires
- it needs the ramp function inlined, the same translation unit, and bounded
wrapper code. Treat a coroutine call as an allocation in hot paths, which is
another reason the code prefers a few larger coroutines over many tiny ones.

Practical rules for this codebase
---------------------------------

#. Pass coroutine parameters **by value** (move large objects in); never by
   reference to caller-owned memory.
#. Use **named functions**, not lambda coroutines, for anything non-trivial.
#. Keep frames small - few non-trivial locals alive across ``co_await``.
#. **Heap-allocate** large containers that must live on a frame.
#. For read-only data shared across parallel coroutines, rely on **structural
   lifetime** (parent outlives children) with raw pointers.
#. Do not hold locks across suspension points; keep the mutex outside the frame.
#. Test on more than one compiler - GCC and Clang have different bug profiles.
#. Keep the clang-tidy coroutine checks on
   (``cppcoreguidelines-avoid-capturing-lambda-coroutines``,
   ``-avoid-reference-coroutine-parameters``, ``-no-suspend-with-lock``).

Compiler support
----------------

.. list-table::
   :header-rows: 1
   :widths: 20 20 20 40

   * - Compiler
     - Minimum
     - Recommended
     - Notes
   * - GCC
     - 12
     - 14+
     - 12-13 have frame-layout bugs; the mitigations above target them.
   * - Clang
     - 14
     - 17+
     - Thread-safety miscompiles in earlier versions.
   * - MSVC
     - 14.34
     - Latest
     - Most mature implementation.

References
----------

The mitigations above come from hard-won community experience. The most useful
starting points:

- Seastar, `Lambda Coroutine Fiasco <https://github.com/scylladb/seastar/blob/master/doc/lambda-coroutine-fiasco.md>`_
- Arthur O'Dwyer, `ways to get dangling references with coroutines <https://quuxplusone.github.io/blog/2019/07/10/ways-to-get-dangling-references-with-coroutines/>`_
- Lewis Baker, `Understanding Symmetric Transfer <https://lewissbaker.github.io/2020/05/11/understanding_symmetric_transfer>`_
- CppCon 2023, Francesco Zoffoli, `Problems and Solutions Using Coroutines in a Modern Codebase <https://isocpp.org/blog/2023/09/cppcon-2023-problems-and-solutions-using-coroutines-in-a-modern-codebase-fr>`_
- CppCoreGuidelines `CP.51 <https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rcoro-capture>`_ / `CP.52 <https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rcoro-locks>`_ / `CP.53 <https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rcoro-reference-parameters>`_
- GCC bugs `102217 <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=102217>`_ (ternary ``co_await`` double-free), `104177 <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=104177>`_ (frame alignment), `100897 <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=100897>`_ (symmetric transfer)
