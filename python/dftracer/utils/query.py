"""Query DSL for filtering DFTracer trace events.

A thin re-export of the unified expression builder in
:mod:`dftracer.utils.columnar`. ``F`` / ``Field`` build an :class:`Expr`;
comparisons and the string/membership predicates below serialize (``str(expr)``
/ ``expr.to_query()``) to the query string the index and the ``TraceViewer``
scan path consume. The same ``F`` also drives in-memory ``.apply()`` value and
mask evaluation (the columnar half).

    from dftracer.utils.query import F, resolved

    q = (F.cat == "POSIX") & (F.dur > 1000)     # AND of two comparisons
    q = F.cat.is_in(["POSIX", "STDIO"])          # membership
    q = F.name.like("%read%")                    # SQL LIKE (also ilike/regex)
    q = F("args.file").contains("tmp")           # substring
    q = resolved("hhash.name") == "node01"       # dictionary field -> key lookup

    query_string = str(q)                        # render for the C++ parser
"""

from __future__ import annotations

from .columnar import Expr, F, Field, Value, resolved

__all__ = ["Expr", "Field", "F", "resolved", "Value"]
