:description: The flat C ABI to the columnar engine: construct, inspect, free, and operate on columns from any language or plugin via dataframe/c_api.h.

DataFrame
=========

.. seealso::

   :doc:`../cpp_api/dataframe` for the C++ ``Series`` / ``DataFrame`` types that
   back this ABI.

The flat C interface to the columnar engine: construct, inspect, and free
columns, and run the columnar operators, from any language or a plugin. Include
``dftracer/utils/dataframe/c_api.h``.

The buffer getters below (``dftu_series_validity``, ``dftu_series_data``,
``dftu_series_offsets``, ``dftu_series_offsets64``, ``dftu_series_chunk*``,
``dftu_series_dictionary_*``, ``dftu_series_view_*``) return the column's own
buffers, borrowed and zero copy, in Arrow's layout. The borrowed
constructors (``dftu_series_new_flat_borrowed``,
``dftu_series_new_string_borrowed``, ``dftu_series_new_string_view``) do the
reverse: they wrap Arrow memory as a column without copying it. Together they
let a plugin build Arrow arrays from a column and hand Arrow memory back, with
its own Arrow library (see :doc:`../plugins` and
``examples/plugins/arrow_convert.c``).

.. doxygenfile:: dftracer/utils/dataframe/c_api.h
   :project: dftracer-utils
