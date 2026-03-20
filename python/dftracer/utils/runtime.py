"""Runtime wrapper with TaskHandle support for async task submission."""

from __future__ import annotations

import threading
from concurrent.futures import Future, ThreadPoolExecutor
from typing import Any, Callable, List, Optional

from .dftracer_utils_ext import Runtime as _NativeRuntime
from .dftracer_utils_ext import TaskHandle as _NativeTaskHandle


def _derive_name(fn: Any) -> str:
    qualname = getattr(fn, "__qualname__", None)
    if qualname:
        module = getattr(fn, "__module__", None)
        if module and module != "__main__":
            return f"{module}.{qualname}"
        return qualname
    name = getattr(fn, "__name__", None)
    if name:
        return name
    return type(fn).__name__


class TaskHandle:
    """Unified handle for both C++ and Python tasks.

    Wraps either a C++ _NativeTaskHandle or a concurrent.futures.Future.
    """

    __slots__ = ("_native", "_future", "_name", "_task_id", "_exception")

    def __init__(
        self,
        native: Optional[_NativeTaskHandle] = None,
        future: Optional[Future] = None,  # type: ignore[type-arg]
        name: str = "",
        task_id: int = -1,
    ) -> None:
        self._native = native
        self._future = future
        self._name = name
        self._task_id = task_id
        self._exception: Optional[BaseException] = None

    def get(self) -> Any:
        """Block until task completes and return result. Raises on error."""
        if self._native is not None:
            return self._native.get()
        if self._future is not None:
            return self._future.result()
        return None

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
        """Task name."""
        if self._native is not None:
            return self._native.name
        return self._name

    @property
    def task_id(self) -> int:
        """Task identifier."""
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
    - submit() for both C++ coroutine tasks and Python callables
    - wait_all() across both C++ and Python tasks
    - Error tracking and callbacks

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
        self._handles: List[TaskHandle] = []
        self._failed_handles: List[TaskHandle] = []
        self._lock = threading.Lock()
        self._on_task_error: Optional[Callable[[TaskHandle, BaseException], None]] = (
            None
        )
        self._py_task_counter = 0

    @classmethod
    def _from_native(cls, native: _NativeRuntime) -> "Runtime":
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

    def submit(
        self,
        task_or_fn: Any,
        *args: Any,
        name: Optional[str] = None,
        **kwargs: Any,
    ) -> TaskHandle:
        """Submit a task for async execution.

        Accepts either a C++ TaskHandle (pass-through) or a Python callable.

        Args:
            task_or_fn: Python callable, or a C++ _NativeTaskHandle to wrap.
            *args: Arguments for callable.
            name: Task name for tracking. If None, auto-derived from callable.
            **kwargs: Keyword arguments for callable.

        Returns:
            TaskHandle that can be waited on or used to get the result.
        """
        if isinstance(task_or_fn, _NativeTaskHandle):
            # C++ coroutine path: wrap a native TaskHandle from utility bindings.
            # Currently unused — will be used when utilities are ported to Python.
            handle = TaskHandle(native=task_or_fn, name=name or task_or_fn.name)
            with self._lock:
                self._handles.append(handle)
            return handle

        if not callable(task_or_fn):
            raise TypeError(
                f"Expected callable or TaskHandle, got {type(task_or_fn).__name__}"
            )

        derived_name = name or _derive_name(task_or_fn)

        with self._lock:
            task_id = self._py_task_counter
            self._py_task_counter += 1

        handle = TaskHandle(name=derived_name, task_id=task_id)

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

    def wait(self, handle: TaskHandle) -> None:
        """Block until a specific task completes."""
        handle.wait()

    def wait_all(self, raise_on_error: bool = False) -> None:
        """Block until all submitted tasks complete.

        Args:
            raise_on_error: If True, raise RuntimeError after all tasks
                complete if any task failed.
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

    def get_failed(self) -> List[TaskHandle]:
        """Return handles of tasks that failed since last clear.

        Call after wait_all() to inspect failures.
        """
        with self._lock:
            return list(self._failed_handles)

    def clear_failed(self) -> None:
        """Clear the list of failed task handles."""
        with self._lock:
            self._failed_handles.clear()

    def set_error_callback(
        self,
        callback: Optional[Callable[[TaskHandle, BaseException], None]],
    ) -> None:
        """Set callback invoked when any task fails.

        Called from the task's thread. Must be thread-safe.
        Set to None to clear.
        """
        self._on_task_error = callback

    def shutdown(self, wait: bool = True) -> None:
        """Shut down the runtime."""
        if wait:
            try:
                self.wait_all()
            except Exception:
                pass
        if self._py_pool is not None:
            self._py_pool.shutdown(wait=wait)
            self._py_pool = None
        self._native.shutdown()

    def get_progress(self) -> dict:  # type: ignore[type-arg]
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
        return self._py_pool._max_workers  # type: ignore[attr-defined]

    def __enter__(self) -> "Runtime":
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        self.shutdown()
