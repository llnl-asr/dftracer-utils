:description: Reference for the C++ key-value store: Store, Namespace, Batch, Snapshot and Store::pack.

Key-value store
===============

.. seealso::

   :doc:`../guides/runtime/key-value-store` for how to use the store.

The key-value store in ``dftracer::utils::store``: ``Store`` opens a directory, ``Batch`` holds one atomic commit, and ``Snapshot`` reads a point-in-time view.

.. include:: /cpp_api/_generated/store.rst.inc
