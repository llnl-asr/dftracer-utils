:description: Get and operate on one typed Series column with SIMD kernels: reducers, element-wise math, scans, and edge conversions to NumPy or Arrow.

Work with Series
==================

A ``Series`` is one typed column: SIMD (Highway) kernels over a
flat/dictionary/selection-encoded buffer, NumPy-like in Python. Every op below
returns a new ``Series`` (nothing mutates in place) and stays inside the native
engine; ``to_arrow`` / ``to_pandas`` / ``to_numpy`` / ``to_polars`` are
conversions at the edge. For the op catalog in one place see
:doc:`../core/columnar-ops`; for how a ``DataFrame`` holds these columns see
:doc:`dataframe`.

Get a Series
-------------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         dur = df["dur"]        # __getitem__
         dur = df.column("dur")

   .. tab-item:: C++

      .. code-block:: cpp

         Series dur = df.column("dur");  // shares the frame's buffer, zero-copy

Or build one from data you already have.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import Series

         s = Series.from_numpy(arr)
         s = Series.from_pandas(pandas_series)
         s = Series.from_arrow(pyarrow_array)
         s = Series.from_polars(polars_series)
         s = Series.from_list([1, 2, 3])

   .. tab-item:: C++

      ``Series::flat`` (and the ``flat_i64`` / ``flat_f64`` shorthands) copy a
      fixed-width buffer; ``strings`` / ``structs`` / ``list`` build the nested
      encodings. To wrap external memory with **no copy**, use
      ``Series::from_borrowed``: the column points straight at your buffer, which
      you keep alive until the release callback fires - it runs exactly once when
      the column's last reference is dropped (possibly off the creating thread).

      .. code-block:: cpp

         std::vector<std::int64_t> v = {1, 2, 3};

         // Copying factories.
         Series a = Series::flat_i64(v.data(), v.size());

         // Zero-copy borrow with a release callback (data must outlive it).
         Series b = Series::from_borrowed(
             TypeId::Int64, v.data(), v.size(), /*validity=*/nullptr,
             [](void* ctx) { /* free ctx */ }, /*release_ctx=*/nullptr);

         // Zero-copy borrow that takes ownership of a move-in owner; the owner
         // (here the vector backing the buffer) is destroyed with the column.
         const std::int64_t* data = v.data();
         Series c = Series::from_borrowed(TypeId::Int64, data, v.size(),
                                          std::move(v));

      Borrowing rejects variable-width types (String/Binary), returning an
      invalid ``Series`` (and running the release once). Pass a null ``release``
      to borrow static or otherwise-owned memory.

Arithmetic and the numpy-like protocol
----------------------------------------

.. tab-set::

   .. tab-item:: Python

      A ``Series`` behaves like a NumPy array: elementwise ``+ - * /`` against
      another ``Series`` or a scalar, ``s[i]`` / ``s[a:b]`` indexing, and
      ``np.asarray(s)``.

      .. code-block:: python

         total = a + b
         scaled = dur * 2
         first = dur[0]           # a Python scalar
         window = dur[10:20]      # a Series (step-1 slices only)
         arr = np.asarray(dur)    # zero-copy for flat, non-null, fixed-width numeric

      Comparisons return a boolean mask ``Series``: ``a < b``, ``dur > 100``, or
      the named form ``dur.gt(100)`` / ``.ge()`` / ``.lt()`` / ``.le()`` /
      ``.eq()`` / ``.ne()``. ``==``/``!=`` are not overloaded (so a ``Series``
      still hashes and works with ``in``); use ``.eq()`` / ``.ne()``.
      Reflected scalar division (``2 / s``) is not provided - use ``2 /
      s.to_numpy()``.

   .. tab-item:: C++

      .. code-block:: cpp

         Series total = a + b;              // or a.add(b)
         Series scaled = dur * 2;           // operator sugar; mul_scalar is Python-only
         Series mask = dur.gt(100);         // template compare: gt/ge/lt/le/eq/ne
         Series mask2 = dur > 100;          // operator sugar for gt
         Series between = dur.is_between(10, 100);
         Series both = mask & mask2;        // logical_and; also | (or), ~ (not)

      Binary ``Series`` ops need FLAT numeric columns of equal length. Scalar
      arithmetic converts the scalar to the column's element type.

