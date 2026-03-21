"""Type stubs for StatisticsAggregatorUtility."""

from typing import Any, Dict, Optional

from ..dftracer_utils_ext import Runtime

class StatisticsAggregatorUtility:
    """Aggregate statistics from a trace file via full scan."""

    def __init__(self, runtime: Optional[Runtime] = None) -> None: ...
    def process(self, file_path: str, index_dir: str = "") -> Dict[str, Any]: ...
    def __call__(self, file_path: str, index_dir: str = "") -> Dict[str, Any]: ...
