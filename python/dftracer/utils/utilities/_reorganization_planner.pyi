"""Type stubs for ReorganizationPlannerUtility."""

from typing import Any, Dict, List, Optional

from ..dftracer_utils_ext import Runtime

class ReorganizationPlannerUtility:
    """Plan semantic reorganization of trace files."""

    def __init__(self, runtime: Optional[Runtime] = None) -> None: ...
    def process(
        self,
        source_files: List[str],
        groups: Optional[List[Dict[str, str]]] = None,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        source_files: List[str],
        groups: Optional[List[Dict[str, str]]] = None,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
