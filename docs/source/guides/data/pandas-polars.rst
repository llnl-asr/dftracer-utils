:description: Use the DataFrame, Series and LazyFrame with pandas or polars muscle memory: the index model, loc and iloc, group-by functions, the accessors, expression column ops, and what is left out on purpose.

The pandas and polars surfaces
================================

The frame types are a drop-in for both pandas and polars: swap ``pandas``
(or ``polars``) for ``dftracer.utils`` and most code runs unchanged, on the
SIMD engine, eagerly or as a plan. pandas convention is primary for names,
argument names and default semantics; the polars spellings sit alongside as
thin aliases on the same objects. Every method here is a composition of
engine ops (nothing runs in Python per row), and every value is checked
against pandas or polars on the same data in ``tests/python/test_pandas_surface.py``
and ``test_polars_surface.py``. ``benchmarks/dataframe_vs_pandas_polars.py``
is the proof at scale: its pandas column and ours are the same code
(``df[df["v"] > 0.5]``, ``groupby("k")["v"].agg(["sum", "mean"])``,
``nlargest``, ``.str.contains``), checked for the same result and timed
side by side; see :doc:`../runtime/performance`.

The index is a named column
-----------------------------

There is no separate index object. ``set_index("ts")`` records the column
name; the column stays a column, ``reset_index`` forgets the name. ``loc``
therefore filters by the value of that column, ``iloc`` by position, and
``set_index`` over several columns records them all (``loc[(a, b)]`` filters
on both). Slicing follows pandas: ``iloc[1:3]`` is two rows, ``loc[1:3]`` is
inclusive.

.. code-block:: python

   df = DataFrame({"ts": [0, 1, 2], "v": [1.0, 2.0, 3.0]}).set_index("ts")
   df.loc[1]              # the row(s) whose ts == 1
   df.iloc[0:2]           # the first two rows
   df.at[2, "v"]          # one cell by label
   df.loc[df["v"] > 1, "v"] = 0.0   # copy-on-write: rebinds df, never mutates

Assignment through ``loc`` / ``iloc`` / ``at`` / ``iat`` is copy-on-write:
the name is rebound to a new frame, the old one is untouched, so a frame
another variable holds never changes under it.

Not there, on purpose: label alignment in arithmetic (``df + other`` is
positional) and a ``MultiIndex`` type. Both would put an index object
between the user and the columns.

Group-by
--------

``group_by`` (``groupby`` too) takes one name or several, or a Series
computed from the frame (the key column is then named ``key``), and
returns a group-by whose reductions are one engine ``group_by`` each;
``g["v"]`` or ``g[["v", "w"]]`` narrows it to those value columns, and
with one column a list of aggregate names labels the outputs by name
alone, as pandas does. Groups come out in first-seen order (pandas sorts
them unless ``sort=False``; polars and DuckDB do not):

