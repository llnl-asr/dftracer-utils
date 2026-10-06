:description: The flat C ABI to the indexer: open a set of traces, report index status, build or rebuild the index, list indexed files and open Views via index/abi.h and trace/views/abi.h.

Indexer
=======

.. seealso::

   :doc:`../cpp_api/indexer` for the C++ ``Indexer`` that backs this ABI, and
   :doc:`../guides/core/indexing` for the indexing model.

The flat C interface to the indexer. Include ``dftracer/utils/index/abi.h``.
Calls block until done and return a ``DFTU_RESULT`` value-or-error; an
error message is borrowed until the next call on the same indexer. The
aggregation tier, bloom tuning, a caller-chosen runtime and the async forms
are C++ only.

.. doxygenfile:: dftracer/utils/index/abi.h
   :project: dftracer-utils

Views
-----

To read events from C, use the View C ABI in
``dftracer/utils/trace/views/abi.h``. A ``dftu_view`` is a pure plan over
indexed trace files. ``dftu_view_duql`` and ``dftu_view_explain_duql`` run and
explain a duql pipeline (see :doc:`../reference/duql`).

.. doxygenfile:: dftracer/utils/trace/views/abi.h
   :project: dftracer-utils
