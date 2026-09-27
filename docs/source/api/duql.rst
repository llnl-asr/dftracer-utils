:description: Reference for the Python duql builder: build Expr predicates from F/Field, combine with & | ~, and render the duql string TraceViewer scans with.

duql
====

A small expression builder for duql, the trace filter language. ``F`` (or the
back-compat spelling ``Field``) references an event field (including dotted
paths for nested JSON, e.g. ``args.level``), and its operators build ``Expr``
objects that combine with ``&`` (and), ``|`` (or), and ``~`` (not).
``str(expr)`` / ``expr.to_duql()`` render the duql string consumed by the
index and the ``TraceViewer`` scan path.

``F`` and ``Field`` are the same objects exported from
``dftracer.utils`` and ``dftracer.utils.duql``. ``F`` is the single unified
builder: the same field leaf that builds these predicates also builds columnar
value expressions and evaluates them in memory with ``.apply()`` (see
:doc:`columnar`).

.. code-block:: python

   from dftracer.utils import F, Field

   cat = Field("cat")
   dur = F.dur                                 # F.<name> is shorthand for Field("<name>")

   q = (cat == "POSIX") & (dur > 1000)       # AND
   q = (cat == "POSIX") | (cat == "STDIO")   # OR
   q = ~(cat == "POSIX")                      # NOT
   q = cat.is_in(["POSIX", "STDIO"])          # membership
   q = cat.not_in(["MPI"])

   level = F("args.level")                     # nested/non-identifier field: call or subscript
   q = level == "DEBUG"

   q = F.name.like("%read%")                   # SQL LIKE
   q = F.name.regex("^p?read$")                 # duql regex

   query_string = str(q)                      # render to string

Operators
---------

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Expression
     - Meaning
   * - ``field == v`` / ``field != v``
     - equality / inequality
   * - ``field > v`` / ``field < v`` / ``field >= v`` / ``field <= v``
     - ordered comparison
   * - ``field.is_in([...])`` / ``field.not_in([...])``
     - membership / exclusion
   * - ``field.like(pattern)`` / ``field.ilike(pattern)``
     - SQL LIKE (``%`` any run, ``_`` one character) / ASCII case-insensitive
   * - ``field.regex(pattern)`` / ``field.iregex(pattern)``
     - duql regex search / case-insensitive
   * - ``field.contains(sub)``
     - unanchored substring search
   * - ``a & b`` / ``a | b`` / ``~a``
     - logical AND / OR / NOT

Names behind hashes
-------------------

Traces store hashes, not the full strings, for host, file path, and command.
The builder has no form for them: read the string with an arrow into a row
set of the record schema's source in duql text, such as
``TraceViewer.duql('where hhash -> hosts.name == "node01"')``. See :doc:`the
duql how-to <../guides/core/duql>`.

Reference
---------

.. autoclass:: dftracer.utils.Field
   :members: is_in, not_in, like, ilike, regex, iregex, contains

.. autoclass:: dftracer.utils.Expr
   :members:
   :no-index:
