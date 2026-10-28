"""Runtime wrapper with TaskHandle support for async task submission."""

from __future__ import annotations

import os
import threading
from concurrent.futures import Future, ThreadPoolExecutor
from typing import (
    TYPE_CHECKING,
    Callable,
    Generic,
    List,
    Optional,
    TypedDict,
    TypeVar,
    Union,
    cast,
    overload,
)

from ._units import coerce_duration
from .dftracer_utils_ext import Runtime as _NativeRuntime
from .dftracer_utils_ext import TaskHandle as _NativeTaskHandle

if TYPE_CHECKING:
    from types import TracebackType

    from typing_extensions import ParamSpec

    # Only the type checker needs P: submit's overloads reference it in string
    # annotations (from __future__ import annotations), so it is never evaluated
    # at runtime and needs no import there.
    P = ParamSpec("P")

# Covariant (get() -> T_co only) so the runtime can hold one list of handles
# typed TaskHandle[object] instead of Any.
T_co = TypeVar("T_co", covariant=True)
# Invariant binder for submit(): ties the callable's return to TaskHandle[R].
R = TypeVar("R")


class WorkerProgress(TypedDict):
    """One worker's slice of :meth:`Runtime.get_progress`."""

    id: int
    idle: bool
    task: str
    queue_depth: int


class TaskProgress(TypedDict):
    """One task's slice of :meth:`Runtime.get_progress` (``children`` recurses)."""

    name: str
    state: str
    queued_duration_ms: float
    execution_duration_ms: float
    total_subtasks: int
    completed_subtasks: int
    progress_pct: float
    location: str
    children: List["TaskProgress"]


class RuntimeProgress(TypedDict):
    """Return shape of :meth:`Runtime.get_progress`."""

    total: int
    completed: int
    running: int
    queued: int
    failed: int
    workers: List[WorkerProgress]
    tasks: List[TaskProgress]


def _source_location(fn: object) -> str:
    """``basename:lineno`` of a callable's definition, or "" if unavailable.

    Column info would need ``code.co_positions`` (Python 3.11+); the supported
    floor is 3.8, so only file and line are reported.
    """
    code = getattr(fn, "__code__", None)
    if code is None:
        return ""
    filename = getattr(code, "co_filename", "")
    lineno = getattr(code, "co_firstlineno", 0)
    if not filename or not lineno:
        return ""
    return f"{os.path.basename(filename)}:{lineno}"


def _derive_name(fn: object) -> str:
    """Derive a task name: the callable's qualified name plus, when available,
    the source location where it is defined (e.g. ``"mod.fn (worker.py:42)"``)."""
    qualname = getattr(fn, "__qualname__", None)
    if qualname:
        module = getattr(fn, "__module__", None)
        base = f"{module}.{qualname}" if module and module != "__main__" else str(qualname)
    else:
        name = getattr(fn, "__name__", None)
        base = str(name) if name else type(fn).__name__
    location = _source_location(fn)
    return f"{base} ({location})" if location else base


