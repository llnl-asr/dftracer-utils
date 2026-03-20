"""Runtime wrapper with TaskHandle support for async task submission."""

from __future__ import annotations

import threading
from concurrent.futures import Future, ThreadPoolExecutor
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    Generic,
    List,
    Optional,
    TypeVar,
    overload,
)

from .dftracer_utils_ext import Runtime as _NativeRuntime
from .dftracer_utils_ext import TaskHandle as _NativeTaskHandle

if TYPE_CHECKING:
    from types import TracebackType

T = TypeVar("T")


def _derive_name(fn: object) -> str:
    """Derive task name from callable."""
    qualname = getattr(fn, "__qualname__", None)
    if qualname:
        module = getattr(fn, "__module__", None)
        if module and module != "__main__":
            return f"{module}.{qualname}"
        return str(qualname)
    name = getattr(fn, "__name__", None)
    if name:
        return str(name)
    return type(fn).__name__


class TaskHandle(Generic[T]):
    """Unified handle for both C++ and Python tasks.

    Wraps either a C++ _NativeTaskHandle or a concurrent.futures.Future.

    Example::

        h = rt.submit(lambda: 42)
        result = h.get()  # int
        h.wait()
        assert h.done()
    """

    __slots__ = ("_native", "_future", "_name", "_task_id", "_exception")

    def __init__(
        self,
        native: Optional[_NativeTaskHandle] = None,
        future: Optional[Future[T]] = None,
        name: str = "",
        task_id: int = -1,
    ) -> None:
        self._native = native
        self._future = future
        self._name = name
        self._task_id = task_id
        self._exception: Optional[BaseException] = None

    def get(self) -> T:
        """Block until task completes and return result. Raises on error."""
        if self._native is not None:
            return self._native.get()
        if self._future is not None:
            return self._future.result()
        return None  # type: ignore[return-value]  # no backing future

    def wait(self) -> None:
        """Block until task completes. Raises on error."""
        if self._native is not None:
            self._native.wait()
        elif self._future is not None:
            self._future.result()

    def done(self) -> bool:
        """Return True if task has completed (success or failure)."""
        if self._native is not None:
            return self._native.done()
        if self._future is not None:
            return self._future.done()
        return True

    @property
    def name(self) -> str:
        """Task name (auto-derived or user-provided)."""
        if self._native is not None:
            return self._native.name
        return self._name

    @property
    def task_id(self) -> int:
        """Unique task identifier."""
        if self._native is not None:
            return self._native.task_id
        return self._task_id

    @property
    def exception(self) -> Optional[BaseException]:
        """Stored exception if task failed, None otherwise."""
        return self._exception


