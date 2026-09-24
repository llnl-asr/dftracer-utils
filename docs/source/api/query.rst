:description: Reference for the Python filter DSL: build Expr predicates from F/Field, combine with & | ~, and render the query string TraceViewer scans with.

Query DSL
=========

A small expression DSL for building trace filter queries. ``F`` (or the
back-compat spelling ``Field``) references an event field (including dotted
paths for nested JSON, e.g. ``args.level``), and its operators build ``Expr``
objects that combine with ``&`` (and), ``|`` (or), and ``~`` (not).
``str(expr)`` / ``expr.to_query()`` render the query string consumed by the
index and the ``TraceViewer`` scan path.

``F``, ``Field``, and ``resolved`` are the same objects exported from
``dftracer.utils`` and ``dftracer.utils.query``. ``F`` is the single unified
builder: the same field leaf that builds these predicates also builds columnar
value expressions and evaluates them in memory with ``.apply()`` (see
:doc:`columnar`).

.. code-block:: python

   from dftracer.utils import F, Field, resolved

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
   q = F.name.regex("^p?read$")                 # ECMAScript regex

   q = resolved("hhash.name") == "node01"       # dictionary field, rewritten to a key lookup

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
     - SQL LIKE (``%`` any run, ``_`` one char) / case-insensitive
   * - ``field.regex(pattern)`` / ``field.iregex(pattern)``
     - ECMAScript regex match / case-insensitive
   * - ``field.contains(sub)``
     - unanchored substring search
   * - ``a & b`` / ``a | b`` / ``~a``
     - logical AND / OR / NOT

Resolved (virtual) fields
--------------------------

Traces store hashes, not the full strings, for host, file path, and command.
``resolved(name)`` (or ``r.<name>``) queries the real value; the engine rewrites
it to a hash lookup against the index. Known names: ``fpath``, ``cwd``,
``hostname`` (alias ``host``), ``exec``, ``cmd``. See :doc:`the query-DSL
how-to <../guides/core/query-dsl>` for more.

Reference
---------

.. autoclass:: dftracer.utils.Field
   :members: is_in, not_in, like, ilike, regex, iregex, contains

.. autoclass:: dftracer.utils.Expr
   :members:
   :no-index:

.. autofunction:: dftracer.utils.resolved
