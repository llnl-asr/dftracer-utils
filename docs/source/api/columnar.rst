:description: Build and evaluate Polars-style columnar value expressions (col/F/lit, eval_many) over the native SIMD DataFrame engine.

Columnar Expression DSL
========================

A Polars-style value-expression DSL over the native SIMD ``DataFrame`` engine.
Build an expression from ``col("dur")`` / ``F.dur`` and ``lit(...)``, combine it
with ``+ - * /`` and comparisons, then evaluate it against a native
``DataFrame`` (or a ``pyarrow.Table``, imported at the boundary) to get a
native :class:`~dftracer.utils.Series`. The expression stays columnar
throughout; Arrow appears only when the caller asks for it via
``.to_arrow()`` / ``.to_pandas()``.

This is the value-expression half of the DSL. The same ``F`` also builds row
filter predicates (the :doc:`duql` half); one unified :class:`Expr` covers
both, and the two compose - see :func:`~dftracer.utils.where`.

.. code-block:: python

   from dftracer.utils import F, lit, eval_many

   avg = (F.sum_dur / F.count).apply(df)          # a Series
   bucket = F.dur.ilog2().apply(df)                # unary primitive
   ms = (F.dur * lit(0.001)).apply(df)             # int * float -> float

   # Evaluate several expressions in one compiled, CSE'd pass:
   a, b = eval_many([F.sum_dur / F.count, F.max_dur - F.min_dur], df)

Building expressions
---------------------

Type relationships
------------------

How the value-expression DSL types relate:

.. mermaid:: /_generated/py_columnar.mmd

.. autofunction:: dftracer.utils.col

.. autofunction:: dftracer.utils.lit

``F`` is attribute sugar for ``col``: ``F.dur`` is ``col("dur")``, for any
column name that is a valid Python identifier. It is the single unified builder
exported from ``dftracer.utils`` (and re-exported from ``dftracer.utils.duql``):
the same field leaf builds value expressions here and filter predicates in
:doc:`duql`.

.. autoclass:: dftracer.utils.Expr
   :members: apply, to_duql

Arithmetic (``+ - * /``, with scalar broadcast), comparisons (``> >= < <=
== !=``), and the logical combinators (``& | ~``) are all operator overloads
on :class:`~dftracer.utils.Expr`; combine expressions the way you would combine
Python numbers. Numeric primitives (``ilog2``, ``bit_width``, ``popcount``,
``clz``, ``ctz``, ``mix64``) are available as zero-argument methods on any
integer-column expression, e.g. ``F.hhash.mix64()``. ``ColumnExpr`` is a
back-compat alias of ``Expr``.

Evaluating
-----------

.. autofunction:: dftracer.utils.columnar

.. autoclass:: dftracer.utils.columnar.Columnar
   :members: apply, columns

``columnar(expr)`` is optional - call ``.apply(source)`` directly on a
``ColumnExpr``, or use the frame-first spelling
:meth:`DataFrame.apply <dftracer.utils.DataFrame.apply>`. Wrapping with
``columnar()`` first is useful when you want to inspect ``.columns`` (the
input column names an expression reads) before evaluating.

``source`` for ``.apply()`` is a native ``DataFrame`` (e.g. from
``view.collect()``), a ``{name: Series}`` mapping, or a ``pyarrow.Table``
whose needed columns are imported into the engine at the boundary.

.. autofunction:: dftracer.utils.eval_many

``eval_many`` compiles every expression's AST in one pass, so a
subexpression shared across two or more of them (e.g. ``F.sum_dur /
F.count`` appearing in several outputs) is computed once via common
subexpression elimination. It returns one :class:`~dftracer.utils.Series`
per input expression, in the same order.

Row filtering
--------------

.. autofunction:: dftracer.utils.where

``where`` accepts either a ``ColumnExpr`` predicate (evaluated on the engine
to a boolean mask) or a duql string (see :doc:`duql`), and keeps the
matching rows of a native ``DataFrame`` or a ``pyarrow.Table``.

Group-by aggregation
----------------------

.. autofunction:: dftracer.utils.count

.. autoclass:: dftracer.utils.Agg
   :members: alias, out

An :class:`Agg` is an aggregate over a column expression - ``F.dur.mean()``,
``(F.a + F.b).sum()``, or the bare row count from ``count()``. The reducer
methods (``sum min max mean var std skew kurt``) are added to every
``ColumnExpr``; rename the output column with ``.alias(name)``.

.. autoclass:: dftracer.utils.GroupBy
   :members: agg

A ``GroupBy`` is obtained from a native ``DataFrame``'s ``group_by(key)`` and
consumed by ``.agg(*specs)``, where each spec is an ``Agg`` or a legacy
``"op:column"`` string; value expressions across all the aggregates compile
in one CSE'd, pruned pass. See :doc:`../guides/analysis/aggregation` and
:doc:`../guides/core/columnar-ops` for group-by usage alongside
:class:`~dftracer.utils.TraceViewer`.

See also
--------

- :doc:`dataframe`, :doc:`series` for the frame/column types these
  expressions evaluate against.
- :doc:`../guides/core/columnar-ops` for a task-oriented walkthrough (derived
  columns, group-by, reshaping).
- :doc:`../columnar-engine` for the engine internals (type inference, CSE,
  fused chunked/parallel evaluation).