Reducers
---------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         total = dur.sum()
         avg = dur.mean()
         p99 = dur.quantile(0.99)
         spread = dur.stddev()      # sample by default; stddev(False) for population (positional only)
         var = dur.variance()
         n = dur.count()            # non-null row count
         hi = dur.max()

   .. tab-item:: C++

      .. code-block:: cpp

         // Reducers that can carry any domain return a typed Scalar; read it
         // with .i64() / .f64() / .as<T>() (no raw union access).
         std::int64_t total = dur.sum().i64();
         double avg = dur.mean();
         double p99 = dur.quantile(0.99);
         double spread = dur.stddev();      // stddev(bool sample = true)
         double var = dur.variance();
         std::int64_t n = dur.count();
         std::int64_t hi = dur.max().i64();

Also available on both sides: ``min``, ``product``, ``median``, ``skewness``,
``kurtosis``, ``mode``, ``all``, ``any``, ``arg_min``, ``arg_max``, ``nunique``.

Element-wise kernels
----------------------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         positive = dur.abs()
         bounded = dur.clip(0, 1000)
         rounded = dur.round()      # also ceil() floor() trunc() sign()
         filled = dur.fillna(0)
         root = dur.sqrt()          # also exp() log()
         as_f64 = dur.cast(10)      # TypeId ordinal: Float64 = 10
         encoded = names.dictionary_encode()
         interp = dur.interpolate() # linear fill of interior nulls, edges stay null

   .. tab-item:: C++

      .. code-block:: cpp

         Series positive = dur.abs();
         Series bounded = dur.clip(0, 1000);  // natural values, no dftu_scalar
         Series rounded = dur.round();
         Series filled = dur.fillna(0);       // fillna(1.5) for a float column
         Series root = dur.sqrt();
         Series as_f64 = dur.cast(TypeId::Float64);
         Series encoded = names.dictionary_encode();
         Series interp = dur.interpolate();

``cast`` converts to another element type (``TypeId``: ``Bool``, ``Int8``
.. ``Int64``, ``Uint8`` .. ``Uint64``, ``Float32``, ``Float64``, ``String``,
...); the Python binding takes the same ordinal as an ``int`` (readable back
via ``series.type``). ``dictionary_encode`` rewrites a column to
dictionary encoding (repeated values stored once); most other kernels accept
it transparently.

Scans and windows
-------------------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         running = dur.cumsum()      # also cummax() cummin() cum_prod()
         delta = dur.diff()
         pct = dur.pct_change()
         lagged = dur.shift(1)       # negative n leads instead
         smoothed = dur.rolling(window=5, op="mean")   # sum|mean|min|max
         med = dur.rolling_median(5)
         ranked = dur.rank(method="average", descending=False)

   .. tab-item:: C++

      .. code-block:: cpp

         Series running = dur.cumsum();
         Series delta = dur.diff();
         Series pct = dur.pct_change();
         Series lagged = dur.shift(1);
         Series smoothed = dur.rolling(RollingOp::Mean, 5);  // Sum/Min/Max too
         Series var = dur.rolling_var(5);           // rolling_std / rolling_median /
                                                     // rolling_quantile(w, q) also exist
         Series ranked = dur.rank(RankMethod::Average);  // descending flag too
         Series ewm = dur.ewm_mean(0.3);            // ewm_std(alpha) too

``rolling(window, op)`` (Python) / ``rolling(op, window)`` (C++), and
``rolling_var`` / ``rolling_std`` / ``rolling_median`` / ``rolling_quantile``
leave the first ``window - 1`` rows null. ``rank`` (``RankMethod`` tie-break)
and the plain sum/mean/min/max ``rolling`` window are available from both
Python and C++.

