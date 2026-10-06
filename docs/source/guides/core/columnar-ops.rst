:description: Operate on a native DataFrame in the SIMD engine: project, filter, sort, derive, and reshape columns before converting at the edge.

Compute and reshape columns
===========================

A query returns a native ``DataFrame`` (a set of typed ``Series``). Everything
below stays in the SIMD engine - Arrow, pandas, NumPy, and polars appear only at
the edge, zero-copy, when you ask. See :doc:`../../columnar-engine` for the
engine internals.

Get a DataFrame
---------------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         df = TraceViewer("traces/").group_by("cat").agg("count", "sum:dur").collect()
         # the chain is lazy; collect() runs it (-> DataFrame)
         # or ingest one: DataFrame.from_pandas(pdf) / from_arrow(tbl) / from_numpy(a, columns=...)

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>
         using namespace dftracer::utils::trace::views;

         auto df = View::from_file("trace.pfw.gz")
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "count"},
                             {AggOp::Sum, "dur", "sum_dur"}})
                       .collect()   // -> coro::CoroTask<DataFrame>
                       .get();

Derived columns
---------------

Build a column expression from ``F.<name>`` and evaluate it with ``.apply`` - no
wrapper needed. Mix columns and scalars; ``+ - * /``, comparisons, and the
numeric prims (``ilog2``, ``popcount``, ...) all lower to SIMD kernels. This
``F`` / ``.apply`` expression layer also exists in C++ as
``dftracer::utils::dataframe::field::F`` (``F("dur").apply(df)``); see
:doc:`duql` and :doc:`../data/dataframe`.

.. code-block:: python

   from dftracer.utils import F, lit

   avg = (F.sum_dur / F.count).apply(df)          # Series
   avg = df.apply(F.sum_dur / F.count)            # frame-first spelling, same result
   ms = (F.dur * lit(0.001)).apply(df)            # int * float -> float
   bucket = F.dur.ilog2().apply(df)               # log2 histogram bucket

Both spellings evaluate the same way; ``df.apply(expr)`` is
:meth:`DataFrame.apply <dftracer.utils.DataFrame.apply>`, useful when chaining
off a frame variable reads better than starting from the expression.

To evaluate several expressions in one CSE'd pass (a shared subexpression is
computed once), use :func:`~dftracer.utils.eval_many`:

.. code-block:: python

   from dftracer.utils import eval_many
   a, b = eval_many([F.sum_dur / F.count, (F.sum_dur / F.count) * lit(2)], df)

DataFrame operations
--------------------

All of these stay in the engine and back both the Python API and the C++ engine;
the C++ ``DataFrame`` (``dftracer/utils/dataframe/dataframe.h``) carries the same
members under the same names. For side-by-side C++ and Python tabs of each op see
:doc:`../data/dataframe`, :doc:`../data/series`, :doc:`../data/joins`,
:doc:`../data/reshape`, and :doc:`../data/time-windows`.

