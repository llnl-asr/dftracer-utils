# SPDX-License-Identifier: MIT
"""The duql builder: queries as Python values that print as duql text.

A builder query parses to the same syntax tree as the equal text, so the two
give the same plan and rows::

    from dftracer.utils.duql import c, fn, source

    q = (
        source("trace.pfw.gz")
        .where((c("cat") == "POSIX") & (c("dur") > 1000))
        .group("name", n=fn.count(), total=c("dur").sum())
        .sort(-c("total"))
        .take(10)
    )
    q.text()      # 'duql 0.0\nfrom "trace.pfw.gz"\n| where ...'
    q.collect()   # a DataFrame

Every builder call only builds; ``collect``, ``count``, ``first``, ``stream``
and ``explain`` run the query. A method of :class:`Col` that is not listed
here is a call with the column as its first argument: ``c("dur").sum()`` is
``sum(dur)``. Values are never spliced into the text: bind them with
:meth:`Pipe.bind` and read them with :func:`param`.
"""

from __future__ import annotations

from .col import (
    Col as Col,
)
from .col import (
    Named as Named,
)
from .col import (
    SortKey as SortKey,
)
from .col import (
    c as c,
)
from .col import (
    case_ as case_,
)
from .col import (
    duration as duration,
)
from .col import (
    fn as fn,
)
from .col import (
    lit as lit,
)
from .col import (
    param as param,
)
from .col import (
    tup as tup,
)
from .pipe import (
    Pipe as Pipe,
)
from .pipe import (
    Source as Source,
)
from .pipe import (
    load_path as load_path,
)
from .pipe import (
    rowset as rowset,
)
from .pipe import (
    source as source,
)
from .pipe import (
    sub as sub,
)

__all__ = [
    "Col",
    "Named",
    "Pipe",
    "SortKey",
    "Source",
    "c",
    "case_",
    "duration",
    "fn",
    "lit",
    "load_path",
    "param",
    "rowset",
    "source",
    "sub",
    "tup",
]