Binning and search
--------------------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         breaks = Series.from_list([10, 100, 1000])
         bins = dur.cut(breaks)        # Int32 bin index (count of breaks <= x)
         # pandas.cut(labels=False): right-closed interior bins numbered from 0
         pbins = dur.cut(breaks, right=True, outer=False)
         quartile = dur.qcut(4)        # Int32 bins 0..3, by this column's quantiles
         idx = dur.search_sorted(Series.from_list([50, 500]))  # Int64 insertion index

   .. tab-item:: C++

      .. code-block:: cpp

         Series bins = dur.cut(breaks);
         // right-closed interior bins numbered from 0
         Series pbins = dur.cut(breaks, DFTU_CUT_RIGHT | DFTU_CUT_INNER);
         Series quartile = dur.qcut(4);
         Series idx = dur.search_sorted(values);

``cut`` and ``search_sorted`` both assume the input (``breaks`` for ``cut``,
``dur`` itself for ``search_sorted``) is ascending; a null row in ``cut``
yields a null bin. By default the intervals are ``[a, b)`` and a value below
the first break gets bin 0. ``right=True`` (``DFTU_CUT_RIGHT``) closes each
interval on the right, ``(a, b]``. ``outer=False`` (``DFTU_CUT_INNER``) gives a
null for a value outside the interior intervals and numbers them from 0; with
both, ``cut`` equals ``pandas.cut(labels=False)``.

Select and test rows
-----------------------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         asc = dur.sort()
         order = dur.argsort()         # Int64 row order; df.take(order) reorders a frame
         worst = dur.top_k(10)         # also bottom_k(k)
         first10 = dur.head(10)
         mask = dur.is_in(other)
         nonnull = dur.drop_nulls()
         sorted_ok = dur.is_sorted()
         seen = dur.value_counts()     # a two-column DataFrame
         dup_mask = dur.is_duplicated()  # also is_unique(); per-value, not per-row
                                          # (see DataFrame.is_duplicated for whole-row)

   .. tab-item:: C++

      .. code-block:: cpp

         Series asc = dur.sort();
         Series order = dur.argsort();  // Int64 row order for take()
         Series worst = dur.top_k(10);
         Series first10 = dur.head(10);
         Series mask = dur.is_in(other);
         Series nonnull = dur.drop_nulls();
         bool sorted_ok = dur.is_sorted();
         Series dup_mask = dur.is_duplicated();  // also is_unique()

Note the Python/C++ asymmetry: the Python ``DataFrame`` op for the same idea is
spelled ``topk``, but ``Series`` uses ``top_k`` in both languages - check each
surface's own method table before relying on the name.

Strings
--------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         posix = names.str_eq("POSIX")
         has = names.str_contains("read")
         starts = names.str_starts_with("lib")
         glob = names.str_like("lib%.so")
         re = names.str_matches(r"^lib.*\.so$")
         lower = names.to_lowercase()       # also to_uppercase()
         trimmed = names.str_strip()
         short = names.str_slice(0, 8)
         parts = names.str_split(",")       # -> List<String> column

   .. tab-item:: C++

      .. code-block:: cpp

         Series posix = names.str_eq("POSIX");
         Series has = names.str_contains("read");
         Series starts = names.str_starts_with("lib");
         Series glob = names.str_like("lib%.so");
         Series re = names.str_matches(R"(^lib.*\.so$)");
         Series lower = names.to_lowercase();
         Series trimmed = names.str_strip();
         Series short_s = names.str_slice(0, 8);
         Series parts = names.str_split(",");

Also on both sides: ``str_ends_with``, ``str_find``, ``str_len_bytes``,
``str_len_chars``, ``str_replace`` / ``str_replace_all``, ``str_regex_replace(pattern, to)``, ``str_lstrip`` /
``str_rstrip``, ``str_pad_start`` / ``str_pad_end``, ``str_zfill``.
``str_like`` follows SQL LIKE rules (``%`` any run, newlines included, ``_``
one UTF-8 character, ``\`` escapes a literal ``%``/``_``/``\``);
``str_matches`` takes a duql regex matched against the whole string, and
``str_search`` the same regex anywhere in it. ``str_regex_replace`` replaces
every match of a regex; ``to`` takes ``$n``, ``${n}``, ``${name}`` and ``$$``,
and an invalid pattern or replacement raises ``ValueError`` in Python. The
pandas ``.str.replace`` stays literal. In C it is ``dftu_series_str_regex_replace``,
and ``DFTU_STR_FN_REGEX_REPLACE`` for ``dftu_expr_str_fn``. See :doc:`/reference/duql` for
the regex dialect and the match limit.

