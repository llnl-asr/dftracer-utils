Query DSL
=========

A small expression DSL for building trace filter queries. ``Field`` references
an event field (including dotted paths for nested JSON, e.g. ``args.level``),
and its operators build ``Expr`` objects that combine with ``&`` (and),
``|`` (or), and ``~`` (not). ``str(expr)`` renders the query string consumed by
the reader, statistics, and comparator utilities.

.. code-block:: python

   from dftracer.utils import Field

   cat = Field("cat")
   dur = Field("dur")

   q = (cat == "POSIX") & (dur > 1000)       # AND
   q = (cat == "POSIX") | (cat == "STDIO")   # OR
   q = ~(cat == "POSIX")                      # NOT
   q = cat.is_in(["POSIX", "STDIO"])          # membership
   q = cat.not_in(["MPI"])

   level = Field("args.level")                # nested field
   q = level == "DEBUG"

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
   * - ``a & b`` / ``a | b`` / ``~a``
     - logical AND / OR / NOT

Reference
---------

.. autoclass:: dftracer.utils.Field
   :members: is_in, not_in

.. autoclass:: dftracer.utils.Expr
   :members:
