:description: Use the DataFrame, Series and LazyFrame with pandas or polars muscle memory: the index model, loc and iloc, group-by functions, the accessors, expression column ops, and what is left out on purpose.

The pandas and polars surfaces
================================

The frame types are a drop-in for both pandas and polars: swap ``pandas``
(or ``polars``) for ``dftracer.utils`` and most code runs unchanged, on the
SIMD engine, eagerly or as a plan. polars names are canonical; the pandas
spellings are aliases from one table, with pandas keywords and pandas
results, on the same objects. Every method here is a composition of
engine ops (nothing runs in Python per row), and every value is checked
against pandas or polars on the same data in ``tests/python/test_pandas_surface.py``
and ``test_polars_surface.py``. ``benchmarks/dataframe_vs_pandas_polars.py``
is the proof at scale: its pandas column and ours are the same code
(``df[df["v"] > 0.5]``, ``groupby("k")["v"].agg(["sum", "mean"])``,
``nlargest``, ``.str.contains``), checked for the same result and timed
side by side; see :doc:`../runtime/performance`.

``dftracer.utils.pandas`` and ``dftracer.utils.polars`` are compatibility
namespaces: they re-export the same engine classes that ``dftracer.utils``
exports, with the pandas or polars module-level functions beside them, so
``import dftracer.utils.polars as pl`` reads like polars. They are the one place
a name has a second import path.


.. _pandas-polars-names:

Names: one per idea
-------------------

A method has one canonical name, the polars one: ``fill_null``, ``is_null``,
``is_not_null``, ``cast``, ``is_in``, ``drop_nulls``, ``n_unique``,
``cum_sum``, ``forward_fill``, ``group_by``, ``sort``, ``unique``, ``gather``,
``head``. The pandas spelling is an alias, the same object under another
name, from one table (``python/dftracer/utils/_aliases.py``) that each class
reads when it is built; ``s.fillna`` **is** ``s.fill_null``. A pandas name
whose keywords or result differ from the canonical method is a small function
in its class (marked "own signature" below), so ``df.sort_values("a",
ascending=False, na_position="first")`` and ``df.groupby("k").sum()`` give what
pandas gives, while ``df.sort("a", descending=True, nulls_last=False)`` gives
the polars call.

.. list-table:: Aliases by class
   :header-rows: 1
   :widths: 14 56 30

   * - Class
     - pandas spellings (alias = canonical)
     - polars spellings
   * - ``Series``
     - ``aggregate`` = ``agg``, ``astype`` = ``cast``, ``isna`` = ``is_null``, ``isnull`` = ``is_null``, ``notna`` = ``is_not_null``, ``notnull`` = ``is_not_null``, ``dropna`` = ``drop_nulls``, ``fillna`` = ``fill_null``, ``isin`` = ``is_in``, ``nunique`` = ``n_unique``, ``prod`` = ``product``, ``argmax`` = ``arg_max``, ``idxmax`` = ``arg_max``, ``argmin`` = ``arg_min``, ``idxmin`` = ``arg_min``, ``cumsum`` = ``cum_sum``, ``cumprod`` = ``cum_prod``, ``cummax`` = ``cum_max``, ``cummin`` = ``cum_min``, ``searchsorted`` = ``search_sorted``, ``tolist`` = ``to_list``, ``ffill`` = ``forward_fill``, ``pad`` = ``forward_fill``, ``bfill`` = ``backward_fill``, ``backfill`` = ``backward_fill``, ``kurt`` (own signature) = ``kurtosis``, ``take`` = ``gather``, ``iat`` = ``iloc``, ``at`` (own signature) = ``iloc``, ``loc`` (own signature) = ``iloc``, ``rtruediv`` = ``rdiv``, ``divide`` = ``truediv``, ``div`` = ``truediv``, ``multiply`` = ``mul``, ``subtract`` = ``sub``, ``sort_values`` (own signature) = ``sort``, ``nlargest`` (own signature) = ``top_k``, ``nsmallest`` (own signature) = ``bottom_k``, ``drop_duplicates`` (own signature) = ``unique``, ``between`` (own signature) = ``is_between``
     - ``alias`` = ``rename``, ``clone`` = ``copy``, ``limit`` = ``head``, ``not_`` = ``__invert__``
   * - ``Expr``
     - ``isna`` = ``is_null``, ``isnull`` = ``is_null``, ``notna`` = ``is_not_null``, ``notnull`` = ``is_not_null``, ``isin`` = ``is_in``, ``astype`` = ``cast``, ``fillna`` = ``fill_null``, ``cumsum`` = ``cum_sum``, ``cumprod`` = ``cum_prod``, ``cummax`` = ``cum_max``, ``cummin`` = ``cum_min``, ``ffill`` = ``forward_fill``, ``bfill`` = ``backward_fill``, ``between`` (own signature) = ``is_between``, ``startswith`` = ``starts_with``, ``endswith`` = ``ends_with``
     - none
   * - ``DataFrame``
     - ``aggregate`` = ``agg``, ``astype`` = ``cast``, ``dropna`` = ``drop_nulls``, ``fillna`` = ``fill_null``, ``groupby`` (own signature) = ``group_by``, ``sort_values`` (own signature) = ``sort``, ``drop_duplicates`` = ``unique``, ``take`` = ``gather``, ``nlargest`` (own signature) = ``top_k``, ``nsmallest`` (own signature) = ``top_k``, ``divide`` = ``div``, ``truediv`` = ``div``, ``multiply`` = ``mul``, ``subtract`` = ``sub``, ``product`` = ``prod``, ``rtruediv`` = ``rdiv``, ``map`` = ``applymap``, ``isnull`` = ``isna``, ``notnull`` = ``notna``, ``backfill`` = ``bfill``, ``pad`` = ``ffill``
     - ``limit`` = ``head``, ``clone`` = ``copy``, ``select_seq`` = ``select``, ``with_columns_seq`` = ``with_columns``
   * - ``LazyFrame``
     - ``aggregate`` = ``agg``, ``sort_values`` (own signature) = ``sort``, ``drop_duplicates`` = ``unique``, ``take`` = ``gather``
     - ``limit`` = ``head``, ``select_seq`` = ``select``, ``with_columns_seq`` = ``with_columns``
   * - ``GroupBy``
     - ``aggregate`` = ``agg``, ``product`` = ``prod``
     - none
   * - ``dftracer.utils.typing.LazyGroupBy``
     - ``aggregate`` = ``agg``, ``product`` = ``prod``
     - none

