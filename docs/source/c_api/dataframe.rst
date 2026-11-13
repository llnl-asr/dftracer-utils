:description: The flat C ABI to the columnar engine: construct, inspect, free, and operate on columns from any language or plugin via dataframe/c_api.h.

DataFrame
=========

.. seealso::

   :doc:`../cpp_api/dataframe` for the C++ ``Series`` / ``DataFrame`` types that
   back this ABI.

The flat C interface to the columnar engine: construct, inspect, and free
columns, and run the columnar operators, from any language or a plugin. Include
``dftracer/utils/dataframe/c_api.h``.

.. doxygenfile:: dftracer/utils/dataframe/c_api.h
   :project: dftracer-utils
