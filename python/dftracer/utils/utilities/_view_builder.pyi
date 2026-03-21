"""Type stubs for ViewBuilderUtility."""

from typing import Any, Dict, List, Optional

from ..dftracer_utils_ext import Runtime

class ViewBuilderUtility:
    """Query the bloom-filter index to find candidate chunks."""

    def __init__(self, runtime: Optional[Runtime] = None) -> None: ...
    def process(
        self,
        file_path: str,
        predicates: Optional[Dict[str, List[str]]] = None,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        file_path: str,
        predicates: Optional[Dict[str, List[str]]] = None,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
