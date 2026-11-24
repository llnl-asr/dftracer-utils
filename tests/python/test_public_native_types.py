# SPDX-License-Identifier: MIT
"""The native types are the public classes: Series is the extension's Series,
subclassed in Python, and the native factories build that subclass directly."""

import copy
import gc
import pickle
import subprocess
import sys
import textwrap

import pytest

import dftracer.utils as d
from dftracer.utils import dftracer_utils_ext as _ext
from dftracer.utils._series import Series


def test_series_subclasses_the_native_series_with_no_instance_dict():
    s = Series([1, 2, 3])
    assert isinstance(s, _ext.Series)
    assert type(s) is Series
    assert Series.__slots__ == ()
    assert not hasattr(s, "__dict__")
    with pytest.raises(AttributeError):
        s.extra = 1  # type: ignore[attr-defined]


def test_native_methods_return_the_public_class_with_no_wrapper():
    s = Series([1, -2, 3])
    assert type(s.abs()) is Series
    assert type(s.head(2)) is Series
    assert type(s.sort()) is Series
    assert type(s.is_nan()) is Series
    assert type(Series(["a", "bb"]).str_len_bytes()) is Series
    assert type(d.DataFrame({"x": [1, 2]})["x"]) is Series
    assert type(_ext._series_from_list([1, 2])) is Series
    assert "_native" not in dir(Series)


def test_the_native_type_has_no_constructor_and_no_public_underscore_name():
    with pytest.raises(TypeError):
        _ext.Series()
    for old in ("_Series", "_DataFrame", "_LazyFrame", "_TraceViewer"):
        assert not hasattr(_ext, old)
    for name in ("Series", "DataFrame", "LazyFrame", "TraceViewer"):
        assert getattr(_ext, name).__flags__ & (1 << 10), f"{name} is not subclassable"


def test_constructor_forms():
    assert Series([1, 2, 3]).to_list() == [1, 2, 3]
    assert Series(["a", None]).to_list() == ["a", None]
    assert Series([], dtype=d.DType.INT64).dtype == d.DType.INT64
    named = Series(name="n", data=[1, 2])
    assert named.name == "n"
    s = Series([1, 2])
    shared = Series(s, name="other")
    assert shared.name == "other" and s.name is None
    assert Series(s).to_list() == [1, 2]
    assert Series([1, 2], dtype=d.DType.FLOAT64).to_list() == [1.0, 2.0]


def test_name_is_set_in_place_and_only_on_that_object():
    s = Series([1, 2], name="x")
    view = s.head(1)
    twin = s.share()
    s.name = "y"
    assert s.name == "y"
    assert view.name == "x" and twin.name == "x"
    s.name = None
    assert s.name is None
    with pytest.raises(TypeError, match="a Series name must be a str or None, not int"):
        s.name = 5  # type: ignore[assignment]
    with pytest.raises(TypeError):
        del s.name
    with pytest.raises(TypeError, match="rename: the name must be a str or None"):
        s.rename(3)  # type: ignore[arg-type]
    assert s.rename("z").name == "z" and s.name is None


def test_series_is_generic_at_run_time():
    assert Series[int] is not None
    if sys.version_info >= (3, 9):
        assert Series[int].__origin__ is Series


def test_pickle_and_copy_keep_the_class_and_values():
    s = Series([1, 2, None, 4])
    for out in (pickle.loads(pickle.dumps(s)), copy.copy(s), copy.deepcopy(s)):
        assert type(out) is Series
        assert out.to_list() == [1, 2, None, 4]


def test_instances_free_without_leaking_the_class_reference():
    base = sys.getrefcount(Series)
    for _ in range(20000):
        Series([1, 2, 3]).abs().head(2)
    gc.collect()
    assert sys.getrefcount(Series) == base


def test_bind_types_checks_each_class():
    class Mine(_ext.Series):
        __slots__ = ()

    with pytest.raises(TypeError, match="Series must be a subclass of"):
        _ext._bind_types(Series=int)
    with pytest.raises(TypeError, match="DataFrame must be a subclass of"):
        _ext._bind_types(DataFrame=Mine)
    with pytest.raises(TypeError):
        _ext._bind_types(Series)  # type: ignore[call-arg]
    with pytest.raises(TypeError):
        _ext._bind_types(Mystery=Series)  # type: ignore[call-arg]
    _ext._bind_types()
    _ext._bind_types(Series=Series)
    assert type(Series([1]).abs()) is Series


def test_bound_subclasses_are_what_every_factory_builds():
    code = textwrap.dedent(
        """
        import gc
        import dftracer.utils as d
        from dftracer.utils import dftracer_utils_ext as e

        class S(e.Series):
            __slots__ = ()
        class D(e.DataFrame):
            __slots__ = ()
        class L(e.LazyFrame):
            __slots__ = ()
        class T(e.TraceViewer):
            __slots__ = ()

        e._bind_types(Series=S, DataFrame=D, LazyFrame=L, TraceViewer=T)
        s = e._series_from_list([1, -2, 3])
        assert type(s) is S and type(s.abs()) is S and type(s.head(2)) is S
        frame = e._dataframe_from_columns(["x"], [s])
        assert type(frame) is D and type(frame["x"]) is S
        assert type(frame.lazy()) is L
        assert type(e.scan_file("/no/such/file.pfw.gz")) is L
        tv = T.__mro__[1]
        assert tv is e.TraceViewer
        for _ in range(5000):
            e._series_from_list([1, 2]).abs()
        gc.collect()
        e._bind_types(Series=e.Series, DataFrame=e.DataFrame, LazyFrame=e.LazyFrame)
        assert type(e._series_from_list([1])) is e.Series
        print("ok")
        """
    )
    out = subprocess.run(
        [sys.executable, "-I", "-c", code], capture_output=True, text=True, check=False
    )
    assert out.returncode == 0 and "ok" in out.stdout, out.stdout + out.stderr


def test_comparison_and_arithmetic_return_the_public_class():
    s = Series([1, 2, 3])
    for out in (s > 1, s == 2, s + 1, s * s, s / 2, s // 2, -1 + s, s.rolling_mean(2)):
        assert type(out) is Series
    assert (s > 1).to_list() == [False, True, True]


def test_a_user_subclass_of_series_is_refused():
    class Mine(Series):
        __slots__ = ()

    with pytest.raises(TypeError, match="cannot be subclassed"):
        Mine([1, 2])
    assert type(Series([1, 2])) is Series
