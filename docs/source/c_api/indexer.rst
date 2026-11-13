:description: The flat C ABI to the indexer: open a set of traces, report index status, build or rebuild the index, list indexed files and open Views via index/c_api.h and trace/views/c_api.h.

Indexer
=======

.. seealso::

   :doc:`../cpp_api/indexer` for the C++ ``Indexer`` that backs this ABI, and
   :doc:`../guides/core/indexing` for the indexing model.

The flat C interface to the indexer. Include ``dftracer/utils/trace/index/c_api.h``.
Calls block until done and return a ``dftu_status``, give the result through an
out-parameter and fill a caller-owned ``dftu_error`` on failure. Every
build makes the aggregation tier, and the indexer options set its interval and
percentiles. Bloom tuning, a caller-chosen runtime and the async forms are C++
only.

.. doxygenfile:: dftracer/utils/trace/index/c_api.h
   :project: dftracer-utils

Views
-----

To read events from C, use the View C ABI in
``dftracer/utils/trace/views/c_api.h``. A ``dftu_view`` is a pure plan over
indexed trace files. ``dftu_view_duql`` and ``dftu_view_explain_duql`` run and
explain a duql pipeline (see :doc:`../reference/duql`).

.. doxygenfile:: dftracer/utils/trace/views/c_api.h
   :project: dftracer-utils
