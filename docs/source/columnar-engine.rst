:description: How the native columnar engine works: typed columns, flat/constant/dictionary/selection encodings, Highway SIMD kernels, and zero-copy Arrow export.

Columnar Engine
===============

.. seealso::

   :doc:`cpp_api/dataframe` for the full ``dataframe`` API reference.

``dftracer-utils`` computes over trace data with a native columnar engine: a
compact set of typed columns processed by SIMD kernels and exported to Arrow
with no copy. The View read and aggregation path materializes its results into
this engine, so a query returns a columnar batch that is ready for Arrow,
pandas, or further kernels.

Overview
--------

- **Columns, not rows**: a batch is a set of typed columns, each a contiguous
  buffer. Analytics scan one column at a time, which is cache-friendly and
  vectorizes cleanly.
- **Encodings**: a column is ``FLAT`` (values contiguous), ``CONSTANT`` (one
  value with a logical length N), ``DICTIONARY`` (values indexed into a base),
  or ``SELECTION`` (a view over a base with no data movement). Dictionary and
  selection keep a repeated string - host, category, file name - stored once.
- **SIMD kernels**: filters, comparisons, and reductions run through
  `Highway <https://github.com/google/highway>`_, so a kernel is written once
  and dispatched at runtime to the widest instruction set the CPU supports
  (AVX2/AVX-512, NEON, SVE).
- **Zero-copy Arrow**: a batch exports through the Arrow C Data Interface
  without rebuilding a cell, so it crosses into pandas or pyarrow for free. An
  empty result exports as a valid empty struct.
- **Operators**: the relational and reshape primitives - ``join`` / ``asof`` /
  ``interval``, ``window`` / ``gap_fill``, ``unnest`` / ``explode``, ``melt`` /
  ``pivot``, ``concat`` / ``union`` - are native ``DataFrame`` methods that take
  and return ``DataFrame``s (see :doc:`api/dataframe`); the per-column
  primitives (``sort`` / ``argsort`` / ``rank`` / ``rolling`` / ``search_sorted``)
  are native ``Series`` methods (see :doc:`api/series`). Arrow is crossed only at
  the edge.

.. mermaid::

   graph LR
       Scan["Fused scan"] --> Batch["dataframe::DataFrame<br/>(typed columns)"]
       Batch --> Kernels["SIMD kernels<br/>(Highway)"]
       Kernels --> Arrow["Arrow C Data<br/>(zero-copy)"]
       Arrow --> Pandas["pandas / pyarrow"]

How it fits
-----------

The View is the query surface; the columnar batch is what it returns. A typed
read groups, buckets, aggregates, and collects in one pass:

.. code-block:: python

   typed = (
       viewer.group_by("cat", "name")
             .time_bucket(1_000_000)   # 1 second buckets, microsecond ts
             .agg(...)                 # the metrics to compute
             .collect_typed()          # -> columnar result, Arrow-ready
   )
   frame = typed["aggregated"].to_pandas()

Because the result is already columnar and Arrow-native, moving it to pandas or
handing it to another Arrow consumer copies no data. In C++ the same path
returns a ``dataframe::DataFrame`` and the Arrow bridge (``dataframe::to_arrow``) exports it.

Vectorized column authoring (Python)
------------------------------------

``columnar`` evaluates a Polars-style column expression on the **native dataframe
engine** - our own SIMD columnar format, backed by Highway kernels - and returns
a native ``Series``. Every unit is a column: the expression reads
typed columns and produces a column, and a scanned ``View`` batch is a
``DataFrame`` of typed columns. Arrow (or pandas, or polars) appears **only when
you ask for it**, through a zero-copy edge conversion:

.. code-block:: python

   batch = view.group_by("cat").agg("count", "sum:dur").collect().collect()  # -> DataFrame

   avg = (F.sum_dur / F.count).apply(batch)   # -> Series, all in the engine
   avg.to_arrow()                                      # pyarrow.Array, zero-copy
   avg.to_pandas()                                     # pandas Series

   batch.to_arrow()    # pyarrow.Table   (pa.table(batch) works too, via the
   batch.to_pandas()   # pandas DataFrame  Arrow C stream interface)
   batch.to_polars()   # polars DataFrame

``collect_typed()`` / ``join()`` return a ``DataFrame`` directly, and
``collect()`` returns a ``LazyFrame`` whose own ``.collect()`` does, so
post-aggregation derived metrics stay entirely in the engine with no input Arrow.
``apply`` accepts a ``DataFrame`` (or a ``{name: Series}`` mapping) directly,
or a ``pyarrow.Table``, whose columns are imported into the engine at the boundary.