Moments: ``skew`` and ``kurt`` follow the name. ``Series.skew()`` and
``Series.kurt()`` are the pandas estimators (unbiased; NaN below three values
for ``skew`` and four for ``kurt``, and for a constant column, as pandas 3;
pandas 2 gives 0.0 there), and so are ``DataFrame.skew``, ``DataFrame.kurt``
and the group-by ``skew`` and ``kurt``. The polars keywords give the polars
values: ``Series.skew(bias=True)`` and ``Series.kurtosis(fisher=True,
bias=True)`` are the population moments (NaN for one value or a constant
column, null for no values), ``kurtosis(bias=False)`` the unbiased one (null
below four values), and ``DataFrame.kurtosis`` gives the same value per
column. ``skew`` is one name in both libraries; our ``Series``
attribute names are pandas-shaped, so ``Series.skew()`` follows pandas and
``Expr.skew()`` follows polars (the population value) and takes ``bias=False``
for the pandas estimator, as does ``Expr.kurt``. The engine has both: the
aggregates ``skew`` and ``kurt`` are the population moments, and ``skew_unbiased``
and ``kurt_unbiased`` are the pandas estimators (NaN below three and four
values and for a constant column). The ``"skew:col"`` and ``"kurt:col"``
strings are the population ones; the pandas names in ``agg("skew")``,
``agg({"x": "kurt"})`` and ``agg(out=("x", "skew"))``, the lazy ``skew()`` and
``kurt()``, ``DataFrame.skew`` and the group-by methods use the pandas
estimators, in memory and when a plan spills.

The keywords that a name shares across ``Series``, ``DataFrame``,
``LazyFrame`` and ``Expr`` are the same: ``fill_null(value)``, ``shift(n)``,
``clip(lower, upper)``, ``cast(dtype)``, ``head(n)``, and
``sort(by, *, descending=False, nulls_last=False)`` (``by`` on a frame). Nulls
come first by default, as in polars; a NaN sorts after every number and apart
from the nulls. ``sort_values(by, ascending=True, na_position="last")`` keeps
the pandas defaults (nulls last); pandas treats a NaN as missing, ours stays a
value. The engine sort takes ``nulls_last`` itself, per key for a frame, and is
stable in every mode: equal values and nulls keep their input order, also with
``descending=True`` and nulls first. A lazy sort that asks for nulls first is
not pushed into a source that sorts, which orders nulls last.

``first_valid_index()`` and ``last_valid_index()`` skip nulls only: a NaN is a
value here, where pandas skips it. ``equals`` and ``compare`` follow pandas for
a NaN: two NaN at the same position are equal and ``compare`` does not list them.

``unique(maintain_order=False)`` is the polars one: the distinct values, a null
kept once, sorted with nulls first, or in order of first occurrence with
``maintain_order=True``; ``drop_duplicates(keep="first")`` is the pandas result
(first-occurrence order, one null kept). ``arg_sort(descending, nulls_last=False)``
has the ``sort`` keywords and default, so ``s.gather(s.arg_sort())`` is
``s.sort()``, and equal values and several nulls keep their index order. ``search_sorted(element, side="left")`` takes a scalar (an int) or
values, and ``side`` ``"left"``, ``"right"`` or ``"any"``. ``mul``, ``sub``,
``add`` and ``truediv`` take a Series or a scalar.

``s.is_null()`` is the Bool mask; ``s.is_null_at(i)`` is the bool for one row.
``s.cast(dtype)`` takes a ``DType``, its name, its code or a Python type.

The accessors follow the same rule. Where pandas and polars share a name, the
object decides the shape: a ``Series`` is pandas-shaped, so
``s.dt.year``, ``s.dt.weekday`` (Monday is 0) and ``s.dt.dayofyear`` are
properties that give a ``Series``, and ``s.str.partition(",")`` gives a frame
of three columns; an ``Expr`` is polars-shaped, so ``col("t").dt.year()`` and
``col("t").dt.weekday()`` (Monday is 1) are methods and
``col("s").str.partition(",")`` is one list column. A name only polars has
(``dt.epoch``, ``dt.iso_year``, ``dt.ordinal_day``, ``dt.week``,
``dt.truncate``, ``str.count_matches``, ``str.strip_chars_start``,
``str.strip_chars_end``, ``str.strip_prefix``, ``str.strip_suffix``,
``str.to_titlecase``) is a method on both sides with polars results; a name
only pandas has (``dt.dayofweek``, ``dt.is_month_start``, ``dt.floor``,
``dt.tz_localize``, ...) is a property on a ``Series`` and a method on an
``Expr``. An ``Expr`` method takes the keywords of the ``Series`` method of
the same name (``pat``, ``case``, ``regex``, ``n``, ``errors``, ``expand``,
``width``, ``sub``, ``repl``, ``freq``). What an expression cannot do raises
``NotImplementedError`` and says so: ``str.get_dummies``, ``str.cat()`` without
``others``, ``str.extractall``, ``expand=True`` on ``partition`` and ``split``,
``decode(errors="strict")``. A method on a column of the wrong type raises
``DFTUtilsValueError``, on a ``Series`` and when an ``Expr`` runs.

``dt.as_unit(unit)`` (pandas) and ``dt.cast_time_unit(time_unit)`` (polars)
change the unit of a Timestamp or Duration and keep the instant. A coarser unit
floors toward negative infinity, except that ``cast_time_unit`` truncates a
Duration toward zero, as polars does. A value that does not fit 64 bits in a
finer unit raises ``DFTUtilsValueError``; polars wraps it. ``dt.date`` gives a
Date32 column and ``dt.time`` the time of day in the column's own unit (Time32
for seconds and milliseconds, Time64 for microseconds and nanoseconds), both of
the wall time in the column's zone, like every ``.dt`` part. ``dt.timetz`` is
``dt.time``: a time-of-day column has no zone to carry. ``col("v").arg_max()`` and ``arg_min()`` give the position
within the group of the first extreme present value (null for a group with
none); they need the rows in input order, so ``TraceViewer.agg`` refuses them.
``groupby(k)["v"].nlargest(n)`` and ``nsmallest(n)`` give the keys, the row
position as ``index`` and the value (``keep="first"`` only); ``unique()`` gives
a List column of the first-seen distinct values, ``is_monotonic_increasing`` and
``is_monotonic_decreasing`` a Bool column, all in key order. ``groupby(k)
.corrwith(other)`` correlates each column with the paired column of ``other``
(rows pair by position, ``method="pearson"`` only).

