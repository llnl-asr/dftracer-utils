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

See also
---------

- :doc:`dataframe` and :doc:`series` for the engine-first API.
- :doc:`time-windows` for the window functions and time buckets.
- :doc:`../core/columnar-ops` for the op catalog.