class Runtime:
    """Runtime with async task submission and Python callable support.

    Wraps the C++ Runtime and adds:

    - ``submit()`` for both C++ coroutine tasks and Python callables
    - ``wait_all()`` across both C++ and Python tasks
    - Error tracking and callbacks

    Example::

        with Runtime(threads=8, python_threads=4) as rt:
            h = rt.submit(lambda x: x * 2, 21)
            assert h.get() == 42

    Args:
        threads: Number of C++ executor threads (0 = hardware_concurrency).
        python_threads: Number of Python ThreadPoolExecutor threads
            (0 = min(32, threads)).
    """

    def __init__(self, threads: int = 0, python_threads: int = 0) -> None:
        self._native = _NativeRuntime(threads)
        self._init_fields(python_threads)

    def _init_fields(self, python_threads: int = 0) -> None:
        self._py_pool: Optional[ThreadPoolExecutor] = None
        self._py_pool_size = python_threads
        self._handles: List[TaskHandle[Any]] = []
        self._failed_handles: List[TaskHandle[Any]] = []
        self._lock = threading.Lock()
        self._on_task_error: Optional[Callable[[TaskHandle[Any], BaseException], None]] = None
        self._py_task_counter = 0

    @classmethod
    def _from_native(cls, native: _NativeRuntime) -> Runtime:
        """Create a Runtime wrapper around an existing C++ Runtime."""
        obj = cls.__new__(cls)
        obj._native = native
        obj._init_fields()
        return obj

    @property
    def _python_pool(self) -> ThreadPoolExecutor:
        if self._py_pool is None:
            with self._lock:
                if self._py_pool is None:
                    size = self._py_pool_size or min(32, self._native.threads or 4)
                    self._py_pool = ThreadPoolExecutor(max_workers=size)
        return self._py_pool

    @overload
    def submit(
        self,
        task_or_fn: _NativeTaskHandle,
        *args: Any,
        name: Optional[str] = ...,
        **kwargs: Any,
    ) -> TaskHandle[Any]: ...

    @overload
    def submit(
        self,
        task_or_fn: Callable[..., T],
        *args: Any,
        name: Optional[str] = ...,
        **kwargs: Any,
    ) -> TaskHandle[T]: ...

    def submit(
        self,
        task_or_fn: Any,
        *args: Any,
        name: Optional[str] = None,
        **kwargs: Any,
    ) -> TaskHandle[Any]:
        """Submit a task for async execution.

        Accepts either a C++ TaskHandle (pass-through) or a Python callable.

        Args:
            task_or_fn: Python callable or a C++ _NativeTaskHandle to wrap.
            *args: Arguments for callable (ignored for C++ TaskHandle).
            name: Task name for tracking. If None, auto-derived:

                - C++ TaskHandle: uses name from the handle
                - callable: qualified name (e.g. ``"module.func"``)
            **kwargs: Keyword arguments for callable.

        Returns:
            TaskHandle that can be waited on or used to get the result.

        Raises:
            TypeError: If task_or_fn is not callable or a TaskHandle.

        Example::

            h = rt.submit(lambda x, y: x + y, 3, 4, name="add")
            result = h.get()  # 7
        """
        if isinstance(task_or_fn, _NativeTaskHandle):
            # C++ coroutine path: wrap a native TaskHandle from utility
            # bindings. Currently unused — will be used when utilities
            # are ported to Python.
            handle: TaskHandle[Any] = TaskHandle(native=task_or_fn, name=name or task_or_fn.name)
            with self._lock:
                self._handles.append(handle)
            return handle

        if not callable(task_or_fn):
            raise TypeError(f"Expected callable or TaskHandle, got {type(task_or_fn).__name__}")

        derived_name = name or _derive_name(task_or_fn)

        with self._lock:
            task_id = self._py_task_counter
            self._py_task_counter += 1

        handle = TaskHandle[Any](name=derived_name, task_id=task_id)

        def wrapper() -> Any:
            try:
                return task_or_fn(*args, **kwargs)
            except BaseException as e:
                handle._exception = e
                with self._lock:
                    self._failed_handles.append(handle)
                cb = self._on_task_error
                if cb is not None:
                    try:
                        cb(handle, e)
                    except Exception:
                        pass
                raise

        handle._future = self._python_pool.submit(wrapper)

        with self._lock:
            self._handles.append(handle)

        return handle

    def wait(self, handle: TaskHandle[Any]) -> None:
        """Block until a specific task completes."""
        handle.wait()

    def wait_all(self, raise_on_error: bool = False) -> None:
        """Block until all submitted tasks complete.

        Args:
            raise_on_error: If True, raise RuntimeError after all tasks
                complete if any task failed. The error message includes
                all failed task names. If False (default), failed tasks
                are silently collected — check individual handles with
                ``.get()`` to see errors.
        """
        self._native.wait_all()

        with self._lock:
            handles = list(self._handles)

        errors: List[str] = []
        for h in handles:
            try:
                h.wait()
            except Exception as e:
                h._exception = e
                with self._lock:
                    if h not in self._failed_handles:
                        self._failed_handles.append(h)
                if not raise_on_error:
                    continue
                errors.append(f"{h.name}: {e}")

        with self._lock:
            self._handles.clear()

        if raise_on_error and errors:
            raise RuntimeError(f"{len(errors)} task(s) failed:\n" + "\n".join(errors))

    def get_failed(self) -> List[TaskHandle[Any]]:
        """Return handles of tasks that failed since last clear.

        Call after ``wait_all()`` to inspect failures.
        """
        with self._lock:
            return list(self._failed_handles)

    def clear_failed(self) -> None:
        """Clear the list of failed task handles."""
        with self._lock:
            self._failed_handles.clear()

    def set_error_callback(
        self,
        callback: Optional[Callable[[TaskHandle[Any], BaseException], None]],
    ) -> None:
        """Set callback invoked when any task fails.

        Called from the task's thread. Must be thread-safe.
        Set to None to clear.

        Example::

            rt.set_error_callback(
                lambda h, e: print(f"FAILED {h.name}: {e}")
            )
        """
        self._on_task_error = callback

    def shutdown(self, wait: bool = True) -> None:
        """Shut down the runtime.

        Args:
            wait: If True (default), wait for all tasks to complete first.
        """
        if wait:
            try:
                self.wait_all()
            except Exception:
                pass
        if self._py_pool is not None:
            self._py_pool.shutdown(wait=wait)
            self._py_pool = None
        self._native.shutdown()

    def get_progress(self) -> Dict[str, Any]:
        """Return progress dict from C++ executor."""
        return self._native.get_progress()

    def is_responsive(self) -> bool:
        """Return True if the runtime is making progress."""
        return self._native.is_responsive()

    def set_timeout(self, global_ms: int = 0) -> None:
        """Set global timeout in milliseconds."""
        self._native.set_timeout(global_ms=global_ms)

    def set_default_task_timeout(self, ms: int = 0) -> None:
        """Set default per-task timeout in milliseconds."""
        self._native.set_default_task_timeout(ms=ms)

    @property
    def threads(self) -> int:
        """Number of C++ worker threads."""
        return self._native.threads

    @property
    def python_threads(self) -> int:
        """Number of Python worker threads (0 if pool not yet created)."""
        if self._py_pool is None:
            return 0
        return self._py_pool._max_workers

    def __enter__(self) -> Runtime:
        return self

    def __exit__(
        self,
        exc_type: Optional[type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> None:
        self.shutdown()