The polars math methods ``sin``, ``cos``, ``tan``, ``cot``, ``arcsin``,
``arccos``, ``arctan``, ``sinh``, ``cosh``, ``tanh``, ``arcsinh``, ``arccosh``,
``arctanh``, ``cbrt``, ``log10``, ``erf`` and ``erfc`` work on a ``Series`` and
an ``Expr``. A Float32 column stays Float32 and any other numeric column gives
Float64; a value outside the domain is NaN, as ``arcsin(2)``. The bitwise
methods ``bitwise_count_ones``, ``bitwise_count_zeros``, ``bitwise_leading_ones``,
``bitwise_leading_zeros``, ``bitwise_trailing_ones`` and
``bitwise_trailing_zeros`` give a UInt32 column within the width of an integer
or Bool column. ``bitwise_and``, ``bitwise_or`` and ``bitwise_xor`` reduce a
column to a value of its own type (``None`` when no value is present) and, on an
``Expr``, run per group in ``group_by().agg``.

Removed: ``ColumnExpr`` and ``Field`` (use ``Expr`` and ``col``),
``TraceViewer.query`` and ``explain_query`` (use ``duql`` and ``explain_duql``),
``TraceViewer.limit`` (use ``head``; ``DataFrame.limit`` and
``LazyFrame.limit`` stay as the polars alias of ``head``) and the native
``Series.cast(type_id)`` (``cast`` takes the dtype).

The index is a named column
-----------------------------

A frame holds its row index as a list of column names, ``index_names()``, next
to its columns. ``set_index("ts")`` records the column name; the column stays a
column, ``reset_index`` forgets the name. ``loc`` therefore filters by the value
of that column, ``iloc`` by position, and ``set_index`` over several columns
records them all (``loc[(a, b)]`` filters on both). A frame with no index is the
positional ``RangeIndex``: nothing is stored. Slicing follows pandas:
``iloc[1:3]`` is two rows, ``loc[1:3]`` is inclusive.

The names travel with the frame. Every frame op has one rule, in
``frame_op_index_rules.def``: a row filter, sort, slice, ``take``, ``head``,
``unique``, ``select``, ``with_columns`` and ``explode`` keep the names whose
columns are still there; ``rename`` follows the renamed columns; ``group_by``,
``group_by_dynamic`` and ``pivot`` index the result by their key,
time columns when the input was indexed; ``join`` keeps the left key names
when both sides are indexed on exactly their join keys, and ``concat`` and
``union`` the shared names when every input has the same ones; ``describe``,
``reduce``, ``with_row_index``, ``unpivot`` (pandas ``melt`` gives a
``RangeIndex``) and the like give no index. A lazy plan applies
the same rules, so ``collect()`` has the index the eager op gives. An unindexed
frame stays unindexed under every rule.

``select`` keeps the index names whose columns it keeps: ``select("k", "v")`` on
a frame indexed by ``k`` and ``j`` keeps ``k`` as the index and drops ``j`` from
the names. The index follows the rule of each op, never "all or nothing".

The pandas names show a frame with an index as pandas does. ``columns``,
``dtypes``, ``shape``, ``keys()``, ``items()``, ``to_numpy()``, ``df[list]`` and
the reducers (``sum``, ``mean``, ...) leave out the index columns, ``loc`` and
``iloc`` count positions without them, and ``df[["k"]]`` on an index column is a
``KeyError``. The Polars names see every column: ``schema``, ``width``,
``select``, ``filter``, ``with_columns``, ``group_by``, ``col("k")`` and
``df["k"]`` still name an index column, because the column is still there. A
frame with no index lists every column, as before. ``df["k"]`` and ``"k" in
df`` still see an index column, because the Polars names need it. A ``LazyFrame`` lists every
column of its plan in ``columns``; its ``collect()`` result follows these rules.

The pandas names that read data columns skip the index columns and keep them
in step with the rows: ``describe``, ``drop_duplicates``, ``dropna``,
``fillna``, ``to_dict``, ``to_csv`` (which writes the index when there is one),
and ``drop(columns=[...])``, which raises ``KeyError`` for an index column. The
Polars names ``unique``, ``drop_nulls``, ``fill_null`` and ``drop("k")`` see all
columns. The pandas ``groupby(...)`` has ``as_index`` (default ``True``): an
aggregate result is indexed by the keys, as in pandas, even when the frame had
no index; ``groupby(..., as_index=False)`` and the Polars ``group_by`` keep the
keys as columns.

One dtype difference of the pandas ``groupby`` stays for now.
``shift(fill_value=...)`` raises
``TypeError`` for a fill the column cannot hold (text takes text, a bool column
a bool, a number column a number), where pandas gives an object column.

The index crosses the formats. ``to_pandas()`` moves the index columns into the
pandas index (a ``MultiIndex`` for several; a level that ``from_pandas`` named
``__index_level_<i>__`` is unnamed again), and ``from_pandas`` records a
non-``RangeIndex`` as ``index_names``. Arrow IPC, the C Data Interface and the
C Stream Interface carry the names in the schema metadata key
``dftracer.utils.index`` (a JSON array), and an Arrow schema with pandas
metadata maps its ``index_columns`` to the names (a ``RangeIndex`` is none). The
C ABI reads them with ``dftu_frame_index_count`` and ``dftu_frame_index_name``
and sets them with ``dftu_frame_with_index``. A list or struct column cannot be
an index.

.. code-block:: python

   df = DataFrame({"ts": [0, 1, 2], "v": [1.0, 2.0, 3.0]}).set_index("ts")
   df.loc[1]              # the row(s) whose ts == 1 (a hash lookup, built once)
   df.iloc[0:2]           # the first two rows
   df.at[2, "v"]          # one cell by label
   df.loc[df["v"] > 1, "v"] = 0.0   # copy-on-write: rebinds df, never mutates

Assignment through ``loc`` / ``iloc`` / ``at`` / ``iat`` is copy-on-write:
the name is rebound to a new frame, the old one is untouched, so a frame
another variable holds never changes under it.

Not there yet: label alignment in frame arithmetic (``df + other`` is
positional) and a ``MultiIndex`` type (several index columns stand for one).

Label lookup
~~~~~~~~~~~~

``loc``, ``at``, ``xs``, ``get`` and ``s[label]`` find rows by label through a
hash the frame or Series builds natively on its first lookup and keeps: the
hash is built once even when several threads look up at once, shared by every
copy of the same data and freed with it. A lookup after the first costs the keys
only. Without the hash, ``loc`` compared the whole index column for every
label. Labels compare by value: ``1.0`` finds the label ``1`` (a label above
``2**53`` is compared to an integer key exactly, not through a float), a NaN key
finds the NaN labels of a float level, a ``None`` key finds the null labels of a
text level only (on a number level it is a ``KeyError``, as pandas), a string
finds no number, and a repeated label gives all its rows in row order. A bool
is its own kind of label: ``True`` is not ``1`` (``KeyError`` either way). One
float key on a ``float32`` level is cast to ``float32`` first, as pandas, so
``s.loc[0.1]`` finds it; a list of keys is not cast. A date or timestamp key is
converted to the type of the labels, and a timezone-naive key does not find a
timezone-aware label nor the reverse (``KeyError``, as pandas).

