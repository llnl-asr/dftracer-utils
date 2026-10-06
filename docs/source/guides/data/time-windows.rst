:description: Bucket rows by a timestamp and aggregate each window with group_by_dynamic on a frame or time_bucket straight out of a trace scan.

Roll up over time windows
=========================

Bucket rows by a timestamp column and aggregate each window: events per
millisecond, mean duration per second, a counter series for a timeline. There
are two entry points - ``group_by_dynamic`` on a frame you already hold, and the
trace query's ``time_bucket`` when you want to bucket events straight out of a
scan.

Window a DataFrame you have
---------------------------

``group_by_dynamic`` tumbles (or slides) fixed windows over an **ascending
Int64** time column. Windows start at the first timestamp floored to a multiple
of ``every``, stride by ``every``, and each covers ``[start, start + period)``.
A ``period`` of ``0`` (or any value ``<= 0``) means "same as ``every``", so the
windows tile without overlap; a ``period`` larger than ``every`` makes them
overlap. Each non-empty window emits one row: a leading window-start column
named after the time column, then one column per aggregate.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/dataframe/dataframe.h>

         using namespace dftracer::utils::dataframe;

         DataFrame windows = df.group_by_dynamic(
             "ts", /*every=*/1'000'000, /*period=*/0,
             {{Agg::Count, "", "n"}, {Agg::Mean, "dur", "mean_dur"}});

      ``GroupAgg`` is ``{op, column, out}``; ``op`` is an ``Agg`` enumerator:
      ``Agg::Count`` (column ignored), ``Agg::Sum``, ``Agg::Min``,
      ``Agg::Max``, ``Agg::Mean``, and the higher moments ``Agg::Var`` /
      ``Agg::Std`` / ``Agg::Skew`` / ``Agg::Kurt`` (``Agg::First`` /
      ``Agg::Last`` are pivot-only and rejected here).

   .. tab-item:: Python

      .. code-block:: python

         windows = df.group_by_dynamic("ts", every=1_000_000, period=None,
                                        aggs=["count", "mean:dur"])

      ``aggs`` entries are strings: ``"count"``, or ``"<op>:<column>"`` with
      ``op`` in ``sum`` / ``min`` / ``max`` / ``mean`` / ``var`` / ``std`` /
      ``skew`` / ``kurt`` (the output column is named ``<op>_<column>``, e.g.
      ``mean_dur``). ``period=None`` is treated as ``0`` (tumbling).

   .. tab-item:: C

      .. code-block:: c

         #include <dftracer/utils/dataframe/abi.h>

         dftu_group_agg aggs[] = {
             {"count", NULL, "n"},
             {"mean", "dur", "mean_dur"},
         };
         dftu_dataframe* windows = dftu_dataframe_group_by_dynamic(
             df, "ts", /*every=*/1000000, /*period=*/0, aggs, 2);

      ``op`` is the same lowercase name the C++ ``Agg`` enumerators have
      (``"sum"``, ``"min"``, ``"max"``, ``"count"``, ``"mean"``, ``"var"``,
      ``"std"``, ``"skew"``, ``"kurt"``). The header comment on
      ``dftu_group_agg`` lists only the first five. An unknown name returns
      ``NULL``.

The result of a 1-second (``every = 1'000'000`` us) tumbling window over a trace
is one row per second of wall time, each with the event count and mean duration
in that second.

Bucket events straight from a trace
-----------------------------------

To bucket events as they come out of a scan - without materializing every event
first - add ``time_bucket`` (microseconds) to a trace query. Combined with a
``group_by``/``agg``, this is the counter-timeline rollup: one aggregated row per
(group, bucket).

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>

         using namespace dftracer::utils::trace::views;

         auto timeline = View::from_file("trace.pfw.gz")
                             .group_by({GroupKey::cat()})
                             .time_bucket(1000)   // 1 ms buckets
                             .agg({{AggOp::Count, "", "n"}})
                             .collect()   // -> coro::CoroTask<DataFrame>
                             .get();

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         timeline = (TraceViewer("traces/")
                     .group_by("cat")
                     .time_bucket(1000)   # 1 ms buckets
                     .agg("count")
                     .collect())

For traces whose timestamps are not in microseconds, apply ``time_scale`` before
``time_bucket`` to normalize the unit. To write the bucketed rollup out as a
re-indexable ``ph="C"`` counter trace rather than collecting it, use the counter
export described in :doc:`../analysis/aggregation`.

Rank rows and look at neighbors: ``window``
-------------------------------------------

