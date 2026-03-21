"""Type stubs for dftracer_utils_utilities_ext C extension module."""

from typing import Any, Dict, List, Optional

class StatisticsQueryUtility:
    """Query pre-computed statistics from an indexed trace file."""

    def __init__(self, file_path: str, index_dir: str = "") -> None: ...
    def query(self, query_type: str = "summary", top_n: int = 10) -> Dict[str, Any]: ...

class BloomQueryUtility:
    """Query bloom filters for fast event filtering."""

    def __init__(self, file_path: str, index_dir: str = "") -> None: ...
    def query(self, predicates: Dict[str, List[str]]) -> Dict[str, Any]: ...

class StatisticsAggregatorUtility:
    """Compute statistics from a trace file via full scan."""

    def __init__(self, file_path: str, index_dir: str = "") -> None: ...
    def compute(self) -> Dict[str, Any]: ...

class MetadataCollectorUtility:
    """Collect metadata from a trace file."""

    def __init__(self, file_path: str, index_dir: str = "") -> None: ...
    def collect(self) -> Dict[str, Any]: ...

class AggregatorUtility:
    """High-level aggregation pipeline."""

    def __init__(
        self,
        directory: str,
        time_interval: float = 5.0,
        group_keys: Optional[List[str]] = None,
        categories: Optional[List[str]] = None,
    ) -> None: ...
    def run(self, output: Optional[str] = None) -> Any: ...

class ViewBuilderUtility:
    """Build filtered views over trace files."""

    def __init__(
        self,
        file_path: str,
        predicates: Optional[Dict[str, List[str]]] = None,
        index_dir: str = "",
    ) -> None: ...
    def build(self) -> Dict[str, Any]: ...

class ViewReaderUtility:
    """Read events matching a view definition."""

    def __init__(self, file_path: str, view: Dict[str, Any] = ..., index_dir: str = "") -> None: ...
    def read_lines(self) -> List[str]: ...

class ReorganizationPlannerUtility:
    """Plan semantic reorganization of trace files."""

    def __init__(
        self,
        source_files: List[str],
        groups: Optional[List[Dict[str, str]]] = None,
    ) -> None: ...
    def plan(self) -> Dict[str, Any]: ...

class ReconstructionPlannerUtility:
    """Plan reconstruction from reorganized trace files."""

    def __init__(self, reorganized_files: List[str]) -> None: ...
    def plan(self) -> Dict[str, Any]: ...