- **project**: ``select("cat", "count")``, ``rename({"count": "n"})``, ``with_column("avg", series)``
- **rows**: ``filter(mask)``, ``filter_mask(series)``, ``head(n)`` / ``tail(n)``, ``slice(off, len)``, ``reverse()``, ``take(indices)``, ``sample(n, seed)``, ``sort_by("count", descending=True)``, ``sort_by_multi(...)``, ``topk("count", 10)``, ``with_row_index("i")``
- **reshape**: ``unique()`` / ``drop_duplicates()``, ``is_duplicated()`` / ``is_unique()``, ``drop_nulls()``, ``fill_null(v)``, ``unpivot(...)`` / ``melt(...)``, ``explode("col")``, ``unnest("col")``, ``to_dummies("cat")``, ``pivot(index, on, values)``
- **summarize**: ``group_by("cat", aggs)`` (every ``DFTU_AGG_*`` op, ``prod`` and exact ``median`` / ``quantile`` included), ``describe()``, ``null_count()``, ``value_counts`` (on a Series); the group-wise transforms ``cumsum`` / ``cumprod`` / ``shift`` / ``rank`` / ``ffill`` / ``bfill`` / ``rolling`` / ``ewm`` / ``take`` / ``sample`` / ``resample`` (:doc:`../data/pandas-polars`)
- **relational**: ``join(other, how="inner", on=...)``, ``asof`` / ``join_asof``, ``concat(other)`` / ``vstack``, ``hstack``
- **windowed**: ``group_by_dynamic(time_col, every, period, aggs)``, ``window(...)``, ``gap_fill``, ``interval``
- **string methods in an expression**: the methods of ``Series.str`` have an expression form of the same name and meaning, for ``with_columns``, ``filter`` and lazy plans: ``col("s").capitalize()``, ``.title()``, ``.swapcase()``, ``.casefold()``; the predicates ``.isalnum() .isalpha() .isdecimal() .isdigit() .islower() .isnumeric() .isspace() .istitle() .isupper()``; ``.zfill(w)``, ``.pad(w, side, fill)``, ``.pad_start(w, fill)``, ``.pad_end(w, fill)``, ``.ljust(w, fill)``, ``.rjust(w, fill)``, ``.center(w, fill)``, ``.removeprefix(p)``, ``.removesuffix(p)``, ``.repeat(n)``, ``.slice_replace(start, stop, repl)``; ``.split(sep)``, ``.rsplit(sep)``, ``.partition(sep)``, ``.rpartition(sep)`` and ``.findall(re)`` (a ``list<string>`` column; take a piece with ``.get(i)``, join with ``.join(sep)``), ``.extract(re, group)``, ``.regex_replace(re, to)`` (``$n``, ``${n}``, ``${name}`` and ``$$`` in ``to``; ``col("s").regex_replace(re, to)`` is the same), ``.match(re)``, ``.rfind(sub)``, ``.index(sub)``, ``.rindex(sub)``, ``.get(i)`` and ``.cat(other, sep)``. The values, nulls and column types are the eager ones (ASCII: a byte outside ASCII is left as it is by the case forms and fails the character classes). Left out: ``get_dummies`` makes a column per token, so it raises ``NotImplementedError`` and names ``Series.str.get_dummies``; ``cat()`` with no other column joins the whole column into one string, an aggregate, so it raises and names ``Series.str.cat``; ``count(pat)`` keeps the name of the aggregate ``count()`` and is not an expression (use ``Series.str.count``). ``index`` and ``rindex`` fail the run with ``ValueError`` when a row lacks the substring; a negative ``get`` index needs a list column. ``rsplit`` is ``split`` (as the eager one is): both scan from the left, so a separator that overlaps itself (``--`` in ``a---b``) splits as ``split`` does, where Python's ``rsplit`` scans from the right. The pandas names the expression layer already had under another spelling (``startswith``, ``endswith``, ``len``, ``to_lowercase``, ``to_uppercase``, ``strip_chars``) stay as they are. A bool spells ``true`` or ``false`` in an expression cast; ``col("b").cast("string").capitalize()`` gives ``True`` and ``False``, as the eager ``astype`` does. The kernels scan the whole data buffer with SIMD (Highway, the target of the CPU at run time); ``DFTRACER_UTILS_STRING_SCALAR`` set in the environment runs the byte-at-a-time reference instead, for tests and benchmarks (``benchmarks/string_methods_bench.py``). The character-class predicates pick a scan per column from the mean string length: from 24 bytes a row each string is read vector by vector and stops at its first bad vector, below that a bit mask is built over the whole data buffer; ``DFTRACER_UTILS_STRING_PREDICATE`` set to ``bits`` or ``rows`` forces one.
- **column ops in an expression**: ``col("x").cum_sum()``, ``.shift(1)``, ``.rolling_mean(3)``, ``.rank()``, ``.forward_fill()``, ``.sort()``, ``.str.pad_start(5, "0")``, ``.dt.hour()``, ``.dt.strftime("%F %T")``, each ``.over("k")`` for the group-wise form; eagerly through ``df.apply(expr)``, in a plan as its own step (``dftu.frame.column_op``)

.. code-block:: python

   worst = df.sort_by("sum_dur", descending=True).head(10)
   hist = df["cat"].value_counts()                # a two-column DataFrame

Series operations
-----------------

A ``Series`` is NumPy-like: ``a + b``, ``a * 2``, ``s[0]``, ``s[2:5]``,
``np.asarray(s)``, plus reducers and kernels.

- **reduce**: ``sum() min() max() mean() product() prod() count() nunique() arg_min() arg_max() mode() all() any()``, ``quantile(q) median() stddev() variance() sem() skewness() kurtosis()``

  A bool column counts ``True`` as 1, so ``(x > 2).sum()`` counts the matches and ``mean()`` is the fraction that is true. ``min()`` and ``max()`` of a string column give the bytewise smallest and largest string (the empty string when no value is valid). A reduction the engine does not define, such as ``sum()`` of a string or a timestamp, raises ``TypeError`` (C++ throws ``DFTUtilsException``; the C ABI returns a scalar tagged ``DFTU_SCALAR_TAG_ERR``); it never returns 0. A numeric column with no valid value still reduces to 0.
