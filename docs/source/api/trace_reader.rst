TraceReader Module
==================

The ``TraceReader`` is the recommended way to read trace files. It auto-selects
sequential or indexed reading based on whether an ``.idx`` sidecar exists.

TraceReader Class
-----------------

.. autoclass:: dftracer.utils.TraceReader
   :members:
   :undoc-members:
   :show-inheritance:
   :special-members: __init__, __enter__, __exit__

Streaming Iterators
-------------------

``iter_lines()`` and ``iter_raw()`` return Python iterators backed by a
bounded producer-consumer queue. The C++ coroutine runs on the Runtime's
thread pool and pushes items to the queue; Python's ``__next__`` pops from it.

.. code-block:: python

   reader = TraceReader("trace.pfw.gz")

   # Stream decoded lines
   for line in reader.iter_lines():
       process(line)  # str

   # Stream raw byte chunks (one line per chunk)
   for chunk in reader.iter_raw(multi_line=False):
       process(chunk)  # bytes

   # Materialize to list
   lines = reader.read_lines()   # list[str]
   chunks = reader.read_raw()    # list[bytes]

ReadConfig Parameters
---------------------

All reading methods accept these keyword arguments:

- ``start_line`` / ``end_line`` -- line range (0 = no limit)
- ``start_byte`` / ``end_byte`` -- byte range (0 = no limit)
- ``buffer_size`` -- internal buffer size in bytes (default 4MB)

``iter_raw`` and ``read_raw`` additionally accept:

- ``line_aligned`` -- if True, chunks are aligned to line boundaries (default True)
- ``multi_line`` -- if True, chunks may contain multiple lines (default True)
