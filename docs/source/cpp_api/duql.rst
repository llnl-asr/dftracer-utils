:description: Reference for the C++ duql filter language: the predicate IR, the field builder, the string codec, and the evaluator that runs predicates over columns.

duql
====

.. seealso::

   :doc:`../concepts/indexing-and-pushdown` for how predicates push down into
   the index, and :doc:`../c_api/duql` for the flat C ABI.

The duql filter language in ``dftracer::utils::duql``: the predicate IR, the
field builder, the string codec, and the evaluator that runs predicates over
columns.

Type relationships
------------------

The predicate IR node types and how they nest:

.. mermaid:: /_generated/duql.mmd

.. include:: /cpp_api/_generated/duql.rst.inc
