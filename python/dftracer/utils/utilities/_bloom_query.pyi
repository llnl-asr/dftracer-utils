"""Type stubs for BloomQueryUtility."""

from typing import Any, Dict, List, Optional

from ..dftracer_utils_ext import Runtime

class BloomQueryUtility:
    """Query bloom filters in an index for fast event filtering."""

    def __init__(self, runtime: Optional[Runtime] = None) -> None: ...
    def process(
        self,
        file_path: str,
        predicates: Dict[str, List[str]],
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        file_path: str,
        predicates: Dict[str, List[str]],
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
