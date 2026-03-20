"""Tests for TaskHandle and Runtime submit/wait_all functionality."""

import time
import threading
import pytest
import dftracer.utils as dft_utils
from dftracer.utils import Runtime, TaskHandle


class TestTaskHandleBasic:
    """Basic TaskHandle functionality."""

    def test_submit_callable_returns_task_handle(self):
        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(lambda: 42)
            assert isinstance(h, TaskHandle)

    def test_get_returns_value(self):
        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(lambda: 42)
            assert h.get() == 42

    def test_wait_completes(self):
        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(lambda: 42)
            h.wait()  # should not raise
            assert h.done()

    def test_done_true_after_completion(self):
        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(lambda: 42)
            h.wait()
            assert h.done() is True

    def test_get_with_args(self):
        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(lambda x, y: x + y, 3, 4)
            assert h.get() == 7

    def test_get_with_kwargs(self):
        def add(a, b=10):
            return a + b

        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(add, 5, b=20)
            assert h.get() == 25

    def test_get_none_for_void_callable(self):
        def void_fn():
            pass

        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(void_fn)
            assert h.get() is None

    def test_submit_non_callable_raises(self):
        with Runtime(threads=2, python_threads=2) as rt:
            with pytest.raises(TypeError):
                rt.submit(42)


class TestTaskHandleNaming:
    """Name auto-derivation tests."""

    def test_explicit_name(self):
        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(lambda: 1, name="my-task")
            assert h.name == "my-task"

    def test_auto_name_from_function(self):
        def my_function():
            return 1

        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(my_function)
            assert "my_function" in h.name

    def test_auto_name_from_lambda(self):
        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(lambda: 1)
            assert "<lambda>" in h.name

    def test_auto_name_from_method(self):
        class MyClass:
            def my_method(self):
                return 1

        obj = MyClass()
        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(obj.my_method)
            assert "my_method" in h.name

    def test_task_id_is_int(self):
        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(lambda: 1)
            assert isinstance(h.task_id, int)

    def test_task_ids_are_unique(self):
        with Runtime(threads=2, python_threads=2) as rt:
            h1 = rt.submit(lambda: 1)
            h2 = rt.submit(lambda: 2)
            assert h1.task_id != h2.task_id


class TestWaitAll:
    """wait_all() tests."""

    def test_wait_all_no_tasks(self):
        with Runtime(threads=2, python_threads=2) as rt:
            rt.wait_all()  # should not raise

    def test_wait_all_waits_for_all(self):
        results = []

        def slow_task(val):
            time.sleep(0.05)
            results.append(val)
            return val

        with Runtime(threads=2, python_threads=4) as rt:
            for i in range(10):
                rt.submit(slow_task, i)
            rt.wait_all()
            assert len(results) == 10

    def test_wait_all_after_done_is_noop(self):
        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(lambda: 42)
            h.wait()
            rt.wait_all()  # should not raise

    def test_wait_all_multiple_cycles(self):
        with Runtime(threads=2, python_threads=2) as rt:
            rt.submit(lambda: 1)
            rt.wait_all()

            rt.submit(lambda: 2)
            rt.wait_all()

    def test_concurrent_submits_all_complete(self):
        with Runtime(threads=2, python_threads=4) as rt:
            handles = [
                rt.submit(lambda i=i: i * 2, name=f"task-{i}") for i in range(20)
            ]
            rt.wait_all()
            for i, h in enumerate(handles):
                assert h.get() == i * 2