.. code-block:: python

   s = DataFrame({"k": [30, 10, 20, 10], "v": [1.0, 2.0, 3.0, 4.0]}).set_index("k")["v"]
   s.loc[30]            # 1.0: one row, so the value
   s.loc[10]            # a Series of the two rows labelled 10
   s.loc[[20, 30, 20]]  # in the order asked for, a label repeated gives its rows again
   s.loc[[True, False, False, True]]   # a Bool mask
   s.loc[20]            # KeyError: 20 (a missing label is a KeyError naming it)

A label slice is inclusive on both ends. On a sorted index (increasing or
decreasing; for a MultiIndex, sorted by its levels) a bound need not be a label:
the run is found by binary search. On an unsorted index both bounds must be
labels that no other row has, and the run is the rows from the first to the
second (empty when the second comes first), a missing or repeated bound is a
``KeyError``. A bound of a type that does not compare with the labels
(``"a":"b"`` on integers) is a ``TypeError``, and a slice takes no step.

On a MultiIndex a tuple names the leading levels: the whole tuple is one row,
a shorter one is every row below it and drops the levels it names, as pandas;
``xs(key, level=...)`` selects on any level and drops it unless
``drop_level=False``. A list of tuples finds each in order.

The result shapes follow pandas, with the frame rules above. A Series gives the
value for a whole label of one row, a Series for a repeated label or a part of
a MultiIndex label. A frame gives a one-row frame for a whole label (pandas
gives a Series, which a frame of mixed types cannot be) and ``at`` the value,
or the column Series for a repeated label. A Series with no labels has the
positions as labels, so ``s.loc[1]`` is the second element and a result is
labelled by the positions it keeps; ``loc`` on a frame with no index gives the
result a default index, where pandas keeps the original positions. ``loc`` with a Series of labels takes them
in the Series' order. ``s[label]`` follows pandas 3: an integer is a label on a
Series with labels (pandas 2 fell back to the position when the labels are not
integers); on a Series without labels ``s[i]`` stays positional, and a slice with
integer bounds is positional (``s[1:3]``) as pandas does, any other bounds are
labels. ``get`` gives its default for a missing label. Assignment by label
(``df.loc[label, "c"] = v``) writes every row with that label; a label that is
not there is a ``KeyError`` (no enlargement). A LazyFrame has no frame to hash:
its ``loc`` still builds a mask over the index columns.

Reindex and align
~~~~~~~~~~~~~~~~~

