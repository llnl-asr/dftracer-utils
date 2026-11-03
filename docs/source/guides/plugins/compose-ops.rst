:description: Write a small reusable async transform and pipe it with | && ||, either as a core C++ callable or as a Python op.

Author a compose op
=====================

.. admonition:: Goal
   :class: goal

   Write a small, reusable async transform and pipe it with others, either
   inside the core library (a plain C++ callable) or in Python (``@jit.op``). This
   is the how-to for the model :doc:`../../concepts/compose` explains; read that
   first for why the model looks like this.

In the core library: a plain callable
-------------------------------------------

An async op is any callable of shape ``I -> coro::CoroTask<O>`` - no base
class, no registration. Write it as a lambda, a free function, or a struct
with ``operator()``, then wrap it with
``dftracer::utils::utilities::op`` (``dftracer/utils/runtime/compose.h``)
to get the ``|`` / ``&&`` / ``||`` composition operators:

.. code-block:: cpp

   #include <dftracer/utils/runtime/compose.h>
   namespace u = dftracer::utils::utilities;

   auto doubler = u::op([](int x) -> coro::CoroTask<int> { co_return x * 2; });
   auto plus10  = u::op([](int x) -> coro::CoroTask<int> { co_return x + 10; });

   auto pipeline = doubler | plus10;               // int -> CoroTask<int>
   int result = co_await pipeline(5);               // 20

Compose it further with ``u::map``/``u::fold`` for a parallel map-reduce, or
submit it straight to a :doc:`Runtime <../../concepts/async-runtime>` -
``rt.submit(pipeline(5))`` - since there is no virtual dispatch to cross. See
:doc:`../../concepts/compose` for ``&&``/``||`` and the scope-injection
pattern for an op that needs a ``CoroScope``.

In a plugin: a host op
-----------------------

A host-provided utility is a named op: run it through
``DFTU_SVC_OPS`` (``Host::run_op("dftu.hash.fnv1a", {column})``) on a column the
plugin already holds. See :doc:`../../plugins` for the plugin lifecycle.

In Python: ``@jit.op``
------------------------

``jit.op`` (``dftracer.utils.jit``) turns a single-argument, single-expression
Python function into the ``In -> Out`` compose leaf. The body must be one ``return`` of arithmetic on the
argument; anything else raises ``JitOpError`` at decoration time.

.. code-block:: python

   from dftracer.utils import jit

   @jit.op
   def double(x: jit.i64) -> jit.i64:
       return x * 2

   @jit.op
   def plus10(x: jit.i64) -> jit.i64:
       return x + 10

   pipeline = double | plus10       # i64 -> i64; a mismatched pipe raises JitOpError
   result = pipeline(5)             # 20

Calling a piped op JIT-compiles it and runs it on a standalone compose host
(no plugin, no scan needed) - useful for testing the op's logic in isolation.
The same op referenced from inside a ``@jit.each_event`` body is inlined into
the compiled plugin as a static C function instead, with no host round-trip;
see :doc:`../../jit` for that authoring surface.

Applied to a whole column (``double(series)``), a ``@jit.op`` is lowered to
the engine's expression IR rather than called per element: 2M values run
in a third of a second, where the per-element path through the host took
hours. ``can_fuse()`` on the op says which path a call will take; a body
the IR cannot express keeps the per-element path.

In Python: ``@jit.series``, a column op from an expression
-------------------------------------------------------------

For a reusable **column** op there is no compile step at all: ``@jit.series``
takes a function whose body builds a column expression from its arguments
(the :mod:`~dftracer.utils.columnar` DSL: arithmetic, comparisons, ``clip``
/ ``cast`` / ``fillna``, the prims) and registers it as an engine op under
``<module>.<name>``, next to the built-ins:

.. code-block:: python

   from dftracer.utils import jit
   from dftracer.utils.jit import ops

   @jit.series(module="stats")
   def weighted(dur, w):
       return dur * w + 1

   weighted(dur, w)                  # runs through the Expr evaluator, fused
   ops.run("stats.weighted", dur, w) # the same op by registry name
   dur.ops.stats.weighted(w)         # or as a Series method
   ops.info("stats.weighted")        # {"kind": "series", "arity": 2, ...}

The module prefix defaults to the defining module (``module=`` chooses
it); a bare name or the ``dftu.`` namespace is refused, the same rule a C
plugin's ``register_op`` enforces. Once registered, the op is reachable
from every surface that reads the registry: ``ops.run`` from Python,
``dftu_op_run`` from C, a plugin's ``Host::run_op``, and a ``LazyFrame``
plan.

See also
--------

- :doc:`../../concepts/compose` for the ``AsyncOpFor`` contract and the full
  operator set (``&&``, ``||``, ``map``, ``fold``).
- :doc:`../../plugins` for the plugin lifecycle (``step``/``merge``/``finalize``)
  a compose op runs inside.
- :doc:`../../jit` for ``@jit.plugin`` and ``@jit.each_event``, the Python
  plugin-authoring surface ``@jit.op`` complements.
- :doc:`inter-plugin-comms` for the ports model when two plugins need to
  share a value, a different problem from composing one op with another.
