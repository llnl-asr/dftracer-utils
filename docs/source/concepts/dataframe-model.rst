:description: Why dftracer-utils computes in its own native columnar engine rather than Arrow or pandas, and how columns, encodings, and types are shaped.

The DataFrame model
====================

What this explains: why dftracer-utils computes over trace data in its own
native columnar engine rather than handing every operation to Arrow or
pandas, and how that engine is shaped. For the operations themselves, see the
:doc:`../columnar-engine` and :doc:`../guides/data/dataframe` guides.

Columns are the unit of work, not rows
------------------------------------------

A ``dataframe::DataFrame`` (``include/dftracer/utils/dataframe/dataframe.h``)
is a named, ordered set of ``dataframe::Series`` - the RecordBatch/DataChunk
shape used by most columnar engines. A query result, a plugin's output batch,
and the events a fused scan folds all end up as this same type. Choosing
columns over rows is what makes the rest of the engine possible: an analytic
that filters on ``dur > 1000`` touches one contiguous buffer, not one field
inside every row struct scattered across memory, so it is both
cache-friendly and directly vectorizable.

Encodings: not every column pays for its own storage
----------------------------------------------------------

A ``Series`` is not always materialized as a flat buffer. ``types.h`` defines
four encodings a column can carry:

- **FLAT** - values contiguous in memory, the default and the only encoding a
  raw buffer view is valid over.
- **CONSTANT** - one value with a logical length ``N``; a column that is the
  same for every row (a constant added by an operator, a scalar broadcast)
  costs one value, not ``N`` copies of it.
- **DICTIONARY** - ``value[i] = base[codes[i]]``. A repeated string field -
  host name, file path, category - is stored once in the dictionary and
  referenced by a small integer code per row.
- **SELECTION** - ``value[i] = base[sel[i]]``, a view over a base column with
  no data movement. Filtering, sorting, and slicing frequently return a
  SELECTION rather than copying matched rows, so a chain of row operations
  does not repeatedly reallocate the whole column.

An operator picks the cheapest encoding that is still correct for what it
is producing; string predicates, for instance, evaluate directly against a
DICTIONARY without decoding it back to FLAT first. The four encodings are why
the columnar engine can hold a trace-sized amount of string-heavy data
without collapsing it all to raw bytes up front.

Every kernel reads every encoding. A kernel is written against flat
buffers, and every C entry point materializes a view once before the kernel
runs (``dftu_series_materialize``, or the ``DFTU_FLAT_INPUT`` macro that
wraps it), so a SELECTION or a DICTIONARY never reaches raw pointer
arithmetic that assumes FLAT. A view over a view composes into one
selection rather than nesting. The rule costs one copy per kernel over a
view; a chain of row selections stays a view until the first kernel that
needs the values, and that kernel pays once.

Any type, like Parquet
----------------------

A ``Series`` carries a ``TypeId`` and, where the type needs them, its
parameters: a ``Timestamp`` its unit and timezone, a ``Decimal128`` /
``Decimal256`` its precision and scale, a ``FixedSizeBinary`` /
``FixedSizeList`` its width, and ``List`` / ``LargeList`` / ``Struct`` /
``Map`` their child fields. The set mirrors Arrow's, so a column crosses
the Arrow C Data Interface without loss in either direction, and a plugin
source declares a nested column the same way (``dftu_schema_add_field`` /
``add_child_field``).

``Unknown`` is a schema-only marker: a plan whose source cannot know a
column's type without scanning (an ``args.<key>`` column, whose type can
differ between traces) reports ``Unknown`` for that column rather than
guessing, and a real ``Series`` never carries it; a dispatch switch over
column data refuses it at the boundary the way it refuses an out-of-range
value. A timezone is type metadata: ``tz_localize`` and ``tz_convert`` never
move the instants, and only UTC can mark a naive column, since the engine
has no zone database.

SIMD kernels, dispatched once
--------------------------------

Filters, comparisons, reductions, and string predicates are written against
`Highway <https://github.com/google/highway>`_
(``src/dftracer/utils/dataframe/kernels/``), a portable SIMD library. A
kernel is written once and Highway dispatches it at runtime to the widest
instruction set the running CPU actually supports (AVX2/AVX-512 on x86,
NEON/SVE on ARM), instead of the codebase carrying separate hand-written
paths per architecture or falling back to scalar code on hardware that could
do better.

Compute stays in the engine; Arrow is the edge
---------------------------------------------------

The DataFrame is not a wrapper around Arrow's ``RecordBatch`` - it is its own
in-memory representation, purpose-built for the encodings above and for
kernels that operate directly on those encodings. Arrow, and by extension
pandas, NumPy, and polars, only enter at the boundary: a ``DataFrame`` exports
through the Arrow C Data Interface with no cell rebuilt and no copy, which is
what makes it free to hand a query result to a Python caller or a plugin. The
alternative - compute in Arrow's own kernels, or convert to pandas before
doing anything - would mean every filter or aggregation paid a conversion
cost, and would tie the engine's data model to whatever encodings Arrow
happens to support. Keeping computation inside the engine and treating Arrow
purely as an export format is what keeps a multi-stage query (filter, group,
aggregate, sort) from materializing an intermediate at every step.

.. mermaid::

   graph LR
       Series["Series<br/>(FLAT/CONSTANT/DICTIONARY/SELECTION)"]
       Kernels["SIMD kernels<br/>(Highway)"]
       DF["DataFrame"]
       Arrow["Arrow C Data Interface<br/>(zero-copy)"]
       Series --> Kernels --> DF --> Arrow

This is also why the query and plugin surfaces are typed against
``DataFrame``/``Series`` directly (see :doc:`fused-scan`) rather than against
Arrow types: the fold that produces a query result and the kernel that
post-processes it are both native to the same columnar model, with Arrow
appearing only when the result actually leaves the process.

See also
--------

- :doc:`../columnar-engine` and :doc:`../guides/data/dataframe` for the
  operations available on a ``DataFrame``/``Series``.
- :doc:`../cpp_api/dataframe` for the generated API reference.
- :doc:`fused-scan` for how a scan folds events into this engine.
- :doc:`architecture` for where the DataFrame sits in the overall query path.
