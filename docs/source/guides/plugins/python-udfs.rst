:description: Register a Python function as a scalar UDF, compiled to native code or called once per batch on one Python thread.

Write a UDF in Python
=====================

A scalar UDF is a function that the engine calls on whole columns. ``@dft.udf``
registers a Python function as one, in two forms. Both give an owning
``Udf`` that you call on expressions, and both register the
name for duql.

.. literalinclude:: ../../../../examples/jit/ref_udf.py
   :language: python
   :start-at: @dft.udf  # compiled
   :end-at: out = lazy.with_columns

In duql the UDF is a function, ``derive s = score(dur, size)``.

Compiled form
-------------

``@dft.udf`` on a function whose arguments and result are annotated ``float``,
``int`` or ``bool`` compiles the body to C, builds it once into the JIT cache
and registers it as a native UDF. It runs on the engine's workers and never
takes the GIL.

The body is a per-row subset in the style of ``@jit.each_event`` (see :ref:`jit-conditions`), with ``//``, ``%`` and ``**``: arithmetic
(``+ - * / // % **``), comparisons (``a < x <= b``), ``and``, ``or``, ``not``,
``a if c else b``, ``abs``, ``min``, ``max``, ``float``, ``int``, ``bool``, the
``math`` functions that C has (``sqrt``, ``exp``, ``log``, ``sin``, ``floor`` and
the rest), local variables, and ``if``, ``elif``, ``else`` and ``return``. A
module level ``int``, ``float`` or ``bool`` is read as a constant when the
function is decorated.

- A ``float`` argument takes any numeric column and an ``int`` argument any
  integer column, widened per row. A ``Uint64`` column (the trace columns
  ``pid``, ``tid``, ``ts`` and ``dur``) binds to an ``int`` argument too; each
  batch is range checked once before the loop, and a value above ``2**63 - 1``
  fails the call with the argument number, the row and the value. A ``bool``
  argument takes a ``Bool`` column.
  Another type is an error when the call is bound.
- Nulls: a row with a null argument gives a null result, and the loop skips it.
- A condition, and an operand of ``and``, ``or`` and ``not``, must be a ``bool``;
  compare explicitly (``x != 0``).
- ``int`` wraps at 64 bits. ``/`` and ``**`` give a ``float``, and an
  ``int ** int`` needs a non-negative literal exponent. ``/``, ``//`` and ``%``
  by zero, on ints or floats, and an ``int()`` of a non-finite ``float``, fail the
  call, as Python raises ``ZeroDivisionError`` and ``OverflowError``. A float
  ``//`` and ``%`` use CPython's algorithm, so ``1.0 // 0.1`` is ``9.0``.
- The ``math`` functions are the C ones: ``math.log(0)`` is ``-inf`` and
  ``math.sqrt(-1)`` is ``nan``, not an error.
- ``volatile=True`` says the result can differ for the same arguments.

A body outside the subset is a located ``JitError``
(``file:line:col``) at decoration time. The compiled form never falls back to
the per-batch form on its own: write ``@dft.udf(batch=True)`` for a body that
needs Python.

Per-batch form
--------------

``@dft.udf(batch=True)`` keeps the function as Python. The engine calls it once
per batch with one numpy array for each argument column and takes back an array
of the batch length.

- A numeric column is a zero-copy, read-only view of the engine's buffer. The
  array owns a share of the column, so a view the function keeps stays valid and
  read-only, and holds that batch in memory until it is dropped. (numpy takes no
  export of a ``memoryview``, so a guard that releases one cannot make a kept
  array fail.) A ``bool`` column arrives bit-packed and is unpacked into a new
  array.
- A string column is a typed error when the call is bound: the per-batch form
  takes numeric and ``bool`` columns.
- Nulls: the engine nulls the result of every row where an argument is null, so
  the function may compute anything for such a row.
- The result dtype comes from ``returns=`` (``returns=np.float64``) or from the
  return annotation ``npt.NDArray[np.float64]``. The function must return a
  one-dimensional array of exactly the batch length. A dtype that can be cast
  safely (``int32`` to ``float64``) is cast; any other is an error. A ``bool``
  result must be a ``bool`` array.
- An exception in the function, a wrong length and a wrong dtype fail the query.
  The caller gets a ``DFTUtilsValueError`` whose text is
  ``udf 'name' exec: <ExceptionType>: <message>``.

The GIL is held while the function runs. Every per-batch call runs on one
dedicated lane thread, started with the first such UDF and stopped when the last
is gone or at interpreter exit. A worker queues its batch and waits without the
GIL, and the engine releases the GIL while it waits for its workers, so the
workers never wait on the GIL themselves and another Python thread keeps
running. Numpy releases the GIL in its own loops. Batches run one after another
on the lane. A build of Python without the GIL could run several lane threads;
the queue and the hand-off of a batch already allow it, and the lane is the one
place to change.

Every engine entry that can run an expression releases the GIL while it waits:
``collect``, ``collect_async``, ``collect_all``, ``stream``, the Arrow stream,
``DataFrame.group_by(...).agg(...)``, ``Expr.apply`` and the trace viewer's duql.

When a run is cancelled, a batch still queued fails with a cancelled status. A
batch that is already running cannot be cancelled or interrupted, because it
reads the engine's buffers: the run waits for it. At interpreter exit the open
UDFs are closed and the lane stops: a batch that is running finishes first, the
queued ones fail as cancelled. A function that never returns therefore blocks
the exit.

Registry and scope
------------------

``dft.udf`` is the read-only mapping of registered UDFs (``dft.udf["score"]``,
``.get``, ``in``, iteration, ``len``) and the decorator. A name that is already
taken, by a UDF, a built-in duql function or an op, is a ``ValueError`` that
names both definitions.

A UDF made by ``@dft.udf`` stays registered until you close it. It is never
removed because the object was garbage collected, so a decorated function keeps
working when only its name is used. Close it with ``score.close()`` (twice is
fine), ``with score:``, or the end of a scope:

.. literalinclude:: ../../../../examples/jit/ref_udf.py
   :language: python
   :start-at: with dft.udf.scope():

After ``close`` the name is gone and ``score(...)`` raises ``RuntimeError``, but
an expression or plan that was built before keeps the UDF and still runs. A scope
is process wide, not per thread, and scopes nest.

See also :doc:`../../jit` for the per-row subset, and :doc:`../../plugins` for a
UDF written in C or C++ that a plugin registers.