- **element-wise**: ``abs() clip(lo, hi) round() ceil() floor() trunc() sign() sqrt() exp() log() fillna(v) (a bool column takes a boolean, a string column a string) where(cond, other) mask(cond, other) astype(t) / cast(t) full_like(v)``

  ``where(cond, other)`` and ``mask(cond, other)`` take a null ``other`` (``None`` or ``pd.NA``): the rows it selects become null and the column keeps its type. ``clip`` takes one bound, ``clip(lower=0)`` or ``clip(upper=b)``; a null stays null and ``clip()`` with no bound raises ``TypeError``. On a frame, ``df.astype(dtype)`` with one dtype casts every column (a mapping still casts only the named ones) and, if a column cannot be cast, raises ``TypeError`` that names the column and both types; ``df.with_columns(z=1)`` and ``z=lit(1)`` add a column of the frame's row count holding the value (an empty column for a frame with no rows).

  ``astype(t)`` takes a ``DType``, its int code, a dtype name, a Python type (``str``, ``int``, ``float``, ``bool``) or a NumPy type. Integers, floats and bools cast to string (a float as the shortest text that shows a point or exponent, ``2.0`` and ``1e+21``; a bool as ``True`` or ``False`` from Python's ``astype``, ``true`` or ``false`` from the engine and the C++ and C APIs), integers and floats cast to bool (zero is false; a NaN and a null stay null), and a string casts to a number by parsing it (text that does not parse is null). A column cast to its own type is returned as it is. Any other pair, such as string to bool, raises ``TypeError`` that names both types; the C ABI returns NULL. The column-expression form ``col(x).cast("string")`` takes the same targets as the eager cast (``int64``, ``uint64``, ``float64``, ``bool`` and ``string``) and spells a bool ``true`` or ``false``, the engine's text.
- **scan / window**: ``cumsum() cumprod() cummax() cummin() cum_count() diff() pct_change() shift(n) rank(method, pct=False) ffill() bfill() interpolate()``, ``rolling(w)`` / ``expanding()`` / ``ewm(alpha | span | com | halflife)`` with ``.sum() .mean() .min() .max() .var() .std() .median() .quantile(q)``
  ``rank(pct=True)`` divides each rank by the count of non-null values, a percentile rank in (0, 1] (by the number of distinct values for ``method="dense"``, as pandas does); a null value and a float ``NaN`` have a null rank and are not counted. The C ABI's ``dftu_series_rank`` takes a ``flags`` argument in place of ``descending``: ``DFTU_RANK_FLAG_DESCENDING`` (1, what a caller passing 1 already meant) and ``DFTU_RANK_FLAG_PCT`` (2). ``DataFrame.sort_index(axis=1)`` orders the columns by name.
- **select / test**: ``sort() head(n) reverse() top_k(k) unique() drop_nulls() is_in(values) is_nan() is_finite() is_infinite() is_duplicated() is_unique() is_sorted() compare(other)``
- **strings**: ``str_contains() str_starts_with() str_ends_with() str_like() str_matches(re) to_lowercase() str_len_bytes() str_split(sep)`` ..., and the ``.str`` / ``.dt`` / ``.list`` accessors (``pad``, ``zfill``, ``replace``, ``extract``, ``regex_replace``; ``year`` .. ``nanosecond``, ``floor`` / ``ceil`` / ``round(freq)``, ``strftime(fmt)`` (UTC), ``tz_localize`` / ``tz_convert``; ``len``, ``get``, ``join``)

  ``Expr.starts_with``, ``ends_with``, ``contains`` and ``replace_all`` also take an ``Expr`` argument, such as ``col("fname").starts_with(col("prefix"))``, which compares each row with its own needle. ``replace``, which replaces the first match only, keeps literal arguments. The eager ``Series.str`` methods keep literal arguments. In C++ the builders are ``expr_str_pred_col``, ``expr_str_replace_col``, ``expr_str_substr_col``, ``expr_round_col`` and ``expr_str_extract_col``; the C ABI has ``dftu_expr_str_pred_col``, ``dftu_expr_str_replace_col``, ``dftu_expr_str_substr_col`` and ``dftu_expr_round_col``.

- **registry**: ``ops.run(name, *columns)`` runs any registered op by name (built-in or a plugin's, a ``@jit.series``), ``ops.info(name)`` describes its signature; ``s.ops.<module>.<name>()`` is the same as a method

.. code-block:: python

   p99 = df["sum_dur"].quantile(0.99)
   posix = df.filter(df["cat"].str_eq("POSIX"))

Convert out (only at the edge)
------------------------------

.. code-block:: python

   df.to_arrow()     # pyarrow.Table (zero-copy)      df["count"].to_numpy()
   df.to_pandas()    # pandas DataFrame               df["count"].to_pandas()
   df.to_polars()    # polars DataFrame
