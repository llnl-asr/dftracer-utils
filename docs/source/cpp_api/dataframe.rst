:description: Reference for the native columnar compute layer: typed Series columns, DataFrame, Highway SIMD kernels, and the zero-copy Arrow bridge.

DataFrame
=========

.. seealso::

   :doc:`../columnar-engine` for the engine internals, the
   :doc:`../guides/data/dataframe` and :doc:`../guides/data/series` guides for
   task-oriented usage, and :doc:`../c_api/dataframe` for the flat C ABI that
   foreign languages and plugins build columns through.

The native columnar compute layer in ``dftracer::utils::dataframe``: typed
columns (``Series``) in flat/dictionary/selection encodings, a ``DataFrame`` of
such columns, SIMD (Highway) kernels, and a zero-copy Arrow C Data Interface
bridge. Both ``Series`` and ``DataFrame`` are value types with a fluent, const
method API - each operator returns a new frame or column, so calls chain.

.. code-block:: cpp

   #include <dftracer/utils/dataframe/dataframe.h>

   using namespace dftracer::utils::dataframe;

   DataFrame top = df.filter(df.column("cat").str_eq("POSIX"))
                       .sort_by("dur", /*descending=*/true)
                       .select({"name", "dur"})
                       .head(10);

   Series dur = df.column("dur");
   double p99 = dur.quantile(0.99);
   DataFrame out = df.with_column("dur_ms", dur * 0.001).value();

The field builder (``F`` / ``Field`` / ``Expr``) in
``dftracer::utils::dataframe::field`` composes column expressions that the engine
evaluates in one pass.

Type relationships
------------------

How the columnar types compose:

.. mermaid:: /_generated/dataframe.mmd

.. include:: /cpp_api/_generated/dataframe.rst.inc
.. include:: /cpp_api/_generated/dataframe.field.rst.inc