class TestErrorHandling:
    """Error handling tests."""

    def test_get_raises_on_error(self):
        def failing():
            raise ValueError("test error")

        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(failing, name="fail")
            with pytest.raises(ValueError, match="test error"):
                h.get()

    def test_wait_raises_on_error(self):
        def failing():
            raise ValueError("test error")

        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(failing, name="fail")
            with pytest.raises(ValueError, match="test error"):
                h.wait()

    def test_wait_all_default_no_raise(self):
        def failing():
            raise ValueError("test error")

        with Runtime(threads=2, python_threads=2) as rt:
            rt.submit(failing, name="fail")
            rt.wait_all()  # should NOT raise

    def test_wait_all_raise_on_error(self):
        def failing():
            raise ValueError("test error")

        with Runtime(threads=2, python_threads=2) as rt:
            rt.submit(failing, name="fail")
            with pytest.raises(RuntimeError, match="1 task.*failed"):
                rt.wait_all(raise_on_error=True)

    def test_wait_all_raise_waits_for_all(self):
        """raise_on_error=True should wait for ALL tasks, not fail-fast."""
        completed = []

        def slow_ok(val):
            time.sleep(0.05)
            completed.append(val)

        def failing():
            raise ValueError("boom")

        with Runtime(threads=2, python_threads=4) as rt:
            rt.submit(slow_ok, "a")
            rt.submit(failing, name="fail")
            rt.submit(slow_ok, "b")
            try:
                rt.wait_all(raise_on_error=True)
            except RuntimeError:
                pass
            assert "a" in completed
            assert "b" in completed

    def test_get_failed_returns_failed_handles(self):
        def failing():
            raise ValueError("test error")

        with Runtime(threads=2, python_threads=2) as rt:
            rt.submit(failing, name="fail-1")
            rt.submit(failing, name="fail-2")
            rt.wait_all()
            failed = rt.get_failed()
            assert len(failed) == 2
            names = {h.name for h in failed}
            assert "fail-1" in names
            assert "fail-2" in names

    def test_get_failed_empty_on_success(self):
        with Runtime(threads=2, python_threads=2) as rt:
            rt.submit(lambda: 42)
            rt.wait_all()
            assert len(rt.get_failed()) == 0

    def test_clear_failed(self):
        def failing():
            raise ValueError("test error")

        with Runtime(threads=2, python_threads=2) as rt:
            rt.submit(failing, name="fail")
            rt.wait_all()
            assert len(rt.get_failed()) == 1
            rt.clear_failed()
            assert len(rt.get_failed()) == 0

    def test_error_callback_invoked(self):
        errors = []

        def failing():
            raise ValueError("test error")

        with Runtime(threads=2, python_threads=2) as rt:
            rt.set_error_callback(lambda h, e: errors.append((h.name, str(e))))
            rt.submit(failing, name="cb-test")
            rt.wait_all()
            assert len(errors) == 1
            assert errors[0][0] == "cb-test"
            assert "test error" in errors[0][1]

    def test_error_callback_clear(self):
        errors = []

        def failing():
            raise ValueError("test error")

        with Runtime(threads=2, python_threads=2) as rt:
            rt.set_error_callback(lambda h, e: errors.append(1))
            rt.set_error_callback(None)
            rt.submit(failing, name="no-cb")
            rt.wait_all()
            assert len(errors) == 0

    def test_exception_preserved_in_handle(self):
        def failing():
            raise ValueError("preserved")

        with Runtime(threads=2, python_threads=2) as rt:
            h = rt.submit(failing, name="exc")
            rt.wait_all()
            assert h.exception is not None
            assert isinstance(h.exception, ValueError)


class TestRuntimeLifecycle:
    """Runtime lifecycle tests."""

    def test_context_manager(self):
        with Runtime(threads=2) as rt:
            h = rt.submit(lambda: 42)
            assert h.get() == 42

    def test_shutdown_is_clean(self):
        rt = Runtime(threads=2, python_threads=2)
        rt.submit(lambda: 42)
        rt.wait_all()
        rt.shutdown()

    def test_python_threads_param(self):
        with Runtime(threads=4, python_threads=8) as rt:
            rt.submit(lambda: 1).get()
            assert rt.python_threads == 8

    def test_python_threads_default(self):
        with Runtime(threads=4) as rt:
            assert rt.python_threads == 0
            rt.submit(lambda: 1).get()
            assert rt.python_threads == min(32, 4)

    def test_threads_property(self):
        with Runtime(threads=4) as rt:
            assert rt.threads == 4

    def test_nested_submit(self):
        """Python callable that internally submits another callable."""
        with Runtime(threads=2, python_threads=4) as rt:

            def outer():
                h = rt.submit(lambda: 42, name="inner")
                return h.get()

            h = rt.submit(outer, name="outer")
            assert h.get() == 42

    def test_handle_as_input_get_pattern(self):
        """Pass handle.get() result to next task (resolve-then-pass)."""
        with Runtime(threads=2, python_threads=4) as rt:

            def step1():
                return 10

            def step2(value):
                return value * 3

            h1 = rt.submit(step1, name="step1")
            h2 = rt.submit(step2, h1.get(), name="step2")
            assert h2.get() == 30

    def test_handle_passed_directly(self):
        """Pass handle directly; callee calls .get() internally."""
        with Runtime(threads=2, python_threads=4) as rt:

            def producer():
                return 7

            def consumer(handle_or_value):
                if isinstance(handle_or_value, TaskHandle):
                    val = handle_or_value.get()
                else:
                    val = handle_or_value
                return val + 1

            h1 = rt.submit(producer, name="producer")
            h2 = rt.submit(consumer, h1, name="consumer")
            assert h2.get() == 8

    def test_chain_three_tasks(self):
        """Chain three tasks: A -> B -> C via handle passing."""
        with Runtime(threads=2, python_threads=4) as rt:

            def compose():
                h1 = rt.submit(lambda: 5, name="a")
                h2 = rt.submit(lambda v: v * 2, h1.get(), name="b")
                h3 = rt.submit(lambda v: v + 100, h2.get(), name="c")
                return h3.get()

            h = rt.submit(compose, name="chain")
            assert h.get() == 110  # (5 * 2) + 100
