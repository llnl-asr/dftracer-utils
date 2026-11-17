:description: Reference for the C++ Arrow edge: Batch, conversions of a Series, DataFrame or LazyFrame to and from the Arrow C Data Interface, C Stream Interface streams and IPC files.

Arrow
=====

.. seealso::

   :doc:`../guides/analysis/export` for moving results to pandas, polars and Arrow from Python.

The Arrow edge in ``dftracer::utils::arrow``: ``Batch`` owns an Arrow array, the conversions export and import a ``Series`` or ``DataFrame`` through the Arrow C Data Interface, the stream functions do the same for the C Stream Interface, and ``Writer``, ``Reader`` and ``scan_ipc`` read and write Arrow IPC files.
The engine itself never uses Arrow; this library is the boundary.

.. include:: /cpp_api/_generated/arrow.rst.inc