``apply`` compiles the expression to a flat program and evaluates it **fused in
one parallel chunked pass** (``vec_eval``): the column is processed in
cache-sized chunks, each chunk runs the whole expression through a stack machine
over the SIMD kernels on **zero-copy input slices** (so intermediates stay
chunk-sized instead of full columns), and the chunks run across the runtime via
``Runtime::parallel_for``. The result is identical to the per-node path, which
still serves constant or not-yet-fusable expressions.

.. code-block:: python

   from dftracer.utils import F, lit

   ms = F.dur * lit(0.001) + F.ts     # int * float lit + float -> float
   bucket = F.dur.ilog2()             # log2 bucket, a SIMD prim kernel
   rate = F.bytes / F.dur             # SIMD float division

``F.dur`` (or ``col("dur")``) references a column; ``lit(...)`` marks a scalar,
so a bare string is never mistaken for a column. Each operator lowers to a
SIMD kernel (``dftu_series_add`` / ``dftu_series_mul_scalar`` / ``dftu_series_div`` /
``dftu_series_cast`` / ``dftu_series_prim``) operating on typed columns; mixed
int/float promotes to ``double`` via a cast. Arithmetic (``+ - * /``) and scalar
broadcast run over numeric columns; the numeric primitives (``ilog2``,
``bit_width``, ``popcount``, ``clz``, ``ctz``, ``mix64``) are unary methods over
a 64-bit integer column, sharing ``prims.h`` with the JIT/plugin path so they
compute identically. First-cut inputs are single-chunk, non-null columns.

Comparisons against a scalar (``F.dur > lit(1000)``) lower to ``dftu_series_compare``
and produce a boolean column; combine them with ``&`` / ``|`` / ``~``
(``dftu_series_logical``). ``where(source, pred)`` evaluates the predicate on the
engine to a mask, then returns the matching rows: a ``DataFrame`` in stays
fully in the engine (via ``DataFrame.filter``, ``dftu_series_filter``) and returns a
``DataFrame``; a ``pyarrow.Table`` in returns a filtered ``pyarrow.Table``:

.. code-block:: python

   from dftracer.utils import where, F, lit

   slow = where(batch, (F.dur > lit(1000)) & (F.ret >= lit(0)))   # DataFrame

Comparisons are numeric scalar predicates for now (string predicates stay in the
``Field`` / ``Expr`` DSL, which also compiles to the index query string for
scan-time pushdown).

Native frame and column ops
---------------------------

A materialized ``DataFrame`` carries dataframe operations that stay entirely in
the engine - each is a C++ ``dataframe`` primitive (in ``batch_ops`` / the kernels), so
the same ops back the Python API and a future C++ distributed engine. Arrow is
never in the loop; ``to_arrow()`` / ``to_pandas()`` / ``to_polars()`` convert
only at the edge (and ``DataFrame`` is picklable and zero-copy Arrow-importable
via ``__arrow_c_stream__``). ``to_ipc()`` serializes the frame to an Arrow IPC
stream (``bytes`` any Arrow IPC reader opens, no pyarrow needed) for transport.

The full op catalog is below; each operation links to its Python reference (the
C++ ``dataframe`` methods mirror them one-to-one, see :doc:`cpp_api/dataframe`).
``hash_partition`` plus ``concat`` are the distributed shuffle/merge pair: split
with ``hash_partition`` (equal keys co-locate), run ``group_by`` / ``join``
locally, then re-aggregate the ``concat``-ed partials. ``concat`` is UNION ALL:
``how="vertical"`` (default) requires a shared schema, while
``how="diagonal"`` does a schema-union concat - it unions the columns,
null-filling those absent from a part and promoting a mixed-numeric column to
float.

