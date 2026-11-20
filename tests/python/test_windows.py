# SPDX-License-Identifier: MIT
"""The rolling and ewm windows are native types: Series.rolling(n) is a Rolling
and Series.ewm(...) is an Ewm, both built in C++ and holding their Series."""

import gc
import threading
import weakref

import pytest

import dftracer.utils._series as series_module
from dftracer.utils import DataFrame
from dftracer.utils import dftracer_utils_ext as _ext
from dftracer.utils._series import Series


def _s(values=(1.0, 2.0, 3.0, 4.0, 5.0), name="x"):
    return Series(list(values), name=name)


def test_rolling_and_ewm_are_native_types():
    s = _s()
    assert type(s.rolling(3)) is _ext.Rolling
    assert type(s.ewm(alpha=0.5)) is _ext.Ewm
    assert not hasattr(series_module, "_Rolling")
    assert not hasattr(series_module, "_Ewm")
    assert not hasattr(series_module, "ewm_alpha")


@pytest.mark.parametrize("cls", [_ext.Rolling, _ext.Ewm])
def test_a_window_is_built_by_a_series_only(cls):
    with pytest.raises(TypeError):
        cls()


def test_rolling_methods_reduce_each_trailing_window():
    r = _s().rolling(3)
    nan = None
    assert r.sum().to_list() == [nan, nan, 6.0, 9.0, 12.0]
    assert r.mean().to_list() == [nan, nan, 2.0, 3.0, 4.0]
    assert r.min().to_list() == [nan, nan, 1.0, 2.0, 3.0]
    assert r.max().to_list() == [nan, nan, 3.0, 4.0, 5.0]
    assert r.var().to_list() == [nan, nan, 1.0, 1.0, 1.0]
    assert r.std().to_list() == [nan, nan, 1.0, 1.0, 1.0]
    assert r.median().to_list() == [nan, nan, 2.0, 3.0, 4.0]
    assert r.quantile(0.0).to_list() == r.min().to_list()
    assert r.quantile(1.0).to_list() == r.max().to_list()
    assert r.quantile(q=0.5).to_list() == r.median().to_list()


def test_a_window_with_a_null_is_null_and_the_name_is_kept():
    s = _s([1.0, None, 3.0, 4.0, 5.0], name="v")
    out = s.rolling(2).sum()
    assert out.to_list() == [None, None, None, 7.0, 9.0]
    assert out.name == "v"


def test_the_op_form_and_the_flat_methods_equal_the_window_methods():
    s = _s()
    for op in ("sum", "mean", "min", "max"):
        want = getattr(s.rolling(3), op)().to_list()
        assert s.rolling(3, op).to_list() == want
        assert s.rolling(window=3, op=op).to_list() == want
        assert getattr(s, f"rolling_{op}")(3).to_list() == want
        assert getattr(s, f"rolling_{op}")(window=3).to_list() == want
    assert s.rolling_var(3).to_list() == s.rolling(3).var().to_list()
    assert s.rolling_quantile(3, 0.25).to_list() == s.rolling(3).quantile(0.25).to_list()
    with pytest.raises(ValueError, match="rolling op must be sum|mean|min|max"):
        s.rolling(3, "median")


def test_ewm_parameters_give_one_smoothing_factor():
    assert _ext._ewm_alpha(alpha=0.25) == 0.25
    assert _ext._ewm_alpha(span=3) == 0.5
    assert _ext._ewm_alpha(com=1) == 0.5
    assert _ext._ewm_alpha(halflife=1) == 0.5
    s = _s()
    want = s.ewm(alpha=0.5)
    for other in (s.ewm(span=3), s.ewm(com=1), s.ewm(halflife=1), s.ewm(0.5)):
        assert other.mean().to_list() == want.mean().to_list()
        assert other.std().to_list() == want.std().to_list()
    assert s.ewm(alpha=0.5).mean().to_list() == s.ewm_mean(0.5).to_list()
    assert s.ewm(alpha=0.5).std().to_list() == s.ewm_std(alpha=0.5).to_list()
    assert s.ewm(alpha=1.0).mean().to_list() == s.to_list()


