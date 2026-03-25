"""Type stubs for ComparatorUtility."""

from typing import Optional

from ..arrow import ArrowTable
from ..dftracer_utils_ext import Runtime

class ComparatorUtility:
    """Compare DFTracer trace metrics between baseline and variant."""

    def __init__(self, runtime: Optional[Runtime] = None) -> None: ...
    def compare(
        self,
        baseline: str,
        variant: str,
        query: str = "",
        group_by: str = "",
        format: str = "table",
        time_interval_ms: float = 5000.0,
        threshold: float = 0.0,
        executor_threads: int = 0,
        index_dir: str = "",
        force_rebuild: bool = False,
        config: str = "",
    ) -> ArrowTable: ...
    def __call__(
        self,
        baseline: str,
        variant: str,
        query: str = "",
        group_by: str = "",
        format: str = "table",
        time_interval_ms: float = 5000.0,
        threshold: float = 0.0,
        executor_threads: int = 0,
        index_dir: str = "",
        force_rebuild: bool = False,
        config: str = "",
    ) -> ArrowTable: ...
    def compare_json(
        self,
        baseline: str,
        variant: str,
        query: str = "",
        group_by: str = "",
        format: str = "table",
        time_interval_ms: float = 5000.0,
        threshold: float = 0.0,
        executor_threads: int = 0,
        index_dir: str = "",
        force_rebuild: bool = False,
        config: str = "",
    ) -> str: ...
    def compare_table(
        self,
        baseline: str,
        variant: str,
        query: str = "",
        group_by: str = "",
        format: str = "table",
        time_interval_ms: float = 5000.0,
        threshold: float = 0.0,
        executor_threads: int = 0,
        index_dir: str = "",
        force_rebuild: bool = False,
        config: str = "",
    ) -> str: ...
