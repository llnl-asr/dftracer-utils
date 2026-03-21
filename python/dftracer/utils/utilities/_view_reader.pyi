"""Type stubs for ViewReaderUtility."""

from typing import Any, Dict, Iterator, List, Optional

from ..arrow import ArrowTable
from ..dftracer_utils_ext import Runtime

class ViewReaderUtility:
    """Read events from a trace file filtered by view predicates."""

    def __init__(self, runtime: Optional[Runtime] = None) -> None: ...
    def process(
        self,
        file_path: str,
        predicates: Optional[Dict[str, List[str]]] = None,
        index_dir: str = "",
    ) -> ArrowTable: ...
    def __call__(
        self,
        file_path: str,
        predicates: Optional[Dict[str, List[str]]] = None,
        index_dir: str = "",
    ) -> ArrowTable: ...
    def iter_arrow(
        self,
        file_path: str,
        predicates: Optional[Dict[str, List[str]]] = None,
        index_dir: str = "",
        batch_size: int = 10000,
    ) -> Iterator[Any]: ...
