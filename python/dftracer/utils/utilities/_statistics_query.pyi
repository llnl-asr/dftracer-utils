"""Type stubs for StatisticsQueryUtility."""

from typing import Any, Dict, Optional

from ..dftracer_utils_ext import Runtime

class StatisticsQueryUtility:
    """Query pre-computed statistics from an indexed trace file."""

    def __init__(self, runtime: Optional[Runtime] = None) -> None: ...
    def process(
        self,
        file_path: str,
        query_type: str = "summary",
        top_n: int = 10,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        file_path: str,
        query_type: str = "summary",
        top_n: int = 10,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
