:description: How async ops are authored and combined: the Op<F> wrapper and the ranges-style | && || operators that replaced the old Utility base class.

The compose model
==================

What this explains: how async operations are authored and combined in
``dftracer_utils_runtime`` - the ``Op<F>`` wrapper and the ranges-style ``|`` /
``&&`` / ``||`` operators in ``runtime/compose.h`` - and why it replaced
the older ``Utility`` base-class pattern.

The contract: a callable, nothing more
---------------------------------------

An async op is any callable of shape ``I -> coro::CoroTask<O>``:

.. code-block:: cpp

   template <typename F, typename I>
   concept AsyncOpFor = requires(F& f, I in) {
       typename detail::coro_value<std::invoke_result_t<F&, I>>::type;
   };

That is the entire authoring surface. A free function, a lambda, or a struct
with ``operator()`` all satisfy it - there is no base class to derive from, no
virtual dispatch, and no registration step. ``dftracer::utils::utilities::op(f)``
wraps such a callable in ``Op<F>``, a thin value type that stores ``f`` by
value and adds the composition operators below:

.. code-block:: cpp

   #include <dftracer/utils/runtime/compose.h>
   namespace u = dftracer::utils::utilities;

   struct Doubler {
       coro::CoroTask<int> operator()(int x) const { co_return x * 2; }
   };
   auto stringify = [](int x) -> coro::CoroTask<std::string> {
       co_return std::to_string(x);
   };

   auto pipeline = u::op(Doubler{}) | u::op(stringify);   // int -> CoroTask<string>

Because a composed op is itself just a nested ``Op<F>`` built at compile time,
``a | b`` produces a new value with no type erasure beyond the coroutine frame
each leaf op already allocates. The whole chain inlines.

The operators
-------------

Four combinators cover fan-out, fan-in, and racing, mirroring C++20 ranges'
pipe style:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Operator
     - Meaning
   * - ``a | b``
     - Pipe: run ``a``, feed its result into ``b``.
   * - ``a && b``
     - All: run both on the same input concurrently (``when_all``), yield a
       ``std::tuple`` of both results.
   * - ``a || b``
     - Any: race both on the same input (``when_any``), yield whichever
       finishes first. Both ops must yield the same output type.
   * - ``u::map(f)``
     - Lift ``Item -> CoroTask<O>`` to ``vector<Item> -> CoroTask<vector<O>>``,
       running every element concurrently.
   * - ``u::fold(init, combine)``
     - Collapse a ``vector<Item>`` into one value with a synchronous,
       left-to-right ``combine``.

``map`` and ``fold`` compose with ``|`` like any other op, so a parallel
map-reduce is one pipe:

.. code-block:: cpp

   auto sum_of_doubles =
       u::map(Doubler{}) | u::fold(0, [](int acc, int x) { return acc + x; });

   int total = co_await sum_of_doubles(std::vector<int>{1, 2, 3, 4});  // 20

Context is optional, not structural
------------------------------------

An op that needs a :doc:`CoroScope <async-runtime>` (to spawn children, reach
the I/O backend, or read cancellation) takes it as a leading parameter rather
than through a base-class hook. A struct can offer both a scope-injected and a
scope-less overload, so it composes either as a standalone op or as a node fed
a caller-owned scope:

.. code-block:: cpp

   struct ScopedSum {
       coro::CoroTask<int> operator()(CoroScope& scope, int x) const {
           auto a = scope.spawn([x](CoroScope&) -> coro::CoroTask<int> { co_return x; });
           auto b = scope.spawn([x](CoroScope&) -> coro::CoroTask<int> { co_return x * 10; });
           co_return (co_await a) + (co_await b);
       }
       coro::CoroTask<int> operator()(int x) const {
           return dftracer::utils::with_scope(*this, x);   // provides its own scope
       }
   };

``dftracer::utils::with_scope`` is the seam that lets the same op run either
injected (a caller already inside a scope passes it through) or standalone
(the op opens one for itself). Neither path needs a tag or a behavior wrapper
to opt in.

Why this replaced ``Utility``
-------------------------------

The codebase previously organized domain logic as ``Utility<Input, Output,
Tags...>`` subclasses: a base class with a ``process(const Input&)`` coroutine
method, opt-in cross-cutting behavior via marker tags (``Parallelizable``,
``Cacheable``, ``Monitored``, ``Retryable``), and a behavior-chain wrapper
(``UtilityExecutor``) that interpreted the tags at each call site. That
machinery has been deleted from the codebase.

The reason it was removed:

- **The tag/behavior chain was indirection with no matching payoff.** Every
  cross-cutting concern (retry, caching, monitoring) had to be threaded
  through a shared executor that inspected tags at runtime, when the same
  concern is just another op a caller wraps around the one it modifies. A
  plain function composes with ``|`` the same way a tagged ``Utility`` did,
  without the base class or the tag vocabulary.

With the base class gone, an op is exactly what its signature says it is: a
callable from ``I`` to ``CoroTask<O>``, composed with the same four operators
whether it is a leaf, a pipeline, or a whole subsystem's entry point.

Static submission
------------------

Because there is no virtual dispatch, an op (composed or not) can be submitted
to the :doc:`Runtime <async-runtime>` directly - ``rt.submit(op(x))`` - or
awaited inline inside another coroutine. There is no erasure boundary an op
must cross to run.

See also
--------

- :doc:`async-runtime` for ``CoroTask``, ``CoroScope``, and the runtime that
  drives both.