@pytest.mark.parametrize(
    "kwargs,message",
    [
        ({}, "exactly one"),
        ({"alpha": 0.5, "span": 3}, "exactly one"),
        ({"alpha": 0.0}, r"alpha must be in \(0, 1\]"),
        ({"alpha": 1.5}, r"alpha must be in \(0, 1\]"),
        ({"span": 0.5}, "span must be >= 1"),
        ({"com": -1}, "com must be >= 0"),
        ({"halflife": 0}, "halflife must be > 0"),
    ],
)
def test_a_bad_ewm_parameter_is_a_value_error(kwargs, message):
    with pytest.raises(ValueError, match=message):
        _s().ewm(**kwargs)
    with pytest.raises(ValueError, match=message):
        _ext._ewm_alpha(**kwargs)


def test_a_window_checks_its_arguments_when_it_is_built():
    s = _s()
    with pytest.raises(TypeError):
        s.rolling("3")
    with pytest.raises(TypeError):
        s.rolling()
    with pytest.raises(TypeError, match="unexpected keyword argument 'min_periods'"):
        s.rolling(3, min_periods=1)
    with pytest.raises(TypeError):
        s.ewm(span="a")
    with pytest.raises(TypeError):
        s.ewm(0.5, 0.5)
    with pytest.raises(TypeError):
        s.rolling(3).sum(1)
    with pytest.raises(TypeError):
        s.rolling(3).quantile()


def test_a_window_holds_its_series_and_dies_with_its_last_reference():
    for build in (lambda s: s.rolling(2), lambda s: s.ewm(alpha=0.5)):
        s = _s()
        w = build(s)
        assert w._s is s
        calls = []
        ref = weakref.ref(w, calls.append)
        del w
        gc.collect()
        assert ref() is None and len(calls) == 1
        assert s.to_list() == [1.0, 2.0, 3.0, 4.0, 5.0]


def test_a_window_keeps_the_series_alive():
    w = _s().rolling(2)
    gc.collect()
    assert w.sum().to_list() == [None, 3.0, 5.0, 7.0, 9.0]


def test_a_renamed_long_series_gives_each_result_one_whole_name():
    n = 40000
    s = Series([float(i) for i in range(n)], name="a")
    r = s.rolling(3)
    names = []
    stop = threading.Event()

    def work():
        while not stop.is_set():
            names.append(r.mean().name)

    threads = [threading.Thread(target=work) for _ in range(3)]
    for t in threads:
        t.start()
    for i in range(200):
        s.name = "b" if i % 2 else "a"
    stop.set()
    for t in threads:
        t.join()
    assert names and set(names) <= {"a", "b"}


def test_a_frame_window_checks_ewm_parameters_when_it_is_built():
    frame = DataFrame({"a": [1.0, 2.0, 3.0], "s": ["x", "y", "z"]})
    with pytest.raises(ValueError, match="exactly one"):
        frame.ewm()
    with pytest.raises(ValueError, match="alpha must be"):
        frame.ewm(alpha=5)
    assert frame.ewm(span=3).mean()["a"].to_list() == frame["a"].ewm(alpha=0.5).mean().to_list()
    assert frame.rolling(2).sum()["a"].to_list() == [None, 3.0, 5.0]


def test_a_group_window_checks_ewm_parameters_when_it_is_built():
    frame = DataFrame({"k": [1, 1, 2], "a": [1.0, 2.0, 3.0]})
    with pytest.raises(ValueError, match="exactly one"):
        frame.group_by("k").ewm()
    with pytest.raises(ValueError, match="span must be >= 1"):
        frame.group_by("k").ewm(span=0.5)
    got = frame.group_by("k").ewm(span=3).mean()["a"].to_list()
    assert got == pytest.approx([1.0, 5.0 / 3.0, 3.0])
