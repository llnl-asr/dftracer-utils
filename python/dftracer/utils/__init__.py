# SPDX-License-Identifier: MIT
from . import (  # noqa: F401
    errors,
    index,
    plugins,
    runtime,
    schemas,
    store,
    trace,
    typing,
)
from ._agg import Udaf, count, udaf  # noqa: F401
from ._dataframe import DataFrame  # noqa: F401
from ._enums import AggOp, DType, GroupKey, TimeUnit  # noqa: F401
from ._expr import (  # noqa: F401
    Agg,
    Expr,
    F,
    Udf,
    call,
    col,
    eval_many,
    if_else,
    lit,
    udf,
    when,
    where,
)
from ._groupby import GroupBy  # noqa: F401
from ._lazyframe import (  # noqa: F401
    BoundColumn,
    LazyFrame,
    LazyResult,
    LazyScalar,
    SinkSummary,
    collect_all,
    lazy,
    scan_file,
)
from ._public import set_home as _set_home
from ._series import Series  # noqa: F401
from ._table import TableWriter, read_table, write_table  # noqa: F401
from ._tz import install_search_path as _install_tz_search_path

try:
    from ._version import __version__
except ImportError:  # a wheel built without the generated file
    from importlib.metadata import PackageNotFoundError, version

    try:
        __version__ = version("dftracer-utils")
    except PackageNotFoundError:
        __version__ = "0.0.0"


__all__ = [
    "DataFrame",
    "Series",
    "LazyFrame",
    "GroupBy",
    "Expr",
    "F",
    "col",
    "lit",
    "call",
    "where",
    "when",
    "if_else",
    "eval_many",
    "count",
    "Agg",
    "AggOp",
    "Udf",
    "udf",
    "Udaf",
    "udaf",
    "lazy",
    "collect_all",
    "LazyResult",
    "LazyScalar",
    "BoundColumn",
    "SinkSummary",
    "DType",
    "TimeUnit",
    "GroupKey",
    "read_table",
    "write_table",
    "TableWriter",
    "scan_file",
]

_install_tz_search_path()

_set_home(__name__, globals(), __all__)
