"""Type stubs for ReconstructionPlannerUtility."""

from typing import Any, Dict, List, Optional

from ..dftracer_utils_ext import Runtime

class ReconstructionPlannerUtility:
    """Plan reconstruction of original files from reorganized traces."""

    def __init__(self, runtime: Optional[Runtime] = None) -> None: ...
    def process(
        self,
        reorganized_files: List[str],
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        reorganized_files: List[str],
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
