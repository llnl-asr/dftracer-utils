"""duql builder for filtering DFTracer trace events.

A thin re-export of the unified expression builder in
:mod:`dftracer.utils.columnar`. ``F`` / ``Field`` build an :class:`Expr`;
comparisons and the string/membership predicates below serialize (``str(expr)``
/ ``expr.to_duql()``) to the duql string the index and the ``TraceViewer``
scan path consume. The same ``F`` also drives in-memory ``.apply()`` value and
mask evaluation (the columnar half).

    from dftracer.utils.duql import F

    q = (F.cat == "POSIX") & (F.dur > 1000)     # AND of two comparisons
    q = F.cat.is_in(["POSIX", "STDIO"])          # membership
    q = F.name.like("%read%")                    # SQL LIKE (also ilike/regex)
    q = F("args.file").contains("tmp")           # substring

    duql_string = str(q)                        # render for the C++ parser
"""

from __future__ import annotations

from .columnar import Expr, F, Field, Value
from .dftracer_utils_ext import duql_load_path

__all__ = ["Expr", "Field", "F", "Value", "load_path"]


def load_path(path: str) -> None:
    """Load the ``def`` macros of the ``.duql`` file ``path``, or of every
    ``.duql`` file in the directory ``path``, for every later query of the
    process. They expand after a query's and a source's own macros, like
    those on ``$DFTRACER_DUQL_PATH``.

    Raises:
        DFTUtilsIOError: If a file cannot be read.
        DFTUtilsValueError: If a file does not parse, or defines a macro that
            an already loaded file defines.
    """
    duql_load_path(path)
