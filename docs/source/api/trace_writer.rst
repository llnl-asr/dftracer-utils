:description: Reference for TraceWriter, which writes DFTracer records (events, counters, aggregated rows, metadata) to a gzip or plain trace file.

TraceWriter (Writing Traces)
============================

``TraceWriter`` writes DFTracer records to a trace file, one JSON line per
record. A file opened with the default ``compress=True`` is a multi-member gzip
file whose members hold about ``member_size`` bytes, so ``TraceViewer`` and the
indexer read it in parallel. ``compress=False`` writes a plain ``.pfw`` file.

.. code-block:: python

   from dftracer.utils import TraceViewer, TraceWriter

   with TraceWriter("run.pfw.gz", level=6) as w:
       w.write_metadata("HH", {"name": "node-0", "value": "h0"})
       w.write_event("read", "POSIX", ts=1000, dur=5, pid=1, tid=2,
                     args={"fhash": "ab", "sizes": [4096, 8192]})
       w.write_counter("cpu", "sys", ts=2000, args={"user": 12.5})

   TraceViewer("run.pfw.gz").phase("events").collect()

Records
-------

``ph`` is written as its integer: 1 for an event, 2 for a counter, 3 for an
aggregated row and 4 for metadata (with ``ts`` 0). ``args`` is a dict whose
values are ``None``, ``bool``, ``int``, ``float``, ``str``, or a list or dict of
those. A NaN is written as the string ``"NaN"``, and the infinities as
``"Infinity"`` and ``"-Infinity"``, because JSON has no number for them; any
other float is written in the shortest form that reads back to the same bits.

``write_raw`` appends whole lines as they are, for a caller that formats its
own records; the data must end with a newline.

Errors and threads
------------------

The first write error is kept and raised by ``close()``; a gzip writer that
fails removes its output. Leaving a ``with`` block by an exception, or calling
``discard()``, removes the output too. A write after ``close()`` raises
``ValueError``. The GIL is released while buffered lines are written and while
the file is closed. A writer is not thread-safe: a call made while another
thread is inside a write raises ``RuntimeError``.

TraceWriter Class
-----------------

.. autoclass:: dftracer.utils.TraceWriter
   :members:
   :special-members: __enter__, __exit__