.. list-table:: DataFrame operations
   :header-rows: 1
   :widths: 22 78

   * - Category
     - Operations
   * - Projection
     - :py:meth:`~dftracer.utils.DataFrame.select`, :py:meth:`~dftracer.utils.DataFrame.rename`, :py:meth:`~dftracer.utils.DataFrame.with_column`, :py:meth:`~dftracer.utils.DataFrame.with_row_index`
   * - Row selection
     - :py:meth:`~dftracer.utils.DataFrame.filter`, :py:meth:`~dftracer.utils.DataFrame.query`, :py:meth:`~dftracer.utils.DataFrame.head`, :py:meth:`~dftracer.utils.DataFrame.tail`, :py:meth:`~dftracer.utils.DataFrame.slice`, :py:meth:`~dftracer.utils.DataFrame.reverse`, :py:meth:`~dftracer.utils.DataFrame.take`, :py:meth:`~dftracer.utils.DataFrame.sample`
   * - Ordering
     - :py:meth:`~dftracer.utils.DataFrame.sort`, :py:meth:`~dftracer.utils.DataFrame.sort_by`, :py:meth:`~dftracer.utils.DataFrame.sort_by_multi`, :py:meth:`~dftracer.utils.DataFrame.top_k`, :py:meth:`~dftracer.utils.DataFrame.topk`
   * - Relational
     - :py:meth:`~dftracer.utils.DataFrame.join`, :py:meth:`~dftracer.utils.DataFrame.group_by`, :py:meth:`~dftracer.utils.DataFrame.group_by_dynamic`, :py:meth:`~dftracer.utils.DataFrame.concat`, :py:meth:`~dftracer.utils.DataFrame.union`, :py:meth:`~dftracer.utils.DataFrame.distinct`, :py:meth:`~dftracer.utils.DataFrame.unique`, :py:meth:`~dftracer.utils.DataFrame.drop_duplicates`
   * - Reshape
     - :py:meth:`~dftracer.utils.DataFrame.melt`, :py:meth:`~dftracer.utils.DataFrame.unpivot`, :py:meth:`~dftracer.utils.DataFrame.pivot`, :py:meth:`~dftracer.utils.DataFrame.unnest`, :py:meth:`~dftracer.utils.DataFrame.explode`, :py:meth:`~dftracer.utils.DataFrame.to_dummies`
   * - Time / windows
     - :py:meth:`~dftracer.utils.DataFrame.window`, :py:meth:`~dftracer.utils.DataFrame.gap_fill`, :py:meth:`~dftracer.utils.DataFrame.asof`, :py:meth:`~dftracer.utils.DataFrame.interval`
   * - Nulls / inspection
     - :py:meth:`~dftracer.utils.DataFrame.drop_nulls`, :py:meth:`~dftracer.utils.DataFrame.fill_null`, :py:meth:`~dftracer.utils.DataFrame.is_duplicated`, :py:meth:`~dftracer.utils.DataFrame.is_unique`, :py:meth:`~dftracer.utils.DataFrame.describe`, :py:meth:`~dftracer.utils.DataFrame.null_count`, :py:meth:`~dftracer.utils.DataFrame.keys`, :py:meth:`~dftracer.utils.DataFrame.column_index`
   * - Distributed
     - :py:meth:`~dftracer.utils.DataFrame.hash_partition`
   * - Expressions
     - :py:meth:`~dftracer.utils.DataFrame.apply`
   * - Interop
     - :py:meth:`~dftracer.utils.DataFrame.from_arrow`, :py:meth:`~dftracer.utils.DataFrame.from_pandas`, :py:meth:`~dftracer.utils.DataFrame.from_polars`, :py:meth:`~dftracer.utils.DataFrame.from_parquet`, :py:meth:`~dftracer.utils.DataFrame.from_dict`, :py:meth:`~dftracer.utils.DataFrame.from_numpy`, :py:meth:`~dftracer.utils.DataFrame.to_arrow`, :py:meth:`~dftracer.utils.DataFrame.to_pandas`, :py:meth:`~dftracer.utils.DataFrame.to_polars`, :py:meth:`~dftracer.utils.DataFrame.to_ipc`

