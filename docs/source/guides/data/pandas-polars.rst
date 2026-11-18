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
     - ``aggregate`` = ``agg``, ``astype`` = ``cast``, ``isna`` = ``is_null``, ``isnull`` = ``is_null``, ``notna`` = ``is_not_null``, ``notnull`` = ``is_not_null``, ``dropna`` = ``drop_nulls``, ``fillna`` = ``fill_null``, ``isin`` = ``is_in``, ``nunique`` = ``n_unique``, ``prod`` = ``product``, ``argmax`` = ``arg_max``, ``idxmax`` = ``arg_max``, ``argmin`` = ``arg_min``, ``idxmin`` = ``arg_min``, ``cumsum`` = ``cum_sum``, ``cumprod`` = ``cum_prod``, ``cummax`` = ``cum_max``, ``cummin`` = ``cum_min``, ``searchsorted`` = ``search_sorted``, ``tolist`` = ``to_list``, ``ffill`` = ``forward_fill``, ``pad`` = ``forward_fill``, ``bfill`` = ``backward_fill``, ``backfill`` = ``backward_fill``, ``kurt`` (own signature) = ``kurtosis``, ``take`` = ``gather``, ``iat`` = ``iloc``, ``at`` = ``iloc``, ``loc`` = ``iloc``, ``rtruediv`` = ``rdiv``, ``divide`` = ``truediv``, ``div`` = ``truediv``, ``multiply`` = ``mul``, ``subtract`` = ``sub``, ``sort_values`` (own signature) = ``sort``, ``nlargest`` (own signature) = ``top_k``, ``nsmallest`` (own signature) = ``bottom_k``, ``drop_duplicates`` (own signature) = ``unique``, ``between`` (own signature) = ``is_between``
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
   * - ``LazyGroupBy``
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
for seconds and milliseconds, Time64 for microseconds and nanoseconds), both in
UTC like every ``.dt`` part. ``dt.timetz`` is ``dt.time``: a time-of-day column
has no zone to carry. ``col("v").arg_max()`` and ``arg_min()`` give the position
within the group of the first extreme present value (null for a group with
none); they need the rows in input order, so ``TraceViewer.agg`` refuses them.
``groupby(k)["v"].nlargest(n)`` and ``nsmallest(n)`` give the keys, the row
position as ``index`` and the value (``keep="first"`` only); ``unique()`` gives
a List column of the first-seen distinct values, ``is_monotonic_increasing`` and
``is_monotonic_decreasing`` a Bool column, all in key order. ``groupby(k)
.corrwith(other)`` correlates each column with the paired column of ``other``
(rows pair by position, ``method="pearson"`` only).

Removed: ``ColumnExpr`` and ``Field`` (use ``Expr`` and ``col``),
``TraceViewer.query`` and ``explain_query`` (use ``duql`` and ``explain_duql``),
``TraceViewer.limit`` (use ``head``; ``DataFrame.limit`` and
``LazyFrame.limit`` stay as the polars alias of ``head``) and the native
``Series.cast(type_id)`` (``cast`` takes the dtype).

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
- ``tz_localize`` with a non-UTC zone on a naive column: a wall-time shift
  needs a zone database the engine does not have.
- ``dt.strftime`` takes the directives of ``Series.dt_format`` only, and
  formats in UTC. Other directives raise ``ValueError``.
- ``MultiIndex`` methods, ``align`` / ``reindex``: label alignment was ruled
  out with the index model above.
- Plotting, styling, ``attrs`` / ``flags``, the file writers beyond
  Parquet / CSV / IPC: not this engine's layer.
- ``GroupBy.fillna(value)``: pandas deprecates it; ``ffill`` / ``bfill``
  are the spellings.

Which names exist
------------------

``scripts/api_coverage.py`` compares the public names of the installed
pandas and Polars classes with ours and fails on a name that is neither ours
nor in ``scripts/api_coverage_exclude.txt``. Each line of that file gives one
reason: ``out-of-scope``, ``python-semantics``, ``needs-index`` (waits for the
row index), ``needs-kernel`` (names the missing engine kernel) or
``deprecated-upstream``. ``scripts/api_coverage.py --report`` prints the
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
