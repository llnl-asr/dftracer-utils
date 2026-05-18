"""Python TraceReader wrapping the native C extension.

Delegates all native methods and adds iter_lines_json / read_lines_json
as shims over iter_arrow via pyarrow.
"""

from __future__ import annotations

from types import TracebackType
from typing import TYPE_CHECKING, Any, Dict, Iterator, List, Optional, Type, Union

from .dftracer_utils_ext import (
    JsonDictValue,
    _ArrowBatchCapsule,
)
from .dftracer_utils_ext import (
    TraceReader as _NativeTraceReader,
)

if TYPE_CHECKING:
    from .arrow import ArrowTable
    from .runtime import Runtime


class TraceReader:
    __slots__ = ("_native",)

    def __init__(
        self,
        path: str,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        auto_build_index: bool = False,
        runtime: Optional[Runtime] = None,
    ) -> None:
        if runtime is not None:
            self._native = _NativeTraceReader(
                path,
                index_dir=index_dir,
                checkpoint_size=checkpoint_size,
                auto_build_index=auto_build_index,
                runtime=runtime,
            )
        else:
            self._native = _NativeTraceReader(
                path,
                index_dir=index_dir,
                checkpoint_size=checkpoint_size,
                auto_build_index=auto_build_index,
            )

    # -- properties --

    @property
    def path(self) -> str:
        return self._native.path

    @property
    def index_dir(self) -> str:
        return self._native.index_dir

    @property
    def has_index(self) -> bool:
        return self._native.has_index

    @property
    def num_lines(self) -> int:
        return self._native.num_lines

    # -- lines --

    def read_lines(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
    ) -> List[memoryview]:
        return self._native.read_lines(
            start_line=start_line,
            end_line=end_line,
            start_byte=start_byte,
            end_byte=end_byte,
            buffer_size=buffer_size,
            query=query,
        )

    def iter_lines(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        memory_budget: int = 0,
    ) -> Iterator[memoryview]:
        return self._native.iter_lines(
            start_line=start_line,
            end_line=end_line,
            start_byte=start_byte,
            end_byte=end_byte,
            buffer_size=buffer_size,
            query=query,
            memory_budget=memory_budget,
        )

    # -- json --

    def iter_json(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        batch_size: int = 1024,
        memory_budget: int = 0,
    ) -> Iterator[JsonDictValue]:
        return self._native.iter_json(
            start_line=start_line,
            end_line=end_line,
            start_byte=start_byte,
            end_byte=end_byte,
            buffer_size=buffer_size,
            query=query,
            batch_size=batch_size,
            memory_budget=memory_budget,
        )

    def read_json(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        batch_size: int = 1024,
    ) -> List[JsonDictValue]:
        return self._native.read_json(
            start_line=start_line,
            end_line=end_line,
            start_byte=start_byte,
            end_byte=end_byte,
            buffer_size=buffer_size,
            query=query,
            batch_size=batch_size,
        )

    # -- raw --

    def read_raw(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        line_aligned: bool = True,
        multi_line: bool = True,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
    ) -> List[memoryview]:
        return self._native.read_raw(
            start_line=start_line,
            end_line=end_line,
            start_byte=start_byte,
            end_byte=end_byte,
            line_aligned=line_aligned,
            multi_line=multi_line,
            buffer_size=buffer_size,
            query=query,
        )

    def iter_raw(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        line_aligned: bool = True,
        multi_line: bool = True,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        memory_budget: int = 0,
    ) -> Iterator[memoryview]:
        return self._native.iter_raw(
            start_line=start_line,
            end_line=end_line,
            start_byte=start_byte,
            end_byte=end_byte,
            line_aligned=line_aligned,
            multi_line=multi_line,
            buffer_size=buffer_size,
            query=query,
            memory_budget=memory_budget,
        )

    # -- arrow --

    def iter_arrow(
        self,
        batch_size: int = 10000,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        flatten_objects: bool = False,
        normalize: bool = False,
        memory_budget: int = 0,
    ) -> Iterator[_ArrowBatchCapsule]:
        return self._native.iter_arrow(
            batch_size=batch_size,
            start_line=start_line,
            end_line=end_line,
            start_byte=start_byte,
            end_byte=end_byte,
            buffer_size=buffer_size,
            query=query,
            flatten_objects=flatten_objects,
            normalize=normalize,
            memory_budget=memory_budget,
        )

    def iter_arrow_stream(
        self,
        batch_size: int = 10000,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        flatten_objects: bool = False,
        normalize: bool = False,
        memory_budget: int = 0,
    ) -> Any:
        return self._native.iter_arrow_stream(
            batch_size=batch_size,
            start_line=start_line,
            end_line=end_line,
            start_byte=start_byte,
            end_byte=end_byte,
            buffer_size=buffer_size,
            query=query,
            flatten_objects=flatten_objects,
            normalize=normalize,
            memory_budget=memory_budget,
        )

    def read_arrow(
        self,
        batch_size: int = 10000,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        flatten_objects: bool = False,
        normalize: bool = False,
    ) -> ArrowTable:
        return self._native.read_arrow(
            batch_size=batch_size,
            start_line=start_line,
            end_line=end_line,
            start_byte=start_byte,
            end_byte=end_byte,
            buffer_size=buffer_size,
            query=query,
            flatten_objects=flatten_objects,
            normalize=normalize,
        )

    # -- JSON shims via arrow --

    def iter_lines_json(
        self,
        batch_size: int = 10000,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
    ) -> Iterator[Dict[str, Any]]:
        try:
            import pyarrow as pa
        except ImportError:
            raise ImportError(
                "pyarrow is required for iter_lines_json. Install with: pip install pyarrow"
            ) from None

        for capsule in self._native.iter_arrow(
            batch_size=batch_size,
            start_line=start_line,
            end_line=end_line,
            start_byte=start_byte,
            end_byte=end_byte,
            buffer_size=buffer_size,
            query=query,
        ):
            rb = pa.record_batch(capsule)
            yield from rb.to_pylist()

    def read_lines_json(
        self,
        batch_size: int = 10000,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
    ) -> List[Dict[str, Any]]:
        return list(
            self.iter_lines_json(
                batch_size=batch_size,
                start_line=start_line,
                end_line=end_line,
                start_byte=start_byte,
                end_byte=end_byte,
                buffer_size=buffer_size,
                query=query,
            )
        )

    # -- metadata --

    def get_max_bytes(self) -> int:
        return self._native.get_max_bytes()

    def get_num_lines(self) -> int:
        return self._native.get_num_lines()

    # -- arrow IPC writing --

    def write_arrow(
        self,
        path: str,
        views: Optional[List[Union[str, Dict[str, Any]]]] = None,
        chunk_size_mb: int = 32,
        compression: str = "zstd",
        batch_size: int = 10000,
    ) -> Dict[str, Any]:
        return self._native.write_arrow(
            path,
            views=views,
            chunk_size_mb=chunk_size_mb,
            compression=compression,
            batch_size=batch_size,
        )

    def get_view_chunks(
        self,
        view: Optional[Union[str, Dict[str, Any]]] = None,
    ) -> Dict[str, Any]:
        return self._native.get_view_chunks(view=view)

    def write_view_chunk(
        self,
        output_file: str,
        checkpoint_idx: int,
        start_byte: int,
        end_byte: int,
        view: Optional[Union[str, Dict[str, Any]]] = None,
        compression: str = "zstd",
        batch_size: int = 10000,
    ) -> Dict[str, Any]:
        return self._native.write_view_chunk(
            output_file=output_file,
            checkpoint_idx=checkpoint_idx,
            start_byte=start_byte,
            end_byte=end_byte,
            view=view,
            compression=compression,
            batch_size=batch_size,
        )

    def write_view_chunks(
        self,
        chunks: List[Dict[str, Any]],
        output_dir: str,
        view: Optional[Union[str, Dict[str, Any]]] = None,
        compression: str = "zstd",
        batch_size: int = 10000,
    ) -> Dict[str, Any]:
        return self._native.write_view_chunks(
            chunks=chunks,
            output_dir=output_dir,
            view=view,
            compression=compression,
            batch_size=batch_size,
        )

    # -- context manager --

    def __enter__(self) -> TraceReader:
        self._native.__enter__()
        return self

    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> None:
        self._native.__exit__(exc_type, exc_val, exc_tb)
