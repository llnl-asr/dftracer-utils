"""Type stubs for AggregatorUtility."""

from typing import Any, Iterator, List, Optional

from ..arrow import ArrowTable
from ..dftracer_utils_ext import Runtime

class AggregatorUtility:
    """High-level aggregation pipeline for DFTracer trace files."""

    def __init__(self, runtime: Optional[Runtime] = None) -> None: ...
    def process(
        self,
        directory: str,
        time_interval_ms: float = 5000.0,
        group_keys: Optional[List[str]] = None,
        categories: Optional[List[str]] = None,
        names: Optional[List[str]] = None,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        force_rebuild: bool = False,
        chunk_size_mb: int = 64,
        batch_size_mb: int = 4,
        event_batch_size: int = 10000,
        custom_metric_fields: Optional[List[str]] = None,
        compute_percentiles: bool = False,
    ) -> ArrowTable: ...
    def __call__(
        self,
        directory: str,
        time_interval_ms: float = 5000.0,
        group_keys: Optional[List[str]] = None,
        categories: Optional[List[str]] = None,
        names: Optional[List[str]] = None,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        force_rebuild: bool = False,
        chunk_size_mb: int = 64,
        batch_size_mb: int = 4,
        event_batch_size: int = 10000,
        custom_metric_fields: Optional[List[str]] = None,
        compute_percentiles: bool = False,
    ) -> ArrowTable: ...
    def iter_arrow(
        self,
        directory: str,
        time_interval_ms: float = 5000.0,
        group_keys: Optional[List[str]] = None,
        categories: Optional[List[str]] = None,
        names: Optional[List[str]] = None,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        force_rebuild: bool = False,
        chunk_size_mb: int = 64,
        batch_size_mb: int = 4,
        event_batch_size: int = 10000,
        custom_metric_fields: Optional[List[str]] = None,
        compute_percentiles: bool = False,
    ) -> Iterator[Any]: ...
