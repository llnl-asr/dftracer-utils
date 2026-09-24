:description: Reference for the C++ indexer: building and querying the RocksDB-backed index over trace events.

Index
=====

.. seealso::

   :doc:`../guides/core/indexing` and :doc:`../concepts/indexing-and-pushdown`
   for the indexing model, and :doc:`../c_api/indexer` for the flat C ABI.

The index module in ``dftracer::utils::index``. ``Indexer``
(``dftracer/utils/index/indexer.h``) is the entry point for building and
inspecting an index; the sections after it document the installed layer
types (RocksDB storage, gzip checkpoints, index extensions, the dftracer
record schema and its aggregation tier, the chunk indexer and the pruner) that
other public headers still expose.

Type relationships
------------------

Composition among the indexer types:

.. mermaid:: /_generated/index.mmd

.. include:: /cpp_api/_generated/index.rst.inc
.. include:: /cpp_api/_generated/index.store.rst.inc
.. include:: /cpp_api/_generated/index.gzip.rst.inc
.. include:: /cpp_api/_generated/index.extensions.rst.inc
.. include:: /cpp_api/_generated/index.schemas.rst.inc
.. include:: /cpp_api/_generated/index.schemas.dft.agg.rst.inc
.. include:: /cpp_api/_generated/index.build.rst.inc
.. include:: /cpp_api/_generated/index.plan.rst.inc
