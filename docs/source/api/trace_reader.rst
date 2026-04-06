TraceReader Module
==================

The ``TraceReader`` is the recommended way to read trace files. It auto-selects
sequential or indexed reading based on whether a root-local ``.dftindex``
RocksDB store exists.

TraceReader Class
-----------------

.. autoclass:: dftracer.utils.TraceReader(file_path: str, index_dir: str = '', checkpoint_size: int = 33554432, auto_build_index: bool = False, index_threshold: int = 8388608, runtime: Runtime | None = None)
   :members:
   :undoc-members:
   :show-inheritance:
   :special-members: __enter__, __exit__

Streaming Iterators
-------------------

``iter_lines()``, ``iter_raw()``, and ``iter_lines_json()`` return Python
iterators backed by a bounded producer-consumer queue. The C++ coroutine runs
on the Runtime's thread pool and pushes items; Python's ``__next__`` pops.

.. code-block:: python

   reader = TraceReader("trace.pfw.gz")

   # Stream decoded lines
   for line in reader.iter_lines():
       process(line)  # str

   # Stream raw byte chunks (one line per chunk)
   for chunk in reader.iter_raw(multi_line=False):
       process(chunk)  # bytes

   # Stream parsed JSON objects
   for obj in reader.iter_lines_json():
       print(obj["name"], obj["dur"])  # lazy JSON access

   # Materialize to list
   lines = reader.read_lines()        # list[str]
   chunks = reader.read_raw()         # list[bytes]
   objects = reader.read_lines_json()  # list[JSON]

Arrow Output
------------

``iter_arrow()`` and ``read_arrow()`` parse JSON events into columnar Arrow
record batches using dynamic schema discovery. Each JSON key becomes a column;
types are inferred from values (int64, uint64, double, string, bool). Nested
objects/arrays are serialized as JSON strings.

The returned objects implement the Arrow PyCapsule protocol
(``__arrow_c_array__``) for zero-copy interchange with pyarrow, polars, and
DuckDB.

.. code-block:: python

   reader = TraceReader("trace.pfw.gz")

   # Stream Arrow batches (memory-efficient)
   for batch in reader.iter_arrow(batch_size=10000):
       pa_batch = pyarrow.record_batch(batch)
       df = pa_batch.to_pandas()

   # Materialize all events as ArrowTable
   table = reader.read_arrow()
   df = table.to_pandas()    # requires pyarrow
   df = table.to_polars()    # requires polars

   # With range parameters
   table = reader.read_arrow(start_line=100, end_line=200)

File Metadata
-------------

``get_max_bytes()`` and ``get_num_lines()`` return file metadata without
reading the full file (when a ``.dftindex`` RocksDB index store exists):

.. code-block:: python

   reader = TraceReader("trace.pfw.gz")

   max_bytes = reader.get_max_bytes()  # 0 if no index for compressed files
   num_lines = reader.get_num_lines()  # 0 if no index

   # Partition for parallel processing
   if max_bytes > 0:
       chunk_size = max_bytes // num_workers
       for i in range(num_workers):
           start = i * chunk_size
           end = min((i + 1) * chunk_size, max_bytes)
           process(reader.read_lines_json(start_byte=start, end_byte=end))

Query Filtering
---------------

All line-based reading methods (``read_lines``, ``iter_lines``,
``iter_lines_json``, ``read_lines_json``, ``iter_arrow``, ``read_arrow``)
accept an optional ``query`` parameter for event filtering:

.. code-block:: python

   reader = TraceReader("trace.pfw.gz")

   # Filter by category
   for line in reader.iter_lines(query='cat == "POSIX"'):
       process(line)

   # Combine filters with AND/OR
   lines = reader.read_lines(query='cat == "POSIX" and dur > 1000')

   # Use IN for multiple values
   lines = reader.read_lines(query='name in ["read", "write", "open"]')

   # NOT queries
   lines = reader.read_lines(query='not cat == "MPI"')

   # Nested field paths
   lines = reader.read_lines(query='args.level == "DEBUG"')

   # Combine with range parameters
   lines = reader.read_lines(start_line=1, end_line=1000,
                              query='cat == "POSIX"')

   # Arrow output with query
   table = reader.read_arrow(query='cat == "POSIX" and dur > 100')
   df = table.to_pandas()

When an index exists, the query is used for chunk pruning (skipping
entire chunks that cannot match) before per-event filtering. This
provides significant speedups on large indexed traces.

Query DSL Syntax
~~~~~~~~~~~~~~~~

The query language supports:

- **Comparison**: ``==``, ``!=``, ``>``, ``<``, ``>=``, ``<=``
- **Logical**: ``and``, ``or``, ``not`` (case-insensitive)
- **Membership**: ``in [...]``, ``not in [...]``
- **Grouping**: parentheses ``(...)``
- **Field paths**: dotted notation for nested JSON (``args.level``)
- **Values**: strings (``"POSIX"``), integers (``1000``), floats (``3.14``), booleans (``true``/``false``)

Keywords (``and``, ``or``, ``not``, ``in``, ``true``, ``false``) are
case-insensitive: ``AND``, ``and``, ``And`` all work.
String values are case-sensitive: ``cat == "POSIX"`` does not match ``"posix"``.

Python Field DSL
~~~~~~~~~~~~~~~~

For programmatic query construction, use the ``Field`` class:

.. code-block:: python

   from dftracer.utils.query import Field

   cat = Field("cat")
   dur = Field("dur")
   name = Field("name")

   # Operators: ==, !=, >, <, >=, <=
   q = cat == "POSIX"

   # AND (&), OR (|), NOT (~)
   q = (cat == "POSIX") & (dur > 1000)
   q = (cat == "POSIX") | (cat == "STDIO")
   q = ~(cat == "MPI")

   # IN / NOT IN
   q = name.is_in(["read", "write", "open"])
   q = cat.not_in(["MPI"])

   # Pass to TraceReader
   reader = TraceReader("trace.pfw.gz")
   lines = reader.read_lines(query=str(q))

ReadConfig Parameters
---------------------

All reading methods accept these keyword arguments:

- ``start_line`` / ``end_line`` -- line range (1-indexed; 0 = no limit)
- ``start_byte`` / ``end_byte`` -- byte range (0 = no limit)
- ``buffer_size`` -- internal buffer size in bytes (default 4 MB)
- ``query`` -- query DSL string for event filtering (default None)

``iter_raw`` and ``read_raw`` additionally accept:

- ``line_aligned`` -- if True, chunks are aligned to line boundaries (default True)
- ``multi_line`` -- if True, chunks may contain multiple lines (default True)

``iter_arrow`` additionally accepts:

- ``batch_size`` -- maximum rows per Arrow batch (default 10000)

Out-of-range values are clamped to the actual file bounds (no errors thrown).
