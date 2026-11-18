# SPDX-License-Identifier: MIT
from . import schemas  # noqa: F401
from ._eval import columnar, eval_many, where  # noqa: F401
from .agg import Udaf, count, udaf  # noqa: F401
from .dataframe import DataFrame  # noqa: F401
from .dftracer_utils_ext import (  # noqa: F401
    CheckpointIndexer,
    consolidate_shard_set,
    get_log_level,
    set_log_color,
    set_log_level,
    write_shard_set,
)
from .enums import AggOp, DType, GroupKey, Phase, TimeUnit  # noqa: F401
from .errors import (  # noqa: F401
    DFTUtilsAggregationError,
    DFTUtilsCompressionError,
    DFTUtilsDuqlError,
    DFTUtilsError,
    DFTUtilsIndexerError,
    DFTUtilsIOError,
    DFTUtilsNotFoundError,
    DFTUtilsParseError,
    DFTUtilsPipelineError,
    DFTUtilsReaderError,
    DFTUtilsValueError,
)
from .expr import (  # noqa: F401
    Agg,
    Expr,
    F,
    Udf,
    call,
    col,
    lit,
    udf,
)
from .groupby import GroupBy  # noqa: F401
from .index import (  # noqa: F401
    AggregationConfig,
    BloomConfig,
    Indexer,
    IndexStatus,
)
from .lazyframe import (  # noqa: F401
    BoundColumn,
    LazyFrame,
    LazyResult,
    LazyScalar,
    SinkSummary,
    collect_all,
    lazy,
    scan_file,
)
from .runtime import Runtime, TaskHandle, get_default_runtime, peek_default_runtime  # noqa: F401
from .series import Series  # noqa: F401
from .store import Store  # noqa: F401
from .trace import Containment, LiveFileStatus, LiveTraceViewer, TraceViewer  # noqa: F401

try:
    from ._version import __version__
except ImportError:  # a wheel built without the generated file
    from importlib.metadata import PackageNotFoundError, version

    try:
        __version__ = version("dftracer-utils")
    except PackageNotFoundError:
        __version__ = "0.0.0"


__all__ = [
    "AggregationConfig",
    "BloomConfig",
    "CheckpointIndexer",
    "Expr",
    "col",
    "lit",
    "columnar",
    "F",
    "where",
    "Udaf",
    "Udf",
    "udaf",
    "udf",
    "call",
    "Agg",
    "GroupBy",
    "count",
    "eval_many",
    "Indexer",
    "IndexStatus",
    "AggOp",
    "DType",
    "GroupKey",
    "Phase",
    "TimeUnit",
    "Store",
    "TraceViewer",
    "LiveFileStatus",
    "LiveTraceViewer",
    "Containment",
    "DataFrame",
    "LazyFrame",
    "LazyResult",
    "LazyScalar",
    "BoundColumn",
    "SinkSummary",
    "collect_all",
    "lazy",
    "scan_file",
    "Series",
    "Runtime",
    "TaskHandle",
    "get_default_runtime",
    "peek_default_runtime",
    "get_log_level",
    "set_log_color",
    "set_log_level",
    "write_shard_set",
    "consolidate_shard_set",
    "DFTUtilsError",
    "DFTUtilsAggregationError",
    "DFTUtilsCompressionError",
    "DFTUtilsDuqlError",
    "DFTUtilsIndexerError",
    "DFTUtilsIOError",
    "DFTUtilsNotFoundError",
    "DFTUtilsParseError",
    "DFTUtilsPipelineError",
    "DFTUtilsReaderError",
    "DFTUtilsValueError",
]
