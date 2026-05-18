JSON Module
===========

The ``JsonDictValue`` class is a zero-copy wrapper over a parsed DFTracer JSON
event. It is the element type yielded by
:meth:`~dftracer.utils.TraceReader.iter_json` and
:meth:`~dftracer.utils.TraceReader.read_json`. The underlying bytes are owned
by the C++ reader buffer; call :meth:`JsonDictValue.to_dict` to materialize a
regular Python dict for storage beyond the iterator's lifetime.

JsonDictValue Class
-------------------

.. autoclass:: dftracer.utils.JsonDictValue
   :members: keys, values, items, get, to_dict
   :undoc-members:
   :show-inheritance:
   :special-members: __getitem__, __contains__, __len__

.. code-block:: python

   reader = TraceReader("trace.pfw.gz")
   for event in reader.iter_json():
       name = event["name"]                  # __getitem__
       if "args" in event:                   # __contains__
           ret = event["args"].get("ret")    # nested dict access
       owned = event.to_dict()               # materialize to plain dict