``dt_format(fmt, unit)`` formats a time column as UTC text; ``unit`` is the
unit of the stored integers, coded as in ``dt_part`` (0 seconds, 1
milliseconds, 2 microseconds, 3 nanoseconds); a timestamp column uses its own
unit. In Python
it is the pandas ``.dt.strftime(fmt)``. The directives are ``%Y %y %m %d %H
%I %M %S %f %j %a %A %b %B %p %F %T %s %z %Z`` and ``%%``; ``%f`` is six digit
microseconds, names are English, ``%z`` is ``+0000`` and ``%Z`` is ``UTC``. An
unknown directive or a trailing ``%`` raises ``ValueError``; there is no other
time zone. In C it is ``dftu_series_dt_format``, and ``dftu_expr_date_part``
and ``dftu_expr_format_time`` build the expressions that duql ``date_part``
and ``format_time`` use.

Derived columns via F expressions
------------------------------------

Build a column expression from ``F.<name>`` (or ``col("name")``) and evaluate
it with ``.apply()`` - no wrapper class needed, and the result is a plain
``Series``:

.. code-block:: python

   from dftracer.utils import F, col, lit, eval_many

   avg = (F.sum_dur / F.count).apply(df)         # Series
   ms = (F.dur * lit(0.001)).apply(df)            # int * float -> float
   bucket = F.dur.ilog2().apply(df)               # numeric prim (ilog2, bit_width,
                                                   # popcount, clz, ctz, mix64)

Expressions combine with ``+ - * /``, comparisons (``>`` ``>=`` ``<`` ``<=``
``==`` ``!=``, building a boolean-column predicate), and ``& | ~`` to combine
predicates. This is the same ``F`` used to build row-filter predicates for
``TraceViewer.filter()`` (see :doc:`../core/duql`); a numeric comparison
evaluates here to a mask, while string-match and membership predicates are
filter-only. Evaluate several expressions in one pass with shared
sub-expressions computed once:

.. code-block:: python

   a, b = eval_many([F.sum_dur / F.count, (F.sum_dur / F.count) * lit(2)], df)

``source`` for ``.apply()`` / ``eval_many`` is a ``DataFrame``, a
``{name: Series}`` mapping, or a ``pyarrow.Table`` (imported at the boundary).

The same expression layer is in C++ via ``dftracer::utils::dataframe::field``
(``<dftracer/utils/dataframe/field.h>``) - the one ``F`` that also builds
``View::filter`` predicates. ``F("dur")`` is a field leaf; ``+ - * /``, the
numeric prims (``.ilog2()`` / ``.bit_width()`` / ``.popcount()`` / ``.clz()`` /
``.ctz()`` / ``.mix64()``), and comparisons build the expression, evaluated on a
``DataFrame`` with ``.apply(df)``:

.. code-block:: cpp

   #include <dftracer/utils/dataframe/field.h>
   using namespace dftracer::utils::dataframe::field;

   Series avg = (F("sum_dur") / F("count")).apply(df);
   Series bucket = F("dur").ilog2().apply(df);
   Series mask = (F("dur") > 1000).apply(df);   // a comparison is a mask

For the lower-level index-based API (multiple expressions in one pass with
shared subexpressions computed once), use ``col(i)`` with ``eval`` / ``eval_many``
(``<dftracer/utils/dataframe/expr.h>``), or build directly with ``Series``
arithmetic and kernels (e.g. the prims via ``Series::prim``).

Convert out (only at the edge)
--------------------------------

.. code-block:: python

   dur.to_arrow()     # pyarrow.Array (zero-copy)
   dur.to_pandas()     # pandas Series
   dur.to_numpy()      # NumPy array (zero-copy for flat, non-null, fixed-width numeric)
   dur.to_polars()     # polars Series

See also
---------

- :doc:`dataframe` for the frame-level how-to (project, select rows, reshape,
  summarize, relational).
- :doc:`../core/columnar-ops` for the full op catalog in one place.
- :doc:`../../cpp_api/dataframe` for the generated C++ member reference and the
  C ABI.
- :doc:`../../columnar-engine` for how the engine is built (encodings, SIMD
  kernels, the Arrow bridge).