.. list-table:: Series operations
   :header-rows: 1
   :widths: 22 78

   * - Category
     - Operations
   * - Reductions
     - :py:meth:`~dftracer.utils.Series.sum`, :py:meth:`~dftracer.utils.Series.min`, :py:meth:`~dftracer.utils.Series.max`, :py:meth:`~dftracer.utils.Series.mean`, :py:meth:`~dftracer.utils.Series.median`, :py:meth:`~dftracer.utils.Series.quantile`, :py:meth:`~dftracer.utils.Series.variance`, :py:meth:`~dftracer.utils.Series.stddev`, :py:meth:`~dftracer.utils.Series.skewness`, :py:meth:`~dftracer.utils.Series.kurtosis`, :py:meth:`~dftracer.utils.Series.count`, :py:meth:`~dftracer.utils.Series.product`, :py:meth:`~dftracer.utils.Series.mode`, :py:meth:`~dftracer.utils.Series.nunique`, :py:meth:`~dftracer.utils.Series.arg_min`, :py:meth:`~dftracer.utils.Series.arg_max`, :py:meth:`~dftracer.utils.Series.all`, :py:meth:`~dftracer.utils.Series.any`, :py:meth:`~dftracer.utils.Series.is_sorted`, :py:meth:`~dftracer.utils.Series.dot`
   * - Elementwise math
     - :py:meth:`~dftracer.utils.Series.abs`, :py:meth:`~dftracer.utils.Series.clip`, :py:meth:`~dftracer.utils.Series.round`, :py:meth:`~dftracer.utils.Series.ceil`, :py:meth:`~dftracer.utils.Series.floor`, :py:meth:`~dftracer.utils.Series.trunc`, :py:meth:`~dftracer.utils.Series.sign`, :py:meth:`~dftracer.utils.Series.negate`, :py:meth:`~dftracer.utils.Series.sqrt`, :py:meth:`~dftracer.utils.Series.exp`, :py:meth:`~dftracer.utils.Series.log`
   * - Cumulative / windowed
     - :py:meth:`~dftracer.utils.Series.cumsum`, :py:meth:`~dftracer.utils.Series.cummax`, :py:meth:`~dftracer.utils.Series.cummin`, :py:meth:`~dftracer.utils.Series.cum_prod`, :py:meth:`~dftracer.utils.Series.cum_count`, :py:meth:`~dftracer.utils.Series.diff`, :py:meth:`~dftracer.utils.Series.pct_change`, :py:meth:`~dftracer.utils.Series.shift`, :py:meth:`~dftracer.utils.Series.interpolate`, :py:meth:`~dftracer.utils.Series.rolling`, :py:meth:`~dftracer.utils.Series.rolling_var`, :py:meth:`~dftracer.utils.Series.rolling_std`, :py:meth:`~dftracer.utils.Series.rolling_median`, :py:meth:`~dftracer.utils.Series.rolling_quantile`, :py:meth:`~dftracer.utils.Series.ewm_mean`, :py:meth:`~dftracer.utils.Series.ewm_std`
   * - Ordering / selection
     - :py:meth:`~dftracer.utils.Series.sort`, :py:meth:`~dftracer.utils.Series.argsort`, :py:meth:`~dftracer.utils.Series.rank`, :py:meth:`~dftracer.utils.Series.top_k`, :py:meth:`~dftracer.utils.Series.bottom_k`, :py:meth:`~dftracer.utils.Series.head`, :py:meth:`~dftracer.utils.Series.tail`, :py:meth:`~dftracer.utils.Series.reverse`, :py:meth:`~dftracer.utils.Series.slice`, :py:meth:`~dftracer.utils.Series.take`, :py:meth:`~dftracer.utils.Series.filter`, :py:meth:`~dftracer.utils.Series.search_sorted`
   * - Distinct / binning
     - :py:meth:`~dftracer.utils.Series.unique`, :py:meth:`~dftracer.utils.Series.value_counts`, :py:meth:`~dftracer.utils.Series.cut`, :py:meth:`~dftracer.utils.Series.qcut`, :py:meth:`~dftracer.utils.Series.sample`
   * - Masks / predicates
     - :py:meth:`~dftracer.utils.Series.gt`, :py:meth:`~dftracer.utils.Series.ge`, :py:meth:`~dftracer.utils.Series.lt`, :py:meth:`~dftracer.utils.Series.le`, :py:meth:`~dftracer.utils.Series.eq`, :py:meth:`~dftracer.utils.Series.ne`, :py:meth:`~dftracer.utils.Series.compare`, :py:meth:`~dftracer.utils.Series.is_between`, :py:meth:`~dftracer.utils.Series.is_in`, :py:meth:`~dftracer.utils.Series.is_null`, :py:meth:`~dftracer.utils.Series.is_nan`, :py:meth:`~dftracer.utils.Series.is_finite`, :py:meth:`~dftracer.utils.Series.is_infinite`, :py:meth:`~dftracer.utils.Series.is_unique`, :py:meth:`~dftracer.utils.Series.is_duplicated`, :py:meth:`~dftracer.utils.Series.logical`, :py:meth:`~dftracer.utils.Series.logical_not` (``&``, ``|`` and ``~`` between masks)
   * - Arithmetic
     - :py:meth:`~dftracer.utils.Series.add`, :py:meth:`~dftracer.utils.Series.sub`, :py:meth:`~dftracer.utils.Series.mul`, :py:meth:`~dftracer.utils.Series.div`, :py:meth:`~dftracer.utils.Series.add_scalar`, :py:meth:`~dftracer.utils.Series.sub_scalar`, :py:meth:`~dftracer.utils.Series.mul_scalar`, :py:meth:`~dftracer.utils.Series.div_scalar`
   * - Strings
     - :py:meth:`~dftracer.utils.Series.str_eq`, :py:meth:`~dftracer.utils.Series.str_contains`, :py:meth:`~dftracer.utils.Series.str_starts_with`, :py:meth:`~dftracer.utils.Series.str_ends_with`, :py:meth:`~dftracer.utils.Series.str_matches`, :py:meth:`~dftracer.utils.Series.str_like`, :py:meth:`~dftracer.utils.Series.str_len_bytes`, :py:meth:`~dftracer.utils.Series.str_len_chars`, :py:meth:`~dftracer.utils.Series.str_find`, :py:meth:`~dftracer.utils.Series.str_slice`, :py:meth:`~dftracer.utils.Series.str_split`, :py:meth:`~dftracer.utils.Series.str_replace`, :py:meth:`~dftracer.utils.Series.str_replace_all`, :py:meth:`~dftracer.utils.Series.str_strip`, :py:meth:`~dftracer.utils.Series.str_lstrip`, :py:meth:`~dftracer.utils.Series.str_rstrip`, :py:meth:`~dftracer.utils.Series.str_pad_start`, :py:meth:`~dftracer.utils.Series.str_pad_end`, :py:meth:`~dftracer.utils.Series.str_zfill`, :py:meth:`~dftracer.utils.Series.to_lowercase`, :py:meth:`~dftracer.utils.Series.to_uppercase`
   * - Nulls / structure
     - :py:meth:`~dftracer.utils.Series.drop_nulls`, :py:meth:`~dftracer.utils.Series.fillna`, :py:meth:`~dftracer.utils.Series.cast`, :py:meth:`~dftracer.utils.Series.dictionary_encode`, :py:meth:`~dftracer.utils.Series.materialize`, :py:meth:`~dftracer.utils.Series.share`, :py:meth:`~dftracer.utils.Series.child`, :py:meth:`~dftracer.utils.Series.num_children`
   * - Interop
     - :py:meth:`~dftracer.utils.Series.from_arrow`, :py:meth:`~dftracer.utils.Series.from_pandas`, :py:meth:`~dftracer.utils.Series.from_polars`, :py:meth:`~dftracer.utils.Series.from_numpy`, :py:meth:`~dftracer.utils.Series.from_list`, :py:meth:`~dftracer.utils.Series.to_arrow`, :py:meth:`~dftracer.utils.Series.to_pandas`, :py:meth:`~dftracer.utils.Series.to_numpy`, :py:meth:`~dftracer.utils.Series.to_polars`