``window`` runs SQL window functions (PARTITION BY / ORDER BY) over a frame,
taking a ``DataFrame`` and returning a ``DataFrame``. ``specs`` is a list of
tuples, one per appended output column; every input column passes through, then
one column per spec, in sorted (partition, order) row order.

.. code-block:: python

   ranked = df.window(
       partition_by=["pid"],
       order_by=["ts"],
       specs=[
           ("row_number", "rn"),
           ("lag", "dur", 1, "prev_dur"),
           ("running_sum", "dur", "cum_dur"),
       ],
   )

Supported spec shapes:

- ``("row_number"|"rank"|"dense_rank", out)``
- ``("lag"|"lead", value_col, offset, out)``
- ``("running_sum"|"running_min"|"running_max"|"running_count"|"running_prod", value_col, out)``
  (``running_prod`` is Float64; ``running_sum`` over an integer column is
  exact, Int64, or UInt64 over an unsigned one, and a sum outside the type is
  an error, not a wrapped number)
- ``("delta", value_col, out)`` (over an integer column the exact Int64
  difference; one outside Int64 is an error)
- ``("rate", value_col, time_col, out[, counter])``
- ``("sessionize", time_col, gap, out[, end_col[, span]])``: the 1-based
  session of each row. A row starts a new session when its time is more than
  ``gap`` after the latest end of its session so far (``end_col``, else the
  time), or more than ``span`` after the session's first time; a null time
  is a null session.
- ``("frame_sum"|"frame_min"|"frame_max"|"frame_count"|"frame_mean", value_col, preceding, following, out[, min_periods])``
  (a bound of ``None`` means that side of the ROWS frame runs to the partition edge;
  the output is null while the frame holds fewer than ``min_periods`` present values;
  ``frame_sum`` over an integer column is exact like ``running_sum``)
- ``("frame_var"|"frame_std"|"frame_count_distinct"|"frame_collect", value_col, preceding, following, out[, min_periods[, mode]])``,
  ``("frame_quantile", value_col, preceding, following, out, min_periods, mode, q)`` and
  ``("frame_arg_max"|"frame_arg_min", value_col, preceding, following, out, min_periods, mode, by)``
  (``mode`` is ``"rows"`` or ``"range"``; ``q`` is the quantile level in [0, 1], and
  the quantile is exact with linear interpolation; ``by`` is the ordering column of
  the arg functions, which give the ``value_col`` of the frame row with the
  largest or smallest ``by``, the earliest row on a tie. Nulls are skipped.
  ``frame_var`` and ``frame_std`` are null with fewer than 2 values and 0 on a
  constant frame. ``frame_collect`` gives the list of the frame values in sort
  order, and an empty list for an empty frame. ``frame_count_distinct`` gives 0
  for an empty frame. ``min_periods`` does not apply to ``frame_count_distinct``
  and ``frame_collect``, and counts the present ``by`` rows for the arg
  functions. A ``frame_collect`` that passes 2^27 values in total is an error)
- ``("ntile", n, out)``
- ``("first_value"|"last_value"|"fill_forward", value_col, out)``
  (``fill_forward`` is the nearest present value at or before the row: a group-wise ``ffill``)
- ``("nth_value", value_col, k, out)``

A single-column running or ranked series that needs no partitioning is often
simpler as a :doc:`Series <series>` method (``rank``, ``cumsum``, ``rolling``,
``shift``, ``diff``) on ``df["dur"]``.

Fill a regular time grid: ``gap_fill``
--------------------------------------

``gap_fill`` materializes a regular time grid of width ``bucket`` over the
integer ``time`` column, per partition, filling the ``values`` columns on the
generated rows. It takes a ``DataFrame`` and returns a ``DataFrame``.

.. code-block:: python

   filled = df.gap_fill(
       partition_by=["pid"],
       time="ts",
       bucket=10,
       values="dur",
       mode="locf",
   )

``values`` (a name or list of names) is filled per ``mode``: ``"none"`` leaves
them null, ``"locf"`` carries the last real value forward, ``"linear"``
interpolates (emitting the value columns as double). Pass both ``start`` and
``end`` to set an explicit grid range for every partition, or neither to derive
it from the data. A bad ``mode`` raises ``ValueError``; a missing column raises
``KeyError``.

See also
--------

- :doc:`../analysis/aggregation` for the full trace ``group_by``/``agg`` surface
  and counter export.
- :doc:`dataframe` for the other frame ops and the generated ``window`` /
  ``gap_fill`` reference.
- :doc:`series` for the single-column running/rolling/rank primitives.
- :doc:`../../cpp_api/dataframe` for the full member and C ABI reference.