class TaskHandle(Generic[T_co]):
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
        native: "Optional[_NativeTaskHandle[T_co]]" = None,
        future: "Optional[Future[T_co]]" = None,
        name: str = "",
        task_id: int = -1,
    ) -> None:
        self._native = native
        self._future = future
        self._name = name
        self._task_id = task_id
        self._exception: Optional[BaseException] = None

    def get(self) -> T_co:
        """Block until task completes and return result. Raises on error."""
        if self._native is not None:
            return self._native.get()
        if self._future is not None:
            return self._future.result()
        return None  # type: ignore[return-value]  # ty: ignore[invalid-return-type]  # no backing future

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
    """A handle to the process runtime with async task submission and Python
    callable support.

    The process has one native runtime. Making a ``Runtime`` configures it in
    place (``threads`` and ``io_threads``); ``shutdown()`` and leaving a
    ``with`` block release this handle and leave the runtime running.

    Wraps the C++ Runtime and adds:

    - ``submit()`` for both C++ coroutine tasks and Python callables
    - ``wait_all()`` across both C++ and Python tasks
    - Error tracking and callbacks

    Example::

        with Runtime(threads=8, io_threads=8, python_threads=4) as rt:
            h = rt.submit(lambda x: x * 2, 21)
            assert h.get() == 42

    Args:
        threads: C++ executor threads (0 keeps the current count, the
            hardware concurrency for a new runtime).
        io_threads: C++ I/O threads, which only grow (0 keeps the count).
        python_threads: Number of Python ThreadPoolExecutor threads
            (0 = min(32, threads)).
    """

    def __init__(self, threads: int = 0, io_threads: int = 0, python_threads: int = 0) -> None:
        self._native = _NativeRuntime(threads=threads, io_threads=io_threads)
        self._init_fields(python_threads)

    def _init_fields(self, python_threads: int = 0) -> None:
        self._py_pool: Optional[ThreadPoolExecutor] = None
        self._py_pool_size = python_threads
        # TaskHandle[object]: these collections hold handles of every submitted
        # task, whose result types are heterogeneous and not tracked per handle.
        self._handles: List[TaskHandle[object]] = []
        self._failed_handles: List[TaskHandle[object]] = []
        self._lock = threading.Lock()
        self._on_task_error: Optional[Callable[[TaskHandle[object], BaseException], None]] = None
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
    def submit(self, task_or_fn: _NativeTaskHandle) -> TaskHandle[object]: ...
    @overload
    def submit(
        self, task_or_fn: Callable[P, R], *args: P.args, **kwargs: P.kwargs
    ) -> TaskHandle[R]: ...

    # Erased dispatcher; the overloads carry the precise contract. There is no
    # name= arg because PEP 612 forbids a keyword alongside **P.kwargs, so the
    # name is derived from the callable and its source location.
    def submit(
        self,
        task_or_fn: "Union[_NativeTaskHandle, Callable[..., object]]",
        *args: object,
        **kwargs: object,
    ) -> TaskHandle[object]:
        """Submit a task for async execution.

        Accepts either a C++ TaskHandle (pass-through) or a Python callable; the
        callable's args are passed through and type-checked against it. The task
        name is derived automatically (qualified name plus source location).

        Example::

            h = rt.submit(lambda x, y: x + y, 3, 4)
            result = h.get()  # 7
        """
        if isinstance(task_or_fn, _NativeTaskHandle):
            # C++ coroutine path: wrap a native TaskHandle from utility
            # bindings. Currently unused — will be used when utilities
            # are ported to Python.
            handle: TaskHandle[object] = TaskHandle(native=task_or_fn, name=task_or_fn.name)
            with self._lock:
                self._handles.append(handle)
            return handle

        if not callable(task_or_fn):
            raise TypeError(f"Expected callable or TaskHandle, got {type(task_or_fn).__name__}")

        with self._lock:
            task_id = self._py_task_counter
            self._py_task_counter += 1

        handle = TaskHandle[object](name=_derive_name(task_or_fn), task_id=task_id)

        # The result is opaque at the pool layer; callers retrieve it typed
        # through the handle's overloaded get().
        def wrapper() -> object:
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

    def wait(self, handle: TaskHandle[object]) -> None:
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

    def get_failed(self) -> List[TaskHandle[object]]:
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
        callback: Optional[Callable[[TaskHandle[object], BaseException], None]],
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
        """Release this handle: wait for its tasks and stop its Python threads.

        The process runtime keeps running until the process exits.

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

    def get_progress(self) -> RuntimeProgress:
        """Return progress dict from C++ executor."""
        return cast(RuntimeProgress, self._native.get_progress())

    def is_responsive(self) -> bool:
        """Return True if the runtime is making progress."""
        return self._native.is_responsive()

    def set_timeout(self, global_ms: Union[int, float, str] = 0) -> None:
        """Set the global timeout. A bare number is milliseconds; a string such
        as "30s" or "5m" is converted."""
        ms = int(round(coerce_duration(global_ms, 1e3, "global_ms")))
        self._native.set_timeout(global_ms=ms)

    def set_default_task_timeout(self, ms: Union[int, float, str] = 0) -> None:
        """Set the default per-task timeout. A bare number is milliseconds; a
        string such as "30s" or "5m" is converted."""
        ms_val = int(round(coerce_duration(ms, 1e3, "ms")))
        self._native.set_default_task_timeout(ms=ms_val)

    @property
    def threads(self) -> int:
        """Number of C++ worker threads."""
        return self._native.threads

    @property
    def io_threads(self) -> int:
        """Number of C++ I/O threads."""
        return self._native.io_threads

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