``reindex``, ``reindex_like`` and ``align`` work on a Series and a DataFrame as
in pandas 2 and 3, over the same label hash as ``loc``. ``reindex(labels)`` (or
``index=``) gives the rows of the labels you ask for, in that order, with the
labels you gave as the new index: a label with no row gets a null, and an
integer column that gains a null becomes Float64, as pandas. ``fill_value``
fills only the new rows and keeps the column type when it fits (an integer in
range for an integer column, any number for a float column, a string for a
string column); a float into an integer column widens it, and a value a column
cannot take is a ``TypeError`` (pandas 3; pandas 2 makes the column ``object``).
Labels that equal the index, row for row, return the frame unchanged even when
they repeat; otherwise a repeated label is a ``ValueError`` ("cannot reindex on
an axis with duplicate labels"). A frame or Series with no index has the labels
``0 .. n-1``. A MultiIndex takes a list of tuples naming every level (a scalar
key is a ``ValueError``, where pandas gives all-NaN).

``method`` fills a label that has no row from the row of the nearest label:
``'ffill'``/``'pad'`` before it, ``'bfill'``/``'backfill'`` after it,
``'nearest'`` on either side (a tie takes the larger label). It needs labels
that increase or decrease, found by binary search, and raises ``ValueError``
for any other order. On decreasing labels "before" and "after" follow the label
order, as pandas. ``limit`` caps how many labels one row fills (the exact match
counts for nothing) and needs increasing labels and targets that do not
decrease; ``tolerance`` bounds ``|target - label|``, in label units or as a
timedelta for timestamp, date and duration labels. ``limit`` or ``tolerance``
with no method is a ``ValueError``; ``nearest`` and ``tolerance`` on text labels
a ``TypeError``, and on a MultiIndex a ``NotImplementedError``. A fill ``method``
with ``columns=``, a tolerance per label and ``level=`` are not supported, and
column labels must be strings. ``reindex(columns=[...])`` adds a column of nulls
(Float64) or of ``fill_value`` for a name the frame lacks. ``reindex_like(other)``
is ``reindex`` on the labels (and, for a frame, the columns) of ``other``.

``align(other, join=...)`` gives the two objects on the join of their row
labels: ``outer`` (default) the sorted union, nulls last; ``inner`` the common
labels in the order of the first; ``left`` and ``right`` one side's labels. A
repeated label meets every row that has it (the first object's rows outermost),
and labels that are equal stay as they are. A DataFrame aligns its columns too
(``axis=None``), only the rows (``axis=0``) or only the column names
(``axis=1``); against a Series it needs ``axis``, and a Series against a
DataFrame aligns on the rows. Missing rows and columns are nulls (Float64 for
an integer column). ``fill_value`` fills the nulls of a Series result, and only
the new rows and columns of a frame result, as pandas.

.. code-block:: python

   s = DataFrame({"k": [10, 20, 30], "v": [1, 2, 3]}).set_index("k")["v"]
   s.reindex([20, 40, 10])                       # [2.0, NaN, 1.0] under 20, 40, 10
   s.reindex([20, 40], fill_value=0)             # stays integer: [2, 0]
   s.reindex([5, 15, 35], method="ffill", tolerance=10)   # [NaN, 1.0, 3.0]
   left, right = s.align(other, join="inner")    # the common labels, first object's order

A Series carries labels too
~~~~~~~~~~~~~~~~~~~~~~~~~~~

A ``Series`` holds optional label columns, one per level, as long as it is: the
pandas index, as columns. A Series with none is positional (nothing is
stored). ``df["a"]`` of a frame with an index is a Series under the frame's
index, ``Series.from_pandas`` reads a non-default pandas index and
``to_pandas()`` writes it back (a ``MultiIndex`` for several levels).
``s.index`` gives the labels (a Series for one level, a frame for several, the
positions when there are none), ``s.index_names()`` the level names,
``s.set_axis(labels)`` and ``s.rename_axis(name)`` change them,
``s.reset_index()`` moves them into a frame, and ``s.to_frame()`` makes them
the frame's index.

The ops that keep rows in place keep the labels (casts, ``fillna``, ``cumsum``,
``shift``, ``diff``, ``rank``, scalar arithmetic, string and date ops), and the
ops that select or reorder rows move them with the rows (``sort``, ``filter``,
``take``, ``head``, ``tail``, ``slice``, ``drop_nulls``, ``top_k``,
``bottom_k``, ``sample``, ``reverse``). Reducers and ``unique`` give none. The
pandas spellings ``sort_values``, ``nlargest``, ``nsmallest``, ``tail``,
``take``, ``dropna``, ``drop_duplicates``, ``repeat`` and ``truncate`` label a
positional Series by the positions they keep, as pandas shows the original row
of each; the Polars names give a positional Series no labels.

The pandas spellings of a frame do the same: ``dropna``, ``drop_duplicates``,
``sort_values``, ``nlargest``, ``nsmallest``, ``tail``, ``take``, ``sample``,
``query``, ``df[mask]`` and the rows picked by ``loc`` and ``iloc`` give a frame
with no index an Int64 index of the positions it kept (an op that keeps every
row in order makes none), and the Polars names (``drop_nulls``, ``unique``,
``sort``, ``top_k``, ``gather``, ``filter``, every ``LazyFrame`` op) stay
positional. ``df.loc[label]`` of a label one row holds is a Series over the
columns named by the label when that is text (a row of text and numbers is a
``Row``, as this engine has no object column), ``df.loc[label, column]`` is the
value, and a repeated label gives a frame. ``df.loc[new] = values``,
``df.loc[new, column] = value``, ``s[new] = value`` and ``s.loc[new] = value`` add
a row for a label the index does not have (pandas' setting with enlargement); the
other columns are null, a label that does not fit the index type is a
``TypeError``, and setting a label a Series already has is a ``TypeError``, as a
Series is immutable.
``s[new] = value`` also raises ``BufferError`` while a numpy array or memoryview
over the Series is alive, and ``TypeError`` for a value of another type than the
Series holds, where pandas upcasts. ``DataFrame.query`` takes the duql
expression language, not the pandas one: there is no ``@var`` for a Python
variable and no backticks around a column name.

Arithmetic between two Series aligns on the labels, as pandas does. Labels that
are equal, level for level and in order, leave the rows in place. Otherwise the
result is the outer join of the labels, sorted, with a null where a side has no
row (an integer column that gains a null widens to Float64, a Bool one takes
false), a duplicated label matches every row that has it, a null label equals
another null label and sorts last, and the result takes the left side's level
names. A positional Series counts as the labels ``0 .. n-1`` when the other has
labels. The labels must have the same number of levels on both sides, and numeric
labels of different types meet in their common type (``Int32`` with ``Int64`` in
``Int64``, an integer with a float in ``Float64``). A string label against a
number raises ``ValueError`` (pandas would produce an all-NaN union). A missing
side makes ``**`` null, where pandas gives ``1.0`` for ``1 ** NaN``. ``where``,
``mask`` and ``str.cat`` of two Series with labels need the same labels and
raise ``ValueError`` otherwise. ``sort_index`` orders the rows by their labels,
level by level. ``loc``, ``at``, ``get``, ``xs`` and ``s[label]`` look rows up by
label (see Label lookup). ``s[mask]`` keeps the rows where a Bool Series is true, a null counting as false. Comparing two
Series whose labels differ raises ``ValueError``, as pandas.
``Series.value_counts`` and ``DataFrame.value_counts`` are the pandas Series of
counts named ``count`` (``proportion`` with ``normalize=True``), labelled by the
values (one level per column for a frame). They take ``normalize``, ``sort``,
``ascending`` and ``dropna`` (and ``bins`` on a Series, the counts per interval of
``pandas.cut`` with every interval listed, labelled by its text such as
``"(0.999, 2.0]"``; the bins of an interval label are text, not an interval
object). Ties keep the order of first appearance, ``sort=False`` lists the values
by first appearance, and ``dropna=False`` counts a null and a NaN as one missing
value. Against a pandas nullable dtype (``Int64``, ``Float64``) the null sits at
its first appearance among tied counts, where pandas puts it last; pandas 2 orders
the tied rows of a frame and the rows with a null key in its own way, and a
pandas 3 ``str`` column gives a pyarrow-backed count. ``Expr.value_counts`` keeps
its Polars struct and the lazy group-by keeps its frame.
The labels a group-by gives its results are in "Levels" below.

Levels
~~~~~~

The levels of a row index (a name, a position, a negative position, or a list
of them) are edited as in pandas 2 and 3, on a Series and a DataFrame, by
changing the list of level names and the label columns: no row moves except in
``sort_index``.

- ``droplevel(level)`` removes levels (a frame's label columns are gone, not
  data) and raises ``ValueError`` when none would be left, ``KeyError`` for an
  unknown name and ``IndexError`` for a position out of range. ``swaplevel(i, j)``
  (the last two by default) and ``reorder_levels(order)`` need a MultiIndex;
  pandas raises ``AssertionError`` or a bare ``Exception`` there for some
  inputs, and this raises ``TypeError`` or ``ValueError``. ``axis=1`` is the
  column labels, which have one level.
- ``rename_axis(["a", "b"])`` names every level, ``rename_axis(index={"a": "x"})``
  and ``rename_axis(index=func)`` map the names, and a dict or a function as the
  first argument is a ``ValueError`` as in pandas. A frame with no index gains
  its positions as the index under the name. A frame cannot hold two columns of
  one name, so a level name that is already a column, or a repeated level name,
  is a ``ValueError`` (pandas allows both); the name of the column axis
  (``columns=``) is not kept and raises ``NotImplementedError`` unless it is
  ``None``.
- ``df.index`` and ``s.index`` (a Series for one level, a frame for several)
  have the pandas Index names ``names``, ``nlevels``, ``get_level_values(level)``
  and ``set_names(names, level=)``; they read the levels of the object that
  ``index`` returned, and exist on every Series and DataFrame for that reason.
- ``add_prefix`` and ``add_suffix`` of a Series give every row label as text with
  the prefix (each level of a MultiIndex; a positional Series gets the
  positions); numbers, Bool and text go through the native string kernels. A DataFrame names the columns (``axis=1``, the default) or, with
  ``axis=0``, does the same to the row labels. A null label prints as pandas 3
  prints it (``nan``, ``NaT``).
- ``set_index(keys, append=True)`` adds the columns as levels after the current
  ones, and a frame with no index keeps its positions as an unnamed first level;
  a key that is already a level is a ``KeyError``. Without ``append`` the old index
  columns are dropped, as pandas does (``df.set_index("a").set_index("b")`` keeps
  neither as a column). ``reset_index(level=...)``
  moves some levels back into columns, in front of the others and in level
  order, named as pandas names them (``index`` or ``level_<i>`` for an unnamed
  one); with ``drop=True`` they are removed. ``sort_index(level=...,
  sort_remaining=True)`` orders by those levels first and then by the rest;
  ``ascending`` is one flag or one for each level in ``level`` (a list sorts the
  listed levels only, as pandas), ``na_position``, ``ignore_index`` and ``key`` (a
  function of each level) work as in pandas, and the sort is stable. A single
  level ignores ``level``, as pandas.

``groupby(keys)["column"]`` is the pandas ``SeriesGroupBy``: its reductions
(``sum``, ``mean``, ``min``, ``max``, ``count``, ``first``, ``last``, ``median``,
``nunique`` and the rest), ``agg`` with one name, and its transforms (``cumsum``,
``shift``, ``rank``, ``head`` and the rest) give a Series named by the column,
under the group keys or the row labels, where ``groupby(keys)[["column"]]``, a
list of aggregate names and ``as_index=False`` give a frame.
``groupby(...).rolling(n)``, ``expanding()`` and ``ewm(alpha)`` give the rows in
group order under the ``(key, row)`` index, as pandas; the Polars ``group_by`` keeps
the input order and no index.

The group-by results that pandas labels with the group keys in front
(``group_keys=True``) are labelled the same way by the pandas ``groupby``:
``apply`` that returns a frame or a Series, ``take``, ``SeriesGroupBy.nlargest``
and ``nsmallest`` (now a Series named like the column, where the Polars-shaped
frame with an ``index`` column was) and ``value_counts`` (a Series named
``count``). Each row is labelled by its group key, then by the label it had (its
index, or its position when the frame has none), and the groups come in key order
(or first appearance with ``sort=False``). As pandas 3, the group function gets
the frame without the key columns (pandas 2 passes them with a warning); a result
column that is named like a key is a ``ValueError``. Series results that all have
the same labels become one row for each group (columns are the labels, as pandas),
and a scalar or a list per group is a Series with no name under the keys, as pandas 3 (the traced reduction of several columns, ``g[["a", "b"]].sum()``, is a frame with those columns under the keys). With
``as_index=False`` the keys are left out and the rows keep their labels. The Polars
``group_by`` keeps the old shapes: key columns present and rows in input order
for ``take``. ``take`` raises ``IndexError`` when a group is too small, as
pandas; ``value_counts`` with ``sort=False`` orders the combinations by count, ties in
order of first appearance, as pandas; the lazy group-by is unchanged.

.. code-block:: python

   df = DataFrame({"g": ["a", "a", "b"], "n": [1, 2, 1], "v": [1.0, 2.0, 3.0]}).set_index(["g", "n"])
   df.swaplevel()                      # levels n, g
   df.droplevel("g")                   # the index n alone
   df.reset_index(level="g")           # g back as a column, n stays the index
   df.sort_index(level="n", ascending=False)
   df.reset_index().groupby("g")["v"].nlargest(1)   # Series under (g, position)

Reshape
~~~~~~~

``stack`` and ``unstack`` move columns into the row labels and back, on the
native sort, take, unpivot and pivot.

- ``df.stack()`` makes the columns the innermost index level of a Series, row by
  row (each row's columns in column order), under the existing levels (or the
  positions for a frame with no index). The default is pandas 3's implementation
  (``future_stack=True``): every cell is kept, null ones too, and ``dropna`` and
  ``sort`` raise ``ValueError``. ``future_stack=False`` drops null cells unless
  ``dropna=False``. Pandas 2 defaults to ``future_stack=False``, so pass the
  keyword to get the same on both. The columns must share a type or all be
  numeric (pandas gives an object Series for text and numbers; this raises).
  A frame has flat column names, so ``level`` is 0, -1 or ``[0]``, and the result
  is never a frame.
- ``s.unstack(level=-1, fill_value=None, sort=True)`` moves a level (a name, a
  position, a negative position or a list) into the columns. The other levels are
  the sorted index; each label of the level is a column, named with its text (the
  text of a tuple for a list of levels, which come in order of first appearance).
  A cell the Series has no value for is null, or ``fill_value`` (a null the Series
  itself holds stays null; text takes a text fill and numbers a number).
  ``sort=False`` keeps the order of first appearance, as pandas 3.1 does (earlier
  pandas returns values under the wrong labels). A label pair that occurs twice
  is ``ValueError("Index contains duplicate entries, cannot reshape")`` and a
  Series with fewer than two levels is a ``ValueError`` too.
- ``df.unstack(...)`` does the same for each column of a frame with a MultiIndex.
  A frame cannot hold two-level column names, so a column is named with the text
  of the tuple ``(column, label)``, the name ``from_pandas`` gives a MultiIndex
  column. A frame with one index level (or none), or with every level named,
  gives a Series labelled by the column name and the old labels, as pandas.
- ``unstack`` is the pandas method. The Polars ``DataFrame.unstack(step, how,
  columns, fill_values)``, which cuts a column into a frame of ``step`` rows, is
  not offered under that name; the coverage check cannot list a name that exists
  as excluded.
- ``fill_value`` takes a number for number columns, a string for text and a
  ``bool`` for Bool columns; any other kind is a ``TypeError`` (pandas mixes the
  kinds in an object column). A frame whose columns are all null next to numbers
  is stacked as pandas stacks it, the null column taking the numbers' type.
- The name of the column level is lost: ``s.unstack().stack()`` has an unnamed
  last level where pandas keeps the level's name.
- The pandas ``groupby(keys).size()`` is a Series under the keys, so
  ``df.groupby(["a", "b"]).size().unstack()`` works as in pandas. A reduction of
  one column, ``g["c"].sum()``, is still a one-column frame: use
  ``g["c"].sum()["c"].unstack()``.

.. code-block:: python

   df = DataFrame({"a": [1, 1, 2], "b": ["x", "y", "x"], "v": [1.0, 2.0, 3.0]})
   counts = df.groupby(["a", "b"]).size().unstack(fill_value=0)   # columns x, y
   wide = df.set_index(["a", "b"])["v"].unstack("b")              # a is the index
   long = wide.stack()                                           # (a, column) -> v

Time on the index
~~~~~~~~~~~~~~~~~

``resample``, ``asfreq``, ``at_time``, ``between_time``, ``tz_localize`` and
``tz_convert`` act on a Timestamp index level, as on a pandas ``DatetimeIndex``,
for a ``DataFrame`` and a ``Series``. A MultiIndex takes ``level=`` (a name, a
position or a negative position), and ``resample`` takes ``on=`` for a column;
an index that is not a Timestamp (a default index, integers, several levels
with no ``level=``) is a ``TypeError``, as pandas.

``resample(rule, closed, label, on, level, origin, offset)`` takes a fixed width
(``"5s"``, ``"30min"``, ``"2h"``, ``"1D"``, any count of them) and a calendar
rule (``"W"``, ``"W-WED"``, ``"ME"``, ``"MS"``, ``"QE"``, ``"QS"``, ``"YE"``,
``"YS"``, with a count: ``"2ME"``), with the pandas defaults for ``closed`` and
``label`` (right for ``W``, ``ME``, ``QE``, ``YE``) and for ``origin``
(``start_day``; ``epoch``, ``start``, ``end``, ``end_day`` and a timestamp also
work) and ``offset``. The result has one row per bin from the first to the last,
so a bin with no row is listed: ``sum``, ``count``, ``size`` and ``nunique`` give
0 there, ``prod`` 1 and the rest null. It is indexed by the bin label, and the
index name is that of the index. The aggregations are ``sum``, ``prod``, ``mean``,
``median``, ``min``, ``max``, ``count``, ``size``, ``first``, ``last``, ``std``,
``var``, ``nunique``, ``ohlc`` and ``agg`` (a name, a list, a dict or
``out=(column, name)``), and ``r["a"]`` and ``r[["a", "b"]]`` pick columns. Each is
one group-by over the bin number, joined onto the grid of bins. Not done: a
function in place of a name, ``ddof`` other than 1, ``apply``, ``ffill`` and
``interpolate`` of the resampler, ``B`` and the other business-day rules, and a
Timedelta or Period index. ``ohlc`` of a frame names its columns ``<column>_open``,
``<column>_high``, ``<column>_low`` and ``<column>_close`` where pandas has two column
levels, and a list of names in ``agg`` of a frame likewise gives ``<column>_<name>``.
A frame with an Int64 index or ``on=`` column (DFTracer timestamps) still takes the
old form: ``unit="us"``, ``group_by_dynamic`` with fixed left-closed bins.

``asfreq(freq, method, how, normalize, fill_value)`` is a reindex onto
``date_range(first, last, freq)`` by the same label lookup as ``reindex``: a grid
instant takes the row at exactly that time, ``method="ffill"`` / ``"bfill"`` and
``fill_value`` fill only the rows the grid adds (a row that is there keeps its own
nulls), and ``normalize`` moves the labels to midnight. The index must hold each
instant once.

``at_time(time)`` and ``between_time(start, end, inclusive)`` take a
``datetime.time`` or text (``"09:30"``, ``"9:30:15.5"``, ``"9:30AM"``) and keep the
index; they compare at microseconds as pandas does, so an instant is cut to
microseconds first (a time of ``09:30:00.0000009`` is ``09:30:00``); an end before the start wraps past midnight. They compute the time of day
from the ticks with integer arithmetic, so no new kernel.

``tz_localize`` and ``tz_convert`` check the zone at the call with the same error
kinds as pandas (``KeyError`` for an unknown name, ``IndexError`` for an empty one,
``ValueError`` for ``"UTC+1"``), and take every spelling of UTC that pandas
treats as zero offset (``UCT``, ``Etc/GMT``, ``Zulu``, ``Greenwich`` and the
rest). The engine reads the zone files from the directories of :doc:`/environment`
(``DFTRACER_UTILS_TZDIR``), after the ``zoneinfo.TZPATH`` directories and the
``tzdata`` package that Python gives it.
``tz_localize`` reads a naive index as wall times in the zone, and ``None``
turns an aware index into its wall times; ``tz_convert`` changes only the zone
the type shows, no instant moves. ``ambiguous`` (``"raise"``, ``"infer"``,
``"NaT"`` or a boolean array) and ``nonexistent`` (``"raise"``,
``"shift_forward"``, ``"shift_backward"``, ``"NaT"`` or a ``timedelta``) say
what a wall time that happens twice or never becomes. Every ``.dt`` part,
``strftime`` (``%z`` the offset, ``%Z`` the abbreviation), ``floor``,
``ceil``, ``round``, ``normalize``, ``date`` and ``time`` of an aware column
read the wall time in its zone, and so do ``resample``, ``asfreq``,
``at_time`` and ``between_time``: a day of a zone with daylight saving time is
23 or 25 hours long, as pandas. A naive column and a UTC column keep the
direct path.

``dt.replace_time_zone(time_zone, ambiguous=, non_existent=)`` and
``dt.convert_time_zone(time_zone)`` are the polars names of ``tz_localize`` and
``tz_convert``. ``ambiguous`` takes ``"raise"``, ``"earliest"``, ``"latest"``
or ``"null"``, and ``non_existent`` takes ``"raise"`` or ``"null"``, as polars.

Two differences from pandas, kept on purpose. ``nonexistent="shift_forward"``
and ``"shift_backward"`` give the instant of the change (the first instant
after the gap, and one tick before it): pandas rounds to the next or previous
whole hour, which differs for a gap that does not end on the hour
(``Australia/Lord_Howe``, ``Pacific/Chatham``). A fixed offset such as
``"+0530"`` is +05:30; pandas reads it as +05:00.
``floor``, ``ceil`` and ``round`` of an aware column fail with ``ValueError``
when the rounded wall time happens twice or never, as pandas does.

.. code-block:: python

   ts = df.set_index("time")
   ts.resample("1h").sum()                           # empty hours are 0
   ts["bytes"].resample("5min", label="right").ohlc()
   ts.resample("ME").agg(total=("bytes", "sum"))
   ts.between_time("09:00", "17:00", inclusive="left")
   ts.asfreq("1min", method="ffill")

Several columns can be assigned at once from a frame or a list of Series. The
targets and the value's columns pair by position (the value's own names are
ignored, as in pandas), a target that does not exist is added, and a count or
row mismatch is a ``ValueError`` that names it before any column changes.

.. code-block:: python

   df[["a", "b"]] = other[["x", "y"]]          # a gets x, b gets y
   df[["p", "q"]] = [s1, s2]                   # one Series per target
   df.loc[mask, ["a", "b"]] = selected         # the selected rows only

Replace
-------

``Series.replace`` and ``DataFrame.replace`` take pandas' forms: ``replace(old,
new)``, ``replace([old, ...], new)``, ``replace([old, ...], [new, ...])`` (paired
by position) and ``replace({old: new, ...})``. Every match is found on the
original values and applied together, so ``{1: 2, 2: 1}`` swaps. A new value of
``None`` or ``pd.NA`` makes the value null and keeps the column type; a NaN
makes it NaN. An old ``None`` or ``pd.NA`` matches the nulls and an old NaN the
NaN values (a null is not a NaN; pass both to catch both). The column keeps its
type: a float into an integer column widens it, a string needs a string column
and a number a numeric one (``TypeError`` otherwise). An old value the column
type cannot hold matches nothing. Bool, list and temporal columns raise on a
Series and are left alone by the frame form.

No form is ever ignored: a call changes the values it names, leaves a column alone
because no old value can match its type (or it is a bool, list or temporal column),
or raises an error that names the problem.

.. code-block:: python

   s.replace([float("inf"), float("-inf")], pd.NA)   # infinities to null
   df.replace({0: None})                             # zero to null in every column that can hold it

Formulas as text: ``eval``
--------------------------

``df.eval(expr)`` evaluates Python expressions over the columns, lowered by the
same code as the source tier of ``apply`` and never run as Python code: the text is
parsed with ``ast``, a bare name is a column or a ``KeyError``, and a construct with
no engine form raises ``TranspileError`` naming it. ``df.eval("a / b")`` gives a
Series; ``df.eval("m = a / b")`` gives a new frame with ``m`` added or replaced.
Several lines run in order, each seeing the columns the lines before it made (with
their types), every line but the last an assignment; a last expression line returns
its value.

pandas' operator rule applies: ``&`` and ``|`` mean ``and`` and ``or``, so a comparison
needs no parentheses (``a > 1 & b < 2`` is ``(a > 1) and (b < 2)``, where plain Python
reads ``a > (1 & b) < 2``), and ``&`` binds tighter than ``|``. ``~`` negates a mask. The
pandas ``.str`` accessor spelling works: ``func_name.str.contains("close|open")`` is a
regex search, as in pandas.

Supported: column names; number literals; ``+ - * /`` with a number on either side;
``//`` and ``%`` (integer columns give integers, as Python does); ``**`` with a
constant exponent from 0 to 8 or 0.5; unary ``- +``; ``< <= > >= == !=`` (chains
too); ``and``, ``or``, ``not``; ``x if c else y``; ``in`` and ``not in`` a list of
constants; ``x is None``; ``abs``, ``min``, ``max``, ``int``, ``float``, ``round``
(whole numbers), ``len``; ``math.sqrt``, ``log``, ``exp``, ``floor``, ``ceil``; the
string methods ``lower``, ``upper``, ``strip``, ``lstrip``, ``rstrip``,
``startswith``, ``endswith``, ``replace`` and ``contains``; and ``x.isna()``,
``isnull()``, ``notna()``, ``notnull()``. Nulls propagate as the column operators
propagate them (``a > 1`` is null where ``a`` is), where pandas compares NaN as
false; ``(x.isna() | x == 0)`` is true where ``x`` is null, as in pandas.

Not supported, each with an error that names it: ``@name`` locals and ``@`` matrix
products, backtick column names, ``inplace``, bitwise ``&`` ``|`` ``~`` on integer
columns, attribute access, subscripts, lambdas, comprehensions, and string methods
beyond the list.

.. code-block:: python

   df.eval("m = a / b\nn = m * 100\nn + c")      # three lines, one result
   df.eval("io_cat == 3 and func_name.str.contains('close') and ~func_name.str.contains('dir')")
   df.eval("(x.isna() | x == 0) & (y.isna() | y == 0)")

Left out, and why
------------------

- ``to_period``: a Period is a span, not an instant; ``dt.floor``,
  ``resample`` and the calendar parts give the same buckets.
- ``dt.strftime`` takes the directives of ``Series.dt_format`` only. Other
  directives raise ``ValueError``.
- ``MultiIndex`` methods, ``align`` / ``reindex``: they follow the row index
  (``Series`` labels exist, the lookup and reindex methods do not yet).
- Plotting, styling, ``attrs`` / ``flags``, the file writers beyond
  Parquet / CSV / IPC: not this engine's layer.
- ``GroupBy.fillna(value)``: pandas deprecates it; ``ffill`` / ``bfill``
  are the spellings.

Which names exist
------------------

``scripts/api_coverage.py`` compares the public names of the installed
pandas and Polars classes with ours and fails on a name that is neither ours
nor in ``scripts/api_coverage_exclude.txt``. Each line of that file gives one
reason: ``out-of-scope``, ``python-semantics``, ``needs-kernel`` (names the
missing engine kernel or dtype) or ``deprecated-upstream``. ``scripts/api_coverage.py --report`` prints the
table per class. ``tests/python/test_pandas_parity.py`` and
``tests/python/test_polars_parity.py`` run each method we have in the library
and in ours on the same data; the methods whose result differs are listed with
their reasons in ``tests/python/parity_findings_pandas.txt`` and
``parity_findings_polars.txt``. A line there is one case
(``kind.method#N[input]``) and the case must fail: a case that is not listed
must match the library exactly (NaN and null are different values, a bool is
not an int, the dtype is compared with its width and unit), so a fixed case
fails the run until its line is deleted.

A method that both libraries spell the same way but give another result is
one method of ours, and it follows pandas on a ``Series`` and ``DataFrame``
and Polars on an ``Expr``. ``Expr.quantile`` defaults to ``nearest`` as
Polars does, ``Expr.percentile`` to ``linear`` as numpy, and ``Expr.str.find``
gives null for no match. The cases are listed as findings: ``Series.quantile``
defaults to ``linear`` (Polars ``nearest``; pass ``interpolation``),
``Series.str.find`` gives -1 for no match (Polars null), ``rank``, ``ge``, ``gt``
and the comparison operators treat a NaN as IEEE does (Polars ranks it above
every number), and a float NaN is a value and not a null where pandas reads it
as missing.

The quantile of a group is exact: ``col("x").quantile(q, interpolation)``,
``percentile(p)`` and ``median()`` keep every present value of the group, 8
bytes each, merge partial states by concatenation, and spill across groups under
the aggregation memory budget; one group's values must fit in memory when the
group is finalized. ``approx_quantile(q)`` is the bounded-memory DDSketch
estimate. A reduction over no rows with no key is one row (``sum`` 0, the rest
null).

See also
---------

- :doc:`dataframe` and :doc:`series` for the engine-first API.
- :doc:`time-windows` for the window functions and time buckets.
- :doc:`../core/columnar-ops` for the op catalog.