``group_by`` (and ``group_by_dynamic`` / ``pivot``) take per-group aggregates
named ``sum`` / ``min`` / ``max`` / ``count`` / ``mean`` / ``var`` / ``std`` /
``skew`` / ``kurt`` plus ``first`` / ``last`` (the group's first / last non-null
value in row order). ``first`` / ``last`` merge order-independently, so the
parallel and distributed path is exact.

The moment statistics (``variance``/``stddev``/``skewness``/``kurtosis``) reduce
their values with a Highway SIMD pass; ``abs``/``clip``/``round`` are Highway
kernels; ``rank`` reuses the SIMD ``argsort``; ``cumsum``/``cummax``/``cummin``
and ``rolling`` are sequential scans (a running accumulator / monotonic deque).

The relational and ordering ops have **parity on the View**: ``TraceViewer``
exposes ``group_by``/``agg``/``join``/``limit``, ``sort_by(name,
descending)`` and ``topk(name, k, largest)``. The View records them in its plan and
applies the ordering to the aggregated result with the same SIMD kernels, so
``view.group_by(...).agg(...).topk("count", 10)`` and
``view.group_by(...).agg(...).collect().collect().topk("count", 10)`` return
the same rows.

``sort_by`` / ``topk`` use a SIMD sort (Highway ``VQSort`` / ``VQPartialSort``
over order-preserving-key + index packing) for fixed-width numeric columns, and
a stable scalar sort for strings/nested/null columns. ``quantile`` sorts the
values with SIMD ``VQSort``; ``abs`` / ``clip`` / ``round`` are Highway kernels.

.. code-block:: python

   batch = view.group_by("cat").agg("count", "sum:dur").collect().collect()
   worst = batch.sort_by("sum_dur", descending=True).head(10)   # all in the engine
   p99 = batch["sum_dur"].quantile(0.99)
   merged = part_a.concat(part_b, part_c)                        # distributed merge
