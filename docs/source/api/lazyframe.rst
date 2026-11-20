:description: Reference for LazyFrame, lazy and scan_file: a deferred query plan over a native frame or a trace file.

LazyFrame
=========

A ``LazyFrame`` is a deferred query over a native frame or a file. Nothing runs
until ``collect`` (see :doc:`../columnar-engine` and :doc:`../quickstart`).

.. autoclass:: dftracer.utils.LazyFrame
   :members: collect, schema, columns

.. autofunction:: dftracer.utils.lazy

.. autofunction:: dftracer.utils.scan_file

See also
--------

- :doc:`dataframe` for the eager frame, and :doc:`columnar` for the expressions
  a plan evaluates.