.. code-block:: python

   g = df.group_by("k")                 # a null key is dropped, as pandas
   g = df.group_by("k", dropna=False)   # or its own group, as polars
   g = df.groupby(df["ts"] // 1_000_000)          # a computed key, named key
   g["v"].agg(["sum", "mean"])          # columns k, sum, mean
   g.sum(); g.mean(); g.size(); g.agg(total=("v", "sum"), n=("v", "count"))
   g.median(); g.quantile(0.9)          # exact, not a sketch
   g.cumsum(); g.shift(1); g.rank(); g.head(2); g.nth([0, -1])
   g.ffill(); g.bfill()                 # group-wise fills
   g.rolling(3).mean(); g.expanding().sum(); g.ewm(alpha=0.5).mean()
   g.take([0, -1]); g.sample(2, seed=1)
   g.resample("5s").sum()               # keys + time bucket
   g.corr(); g.cov()                    # long form: key, a, b, value
   g.transform("mean")                  # the group value on every row

``apply`` and ``filter`` take a user function. It is traced once on a
symbolic group, so a body over the group's columns compiles to one
``group_by`` (plus a join for a row result):

.. code-block:: python

   g.apply(lambda grp: grp["v"].sum() / grp["n"].sum())   # keys + value
   g.apply(lambda grp: grp["v"] - grp["v"].mean())        # one value per row
   g.filter(lambda grp: grp["v"].sum() > 10)              # the passing groups' rows

A body the trace cannot follow (``len(grp)``, a ``.to_list()``, ``is``) runs
in Python per group with a warning. A plan (``df.lazy().group_by(...)``)
has no Python tier and refuses such a body.

The datetime index and the accessors
--------------------------------------

Over the index column (or ``on=``): ``at_time("09:30")``,
``between_time(start, end)``, ``first("1min")``, ``last("1D")``,
``asfreq("1s", method="ffill")``, ``resample("5s")``, ``tz_localize("UTC")``,
``tz_convert("Asia/Tokyo")``. A timezone is type metadata on a Timestamp
column: the instants never move, and only UTC can mark a naive column (the
engine has no zone database). The calendar parts read UTC.

On a Series, ``.str`` (``len``, ``pad``, ``zfill``, ``replace``, ``split``,
``extract``, ...), ``.dt`` (``year`` .. ``nanosecond``, ``dayofweek``,
``floor`` / ``ceil`` / ``round(freq)``, ``normalize``, ``total_seconds``,
``tz``, ``tz_localize``, ``tz_convert``) and ``.list`` (``len``, ``get``,
``join``) carry the pandas accessors as batches of engine kernels.

Series and frame reductions and windows
-----------------------------------------

``sum mean median quantile var std sem skew kurt prod count nunique
mode`` on a Series and, per column, on a frame; ``cumsum cumprod cummax
cummin diff pct_change shift rank interpolate ffill bfill`` as scans;
``rolling(n)``, ``expanding()``, ``ewm(alpha | span | com | halflife)``
as window objects with ``.sum() .mean() .min() .max() .var() .std()
.median() .quantile(q)``. A rolling window holding a null is null
(``min_periods = window``); an expanding or ewm window repeats the value
on a null row; ``ewm`` is pandas' ``adjust=True``.

Expressions: the polars column ops
-----------------------------------

``col("x")`` builds an expression. Beside arithmetic, comparisons, casts
and the string predicates, an expression carries the polars column ops:

.. code-block:: python

   from dftracer.utils import col
   df.lazy().with_columns(
       (col("x").cum_sum() * 2 + col("x").shift(1)).alias("a"),
       col("x").rolling_mean(3).over("k").alias("b"),
       (col("x") - col("x").mean().over("k")).alias("c"),
       col("s").str.pad_start(5, "0").alias("d"),
       col("t").dt.hour().alias("e"),
   ).collect()

A column op (``cum_sum cum_prod cum_max cum_min cum_count shift diff
pct_change rank forward_fill backward_fill interpolate rolling_* ewm_mean
ewm_std sort arg_sort reverse is_duplicated is_unique is_nan is_finite
is_infinite hash log10 log1p``, the ``.str`` and ``.dt`` namespaces) reads
the whole column at once. Eagerly (``df.apply(expr)``) it runs the Series
kernel on its argument's value; in a plan it is its own step
(``dftu.frame.column_op``, a breaker), so ``cum_sum`` across a whole
stream is right. ``over(keys)`` is the group-wise form: the engine's
group transform, one value per input row; an aggregate's ``over``
(``col("x").sum().over("k")``) is the group-by joined back on the keys.
``sort`` inside an expression sorts that column alone, as polars; nulls
sort last (the pandas convention).

The polars frame names
------------------------

``sum_horizontal mean_horizontal min_horizontal max_horizontal`` (row-wise
over the numeric columns), ``hstack``, ``vstack``, ``gather_every``,
``partition_by``, ``join_asof``, ``iter_rows`` / ``rows`` / ``row`` /
``item`` / ``to_dicts`` (through Arrow), ``get_column(s)``, ``to_series``,
``is_empty``, ``n_unique``, ``fill_nan``, ``drop_nans``, and the writers
``write_parquet`` / ``write_csv`` / ``write_ipc`` (through pyarrow).

Converting out
--------------

.. code-block:: python

   df.to_arrow()                 # shares the engine's buffers, no copy
   df.to_pandas()                # NumPy dtypes: a copy
   df.to_pandas(arrow=True)      # pd.ArrowDtype columns: no copy
   df.to_polars()                # numbers shared, strings copied

Left out, and why
------------------

- ``to_period``: a Period is a span, not an instant; ``dt.floor``,
  ``resample`` and the calendar parts give the same buckets.
- ``tz_localize`` with a non-UTC zone on a naive column: a wall-time shift
  needs a zone database the engine does not have.
- ``MultiIndex`` methods, ``align`` / ``reindex``: label alignment was ruled
  out with the index model above.
- Plotting, styling, ``attrs`` / ``flags``, the file writers beyond
  Parquet / CSV / IPC: not this engine's layer.
- ``eval``: ``col(...)`` expressions cover it.
- ``GroupBy.fillna(value)``: pandas deprecates it; ``ffill`` / ``bfill``
  are the spellings.

See also
---------

- :doc:`dataframe` and :doc:`series` for the engine-first API.
- :doc:`time-windows` for the window functions and time buckets.
- :doc:`../core/columnar-ops` for the op catalog.
