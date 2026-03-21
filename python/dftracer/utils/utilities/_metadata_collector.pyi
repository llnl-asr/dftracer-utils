"""Type stubs for MetadataCollectorUtility."""

from typing import Any, Dict, Optional

from ..dftracer_utils_ext import Runtime

class MetadataCollectorUtility:
    """Collect metadata from a DFTracer trace file."""

    def __init__(self, runtime: Optional[Runtime] = None) -> None: ...
    def process(self, file_path: str, index_dir: str = "") -> Dict[str, Any]: ...
    def __call__(self, file_path: str, index_dir: str = "") -> Dict[str, Any]: ...
