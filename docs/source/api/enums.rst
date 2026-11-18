:description: Typed str-enum vocabularies for the TraceViewer builder (Phase, GroupKey, AggOp) and the reader's TimeUnit scaling.

Enums and Time Units
====================

Typed vocabularies for the :doc:`trace_viewer` builder API and the reader's
time-unit scaling. Each is a ``str`` enum, so a member is interchangeable with
its string value: ``phase(Phase.EVENTS)`` is the same call as
``phase("events")``, and ``time_unit=TimeUnit.US`` is the same as
``time_unit="us"``. The enums exist so callers get a discoverable, typo-safe
set instead of bare strings; the string form remains valid everywhere.

All four are exported at the package top level (``dftracer.utils.Phase``, and
so on).

Builder vocabularies
--------------------

.. autoclass:: dftracer.utils.Phase
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.GroupKey
   :members:
   :undoc-members:

``GroupKey.arg("epoch")`` builds an ``"arg:epoch"`` dimension for grouping on an
entry of the event args map, which has no fixed member.

.. autoclass:: dftracer.utils.AggOp
   :members:
   :undoc-members:

``AggOp.of`` builds the ``"op:field"`` spec a view's ``agg`` accepts:
``AggOp.STD.of("dur")`` is ``"std:dur"``, and ``AggOp.ARGMAX.of("name",
by="dur")`` is ``"argmax:name:dur"`` (the row with the maximum ``dur``, keyed by
``name``). ``AggOp.COUNT`` needs no field.

Series dtypes
-------------

.. autoclass:: dftracer.utils.DType
   :members:
   :undoc-members:

``DType`` is an ``IntEnum`` that mirrors the engine's type ids; it is also
exported at the package top level. ``is_integer``, ``is_float`` and
``is_numeric`` classify a type, and ``int_range`` gives the ``(min, max)`` of an
integer type (``None`` for the others). ``dftracer.utils.enums`` is the one
table for dtypes and time units; the other modules derive their lookups from it.

Time units
----------

.. autoclass:: dftracer.utils.TimeUnit
   :members:
   :undoc-members:

A trace declares its native ``ts``/``dur`` unit through a leading ``CM``
time_metric metadata event, and different runs can emit different units. Pass a
``TimeUnit`` (or its string, or ``None`` for native, no scaling) where a reader
or ``TraceViewer`` accepts ``time_unit``; the reader scales from the trace's
native unit to the target.

See also
--------

- :doc:`trace_viewer` for the builder ops (``phase``/``group_by``/``agg``) that
  consume these vocabularies.
- :doc:`../guides/analysis/aggregation` for the aggregation how-to.
