:description: Reference for the C++ indexer: building and querying the index over trace events.

Index
=====

.. seealso::

   :doc:`../guides/core/indexing` and :doc:`../concepts/indexing-and-pushdown`
   for the indexing model, and :doc:`../c_api/indexer` for the flat C ABI.

The index module in ``dftracer::utils::index``. ``Indexer``
(``dftracer/utils/index/indexer.h``) is the entry point for building and
inspecting an index.
The sections after it also document the aggregation configuration types that ``indexer.h`` exposes.

Type relationships
------------------

Composition among the indexer types:

.. mermaid:: /_generated/index.mmd

.. include:: /cpp_api/_generated/index.rst.inc
.. include:: /cpp_api/_generated/index.schemas.dft.agg.rst.inc
