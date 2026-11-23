#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""``@dft.udf``: a Python function as a scalar UDF, compiled (JIT) or run per batch.

The JIT form is lowered to native code and never takes the GIL. The batch form
(``batch=True``) is called once per batch with numpy views of the columns, on one
dedicated Python lane thread.
"""

import gzip
import json
import math
import subprocess
import sys
import textwrap
import threading

import pytest

pa = pytest.importorskip("pyarrow")
np = pytest.importorskip("numpy")
import numpy.typing as npt  # noqa: E402

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import DataFrame, col  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402
from dftracer.utils.jit import JitError  # noqa: E402

pytestmark = pytest.mark.skipif(
    not hasattr(_ext, "LazyFrame") or not hasattr(_ext, "udf_register_batch"),
    reason="extension built without Arrow support",
)


@pytest.fixture(autouse=True)
def _scope():
    with dft.udf.scope():
        yield


def _frame(**columns):
    return DataFrame.from_arrow(pa.table(columns))


def _collect(df, **exprs):
    return df.lazy().with_columns(**exprs).collect().to_arrow().to_pydict()


def _same(got, want):
    assert len(got) == len(want)
    for g, w in zip(got, want):
        if w is None:
            assert g is None
        elif isinstance(w, float) and math.isnan(w):
            assert g is not None and math.isnan(g)
        else:
            assert g == pytest.approx(w)


# ---------------------------------------------------------------- JIT form


def _py_score(x, n):
    if n > 2 and x > 1.0:
        return x * 2.0 + math.sqrt(abs(x))
    elif n == 2:
        return min(x, 10.0) - n
    y = x - 1.0
    return y if y > 0 else -y


def test_jit_udf_equals_the_python_function_with_nulls():
    @dft.udf
    def score(x: float, n: int) -> float:
        if n > 2 and x > 1.0:
            return x * 2.0 + math.sqrt(abs(x))
        elif n == 2:
            return min(x, 10.0) - n
        y = x - 1.0
        return y if y > 0 else -y

    xs = [0.5, 4.0, None, 9.0, -3.0, 12.5, 2.0, None]
    ns = [1, 3, 2, 5, 3, 2, None, 4]
    out = _collect(_frame(x=xs, n=ns), s=score(col("x"), col("n")))
    want = [None if x is None or n is None else _py_score(x, n) for x, n in zip(xs, ns)]
    _same(out["s"], want)


def test_jit_udf_matches_an_equivalent_expr():
    @dft.udf
    def mix(d: int, p: int) -> float:
        return d * 0.5 + p - 3

    df = _frame(d=list(range(1000)), p=[i % 7 for i in range(1000)])
    via_udf = _collect(df, m=mix(col("d"), col("p")))["m"]
    via_expr = _collect(df, m=col("d") * 0.5 + col("p") - 3)["m"]
    _same(via_udf, via_expr)


def test_jit_udf_widens_narrow_and_unsigned_columns():
    @dft.udf
    def total(a: float, b: int) -> float:
        return a + b

    df = DataFrame.from_arrow(
        pa.table(
            {
                "a": pa.array([1, 2, 3], pa.int8()),
                "f": pa.array([0.5, 1.5, 2.5], pa.float32()),
                "u": pa.array([10, 20, 30], pa.uint32()),
            }
        )
    )
    out = _collect(df, t=total(col("a"), col("u")), g=total(col("f"), col("u")))
    assert out["t"] == [11.0, 22.0, 33.0]
    assert out["g"] == [10.5, 21.5, 32.5]


def test_jit_udf_returns_int_and_bool():
    @dft.udf
    def bucket(x: int) -> int:
        return x // 10 + x % 10

    @dft.udf
    def is_big(x: float, limit: float) -> bool:
        return x > limit and not (x > 100.0)

    xs = [3, 17, 25, -4, None]
    out = _collect(_frame(x=xs), b=bucket(col("x")))
    assert out["b"] == [None if x is None else x // 10 + x % 10 for x in xs]

    fs = [1.0, 50.0, 500.0, None]
    out = _collect(_frame(x=fs), m=is_big(col("x"), 10.0))
    assert out["m"] == [False, True, False, None]


def test_jit_udf_reads_a_filtered_frame():
    @dft.udf
    def triple(x: int) -> int:
        return x * 3

    df = _frame(x=list(range(20)))
    kept = df.lazy().filter(col("x") > 12).with_columns(t=triple(col("x")))
    assert kept.collect().to_arrow().to_pydict()["t"] == [x * 3 for x in range(13, 20)]


def test_jit_udf_arguments_are_checked_at_bind():
    @dft.udf
    def whole(n: int) -> int:
        return n + 1

    with pytest.raises(Exception, match="argument 1 is Float64, but the function declares int"):
        _collect(_frame(x=[1.5]), w=whole(col("x")))


def test_jit_int_division_by_zero_fails_the_call():
    @dft.udf
    def per(a: int, b: int) -> int:
        return a // b

    out = _collect(_frame(a=[7, -7], b=[2, 2]), q=per(col("a"), col("b")))
    assert out["q"] == [3, -4]
    with pytest.raises(Exception, match="division or modulo by zero.*per"):
        _collect(_frame(a=[7], b=[0]), q=per(col("a"), col("b")))


def test_jit_float_floor_division_and_modulo_follow_python():
    @dft.udf
    def fdiv(a: float, b: float) -> float:
        return a // b

    @dft.udf
    def fmod(a: float, b: float) -> float:
        return a % b

    a = [1.0, 7.5, -7.5, 7.5, -7.5, 0.3, 5.0, 1e18, 0.0, -0.0]
    b = [0.1, 2.0, 2.0, -2.0, -2.0, 0.1, 0.5, 3.0, 2.0, -2.0]
    df = _frame(a=a, b=b)
    out = _collect(df, q=fdiv(col("a"), col("b")), m=fmod(col("a"), col("b")))
    _same(out["q"], [x // y for x, y in zip(a, b)])
    _same(out["m"], [x % y for x, y in zip(a, b)])
    assert out["q"][0] == 9.0


def test_jit_float_division_by_zero_fails_the_call_like_python():
    @dft.udf
    def zdiv(a: float, b: float) -> float:
        return a / b

    @dft.udf
    def zfloordiv(a: float, b: float) -> float:
        return a // b

    @dft.udf
    def zmod(a: float, b: float) -> float:
        return a % b

    for fn in (zdiv, zfloordiv, zmod):
        with pytest.raises(Exception, match=f"division or modulo by zero.*{fn.name}"):
            _collect(_frame(a=[1.0], b=[0.0]), r=fn(col("a"), col("b")))
    # a null divisor row is skipped, not an error
    out = _collect(_frame(a=[1.0, 4.0], b=[None, 2.0]), r=zdiv(col("a"), col("b")))
    assert out["r"] == [None, 2.0]


def test_jit_int_argument_takes_a_uint64_column_and_range_checks_it():
    @dft.udf
    def plus(a: int, n: int) -> int:
        return n + a

    ok = DataFrame.from_arrow(
        pa.table(
            {
                "a": pa.array([1, 2, 3], pa.int64()),
                "u": pa.array([5, 2**63 - 1, None], pa.uint64()),
            }
        )
    )
    out = _collect(ok, r=plus(col("a"), col("u")))
    assert out["r"][0] == 6 and out["r"][2] is None
    assert out["r"][1] == (2**63 - 1) + 2 - 2**64  # wraps at 64 bits like any int

    big = DataFrame.from_arrow(pa.table({"a": [1, 2], "u": pa.array([7, 2**63 + 5], pa.uint64())}))
    with pytest.raises(Exception, match=r"argument 2 has the value 9223372036854775813 at row 1"):
        _collect(big, r=plus(col("a"), col("u")))

    # a null slot is not a value: its bytes are ignored
    nulled = DataFrame.from_arrow(pa.table({"a": [1, 2], "u": pa.array([7, None], pa.uint64())}))
    assert _collect(nulled, r=plus(col("a"), col("u")))["r"] == [8, None]


def test_jit_udf_runs_in_duql_by_name(tmp_path):
    @dft.udf
    def half(x: int) -> float:
        return x / 2

    events = [
        {"ph": 1, "name": "read", "cat": "POSIX", "pid": 1, "tid": i, "ts": i, "dur": i * 7}
        for i in range(40)
    ]
    path = tmp_path / "t.pfw.gz"
    path.write_bytes(gzip.compress(("\n".join(json.dumps(e) for e in events) + "\n").encode()))
    got = (
        dft.trace.TraceViewer(str(path))
        .duql("derive h = half(dur) | where h > 100 | select tid, h")
        .collect()
        .to_dict()
    )
    want = [(e["tid"], e["dur"] / 2) for e in events if e["dur"] / 2 > 100]
    assert sorted(zip(got["tid"], got["h"])) == sorted(want)


def test_jit_body_outside_the_subset_is_a_located_error_and_not_registered():
    with pytest.raises(JitError) as info:

        @dft.udf
        def bad(x: float) -> float:
            return sum([x, x])

    err = info.value
    assert err.file is not None and err.file.endswith("test_udf_python.py")
    assert err.line is not None and err.col is not None
    assert "batch=True" in str(err)
    assert "bad" not in dft.udf


@pytest.mark.parametrize(
    "make",
    [
        lambda: _bad_untyped(),
        lambda: _bad_string_cond(),
        lambda: _bad_falls_off(),
        lambda: _bad_unbound(),
    ],
)
def test_jit_rejects_each_unsupported_shape(make):
    with pytest.raises(JitError):
        make()


def _bad_untyped():
    @dft.udf
    def f1(x):
        return x

    return f1


def _bad_string_cond():
    @dft.udf
    def f2(x: float) -> float:
        if x:
            return 1.0
        return 0.0

    return f2


def _bad_falls_off():
    @dft.udf
    def f3(x: float) -> float:
        if x > 0.0:
            return 1.0

    return f3


def _bad_unbound():
    @dft.udf
    def f4(x: float) -> float:
        if x > 0.0:
            y = 1.0
        return y

    return f4


# -------------------------------------------------------------- batch form


def test_batch_udf_equals_numpy():
    @dft.udf(batch=True)
    def combine(x: np.ndarray, y: np.ndarray) -> npt.NDArray[np.float64]:
        return np.sqrt(x) * 2 + y

    xs = np.arange(1, 5001, dtype=np.float64)
    ys = np.arange(5000, dtype=np.int64)
    out = _collect(_frame(x=xs, y=ys), c=combine(col("x"), col("y")))["c"]
    _same(out, list(np.sqrt(xs) * 2 + ys))


def test_batch_udf_through_expr_apply_does_not_deadlock_on_the_gil():
    @dft.udf(batch=True, returns=np.float64)
    def inc(x):
        return x + 1.0

    # Enough rows that the engine fans the batches out to its workers while the
    # calling thread waits.
    rows = 3_000_000
    result = []
    runner = threading.Thread(
        target=lambda: result.append(
            inc(col("x")).apply(_frame(x=np.arange(rows, dtype=np.float64)))
        ),
        daemon=True,
    )
    runner.start()
    runner.join(60)
    assert not runner.is_alive(), "apply held the GIL while the lane needed it"
    assert len(result[0]) == rows


def test_batch_udf_nulls_follow_the_arguments():
    @dft.udf(batch=True, returns=np.float64)
    def inc(x):
        return x + 1.0

    out = _collect(_frame(x=[1.0, None, 3.0]), r=inc(col("x")))
    assert out["r"] == [2.0, None, 4.0]


def test_batch_udf_gets_read_only_views():
    seen = []

    @dft.udf(batch=True, returns=np.int64)
    def probe(x):
        seen.append((x.flags.writeable, x.dtype.name))
        return x

    _collect(_frame(x=[1, 2, 3]), r=probe(col("x")))
    assert seen and all(w is False and d == "int64" for w, d in seen)


def test_batch_udf_takes_bool_and_gives_bool():
    @dft.udf(batch=True, returns=bool)
    def both(a, b):
        return a & (b > 1)

    out = _collect(_frame(a=[True, False, True, True], b=[0, 5, 2, 1]), r=both(col("a"), col("b")))
    assert out["r"] == [False, False, True, False]


def test_batch_udf_refuses_a_string_column_with_a_typed_error():
    @dft.udf(batch=True, returns=np.int64)
    def length(x):
        return np.zeros(len(x), dtype=np.int64)

    with pytest.raises(Exception, match="argument 1 is not a number or bool column"):
        _collect(_frame(s=["a", "bc"]), r=length(col("s")))


def test_batch_udf_exception_reaches_the_caller_with_the_udf_name():
    @dft.udf(batch=True, returns=np.float64)
    def boom(x):
        raise KeyError("missing thing")

    with pytest.raises(RuntimeError, match=r"udf 'boom'.*KeyError.*missing thing"):
        _collect(_frame(x=[1.0, 2.0]), r=boom(col("x")))


def test_batch_udf_wrong_length_is_a_located_error():
    @dft.udf(batch=True, returns=np.float64)
    def short(x):
        return x[:1]

    with pytest.raises(Exception, match=r"udf 'short'.*returned 1 rows for a batch of 3 rows"):
        _collect(_frame(x=[1.0, 2.0, 3.0]), r=short(col("x")))


def test_batch_udf_wrong_dtype_is_refused_and_safe_casts_are_taken():
    @dft.udf(batch=True, returns=np.int32)
    def narrow(x):
        return x

    @dft.udf(batch=True, returns=np.float64)
    def widen(x):
        return x

    with pytest.raises(Exception, match=r"udf 'narrow'.*float64.*cannot be cast safely.*int32"):
        _collect(_frame(x=[1.0]), r=narrow(col("x")))
    assert _collect(_frame(x=[1, 2]), r=widen(col("x")))["r"] == [1.0, 2.0]


def test_batch_udf_needs_a_declared_result_dtype():
    with pytest.raises(TypeError, match="needs a result dtype"):

        @dft.udf(batch=True)
        def nothing(x: np.ndarray) -> np.ndarray:
            return x


def test_batch_runs_do_not_hold_the_gil_in_workers():
    ident = set()
    inside = [0, 0]
    lock = threading.Lock()

    @dft.udf(batch=True, returns=np.float64)
    def heavy(x):
        with lock:
            inside[0] += 1
            inside[1] = max(inside[1], inside[0])
            ident.add(threading.get_ident())
        # CPU-bound numpy on a large array: it drops the GIL inside its loops
        # and needs it back, so a caller that held the GIL would deadlock.
        y = np.sort(np.sin(x) * np.cos(x))
        with lock:
            inside[0] -= 1
        return y[np.argsort(np.argsort(x))]

    ticks = [0]
    stop = threading.Event()

    def spin():
        while not stop.is_set():
            ticks[0] += 1

    other = threading.Thread(target=spin, daemon=True)
    done = []

    def run():
        df = _frame(x=np.random.default_rng(7).random(4_000_000))
        done.append(df.lazy().with_columns(r=heavy(col("x"))).collect(batch_size=500_000))

    runner = threading.Thread(target=run, daemon=True)
    other.start()
    runner.start()
    runner.join(120)
    stop.set()
    assert not runner.is_alive(), "the batch udf deadlocked on the GIL"
    assert done and done[0].to_arrow().num_rows == 4_000_000
    assert len(ident) == 1 and inside[1] == 1
    # The second Python thread ran while the workers waited for the lane.
    assert ticks[0] > 10_000


def test_the_lane_is_one_thread_for_many_concurrent_runs():
    ident = set()

    @dft.udf(batch=True, returns=np.float64)
    def tag(x):
        ident.add(threading.get_ident())
        return x * 2.0

    before = _ext.udf_lane_stats()
    df = _frame(x=[float(i) for i in range(8000)])
    results = []

    def run():
        results.append(df.lazy().with_columns(r=tag(col("x"))).collect(batch_size=100))

    threads = [threading.Thread(target=run) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(120)
    assert len(results) == 8
    after = _ext.udf_lane_stats()
    assert len(ident) == 1
    assert after["threads_alive"] == 1
    assert after["threads_started"] - before["threads_started"] <= 1
    assert after["batches"] - before["batches"] >= 8 * 80


def test_a_kept_view_stays_valid_and_read_only():
    kept = []

    @dft.udf(batch=True, returns=np.float64)
    def keep(x):
        kept.append(x)
        return x

    def go():
        df = _frame(x=[1.0] * 100_000)
        _collect(df, r=keep(col("x")))

    go()
    import gc

    gc.collect()
    junk = [pa.array(np.full(100_000, -7.0)) for _ in range(5)]
    view = kept[0]
    # The array owns a share of the column, so it never reads freed memory.
    assert view.sum() == float(len(view)) and not view.flags.writeable
    del junk


def test_returning_the_input_view_itself_works():
    @dft.udf(batch=True, returns=np.int64)
    def same(x):
        return x

    out = _collect(_frame(x=list(range(70_000))), r=same(col("x")))
    assert out["r"] == list(range(70_000))


# ------------------------------------------------------- registry and scope


def test_scope_removes_the_names_registered_inside():
    with dft.udf.scope():

        @dft.udf
        def inside(x: float) -> float:
            return x + 1.0

        assert "inside" in dft.udf and dft.udf["inside"] is inside
        assert "inside" in list(dft.udf)

    assert "inside" not in dft.udf
    with pytest.raises(KeyError):
        dft.udf["inside"]
    assert dft.udf.get("inside") is None


def test_nested_scopes_close_only_their_own_udfs():
    @dft.udf
    def outer_f(x: float) -> float:
        return x

    with dft.udf.scope():

        @dft.udf
        def inner_f(x: float) -> float:
            return x

    assert "inner_f" not in dft.udf
    assert "outer_f" in dft.udf


def test_an_empty_nested_scope_does_not_end_the_outer_scope():
    with dft.udf.scope():
        with dft.udf.scope():
            pass

        @dft.udf
        def after_inner(x: float) -> float:
            return x

        assert "after_inner" in dft.udf
    assert "after_inner" not in dft.udf
    with dft.udf.scope():
        with dft.udf.scope():

            @dft.udf
            def deep(x: float) -> float:
                return x

        assert "deep" not in dft.udf


def test_close_twice_is_fine_and_a_closed_udf_cannot_be_called_again():
    @dft.udf
    def once(x: float) -> float:
        return x * 2.0

    expr = once(col("x"))
    once.close()
    once.close()
    assert once.is_closed and "once" not in dft.udf
    with pytest.raises(RuntimeError, match="closed"):
        once(col("x"))
    assert _collect(_frame(x=[1.0, 2.0]), r=expr)["r"] == [2.0, 4.0]


def test_an_expression_keeps_a_closed_batch_udf_running():
    @dft.udf(batch=True, returns=np.float64)
    def twice(x):
        return x * 2.0

    expr = twice(col("x"))
    twice.close()
    assert "twice" not in dft.udf
    assert _collect(_frame(x=[1.0, 2.0]), r=expr)["r"] == [2.0, 4.0]


def test_a_udf_stays_registered_until_closed_not_until_collected():
    import gc

    @dft.udf
    def sticky(x: float) -> float:
        return x

    del sticky
    gc.collect()
    assert "sticky" in dft.udf
    dft.udf["sticky"].close()
    assert "sticky" not in dft.udf


def test_a_name_clash_names_both_definitions():
    @dft.udf
    def dup(x: float) -> float:
        return x

    with pytest.raises(ValueError) as info:

        @dft.udf(batch=True, name="dup", returns=np.float64)
        def other(x):
            return x

    text = str(info.value)
    assert "'dup'" in text and "already registered" in text
    assert text.count("test_udf_python.py") == 2


def test_a_builtin_duql_function_name_is_refused():
    with pytest.raises(ValueError, match="built-in"):

        @dft.udf(name="abs")
        def abs_like(x: float) -> float:
            return x


def test_the_registry_stays_a_read_only_mapping():
    with pytest.raises(TypeError):
        dft.udf["x"] = None  # type: ignore[index]


# ---------------------------------------------------------- interpreter exit

_EXIT_OPEN = """
import numpy as np, pyarrow as pa
import dftracer.utils as dft
from dftracer.utils import DataFrame, col

@dft.udf(batch=True, returns=np.float64)
def keep(x):
    return x + 1.0

df = DataFrame.from_arrow(pa.table({"x": [1.0, 2.0]}))
out = df.lazy().with_columns(r=keep(col("x"))).collect().to_arrow().to_pydict()
assert out["r"] == [2.0, 3.0]
print("ok")
"""

_EXIT_BUSY = """
import threading, time
import numpy as np, pyarrow as pa
import dftracer.utils as dft
from dftracer.utils import DataFrame, col

@dft.udf(batch=True, returns=np.float64)
def slow(x):
    time.sleep(0.5)
    return x

df = DataFrame.from_arrow(pa.table({"x": [float(i) for i in range(4000)]}))
def run():
    df.lazy().with_columns(r=slow(col("x"))).collect(batch_size=500)
threading.Thread(target=run, daemon=True).start()
time.sleep(0.2)
print("exiting")
"""

# The exit handler registered first runs last and gives up the GIL, so the
# daemon thread wakes with its batch cancelled before the interpreter ends.
_EXIT_CANCEL_SEEN = """
import atexit, os, threading, time
atexit.register(time.sleep, 1.0)
import numpy as np, pyarrow as pa
import dftracer.utils as dft
from dftracer.utils import DataFrame, col

@dft.udf(batch=True, returns=np.float64)
def slow(x):
    time.sleep(0.5)
    return x

df = DataFrame.from_arrow(pa.table({"x": [float(i) for i in range(4000)]}))
def run():
    try:
        df.lazy().with_columns(r=slow(col("x"))).collect(batch_size=500)
    except BaseException as e:
        os.write(1, ("caught %s %r\\n" % (type(e).__name__, e.args)).encode())
        raise
threading.Thread(target=run, daemon=True).start()
time.sleep(0.2)
print("exiting", flush=True)
"""

_LANE_STOP = """
import threading, time
import numpy as np, pyarrow as pa
import dftracer.utils as dft
from dftracer.utils import DataFrame, col
from dftracer.utils import dftracer_utils_ext as ext

gate = threading.Event()
entered = threading.Event()

@dft.udf(batch=True, returns=np.float64)
def block(x):
    entered.set()
    gate.wait(30)
    return x

df = DataFrame.from_arrow(pa.table({"x": [float(i) for i in range(2000)]}))
errors = []
def run():
    try:
        df.lazy().with_columns(r=block(col("x"))).collect(batch_size=100)
    except Exception as exc:
        errors.append(str(exc))
threads = [threading.Thread(target=run) for _ in range(2)]
for t in threads:
    t.start()
assert entered.wait(30)
time.sleep(0.3)
stopper = threading.Thread(target=ext.udf_lane_shutdown)
stopper.start()
time.sleep(0.3)
gate.set()
stopper.join(30)
for t in threads:
    t.join(30)
assert not any(t.is_alive() for t in threads)
assert errors and all("cancel" in e for e in errors), errors
print("ok")
"""


_LANE_LIFECYCLE = """
import gc, time
import numpy as np, pyarrow as pa
import dftracer.utils as dft
from dftracer.utils import DataFrame, col
from dftracer.utils import dftracer_utils_ext as ext

def alive():
    return ext.udf_lane_stats()["threads_alive"]

def wait_for(want):
    for _ in range(100):
        if alive() == want:
            return True
        time.sleep(0.05)
    return False

assert alive() == 0
df = DataFrame.from_arrow(pa.table({"x": [1.0, 2.0]}))

def use():
    @dft.udf(batch=True, returns=np.float64)
    def inc(x):
        return x + 1.0
    out = df.lazy().with_columns(r=inc(col("x"))).collect().to_arrow().to_pydict()
    assert out["r"] == [2.0, 3.0]
    assert alive() == 1
    inc.close()

use()
gc.collect()
assert wait_for(0), ext.udf_lane_stats()
use()
gc.collect()
assert wait_for(0), ext.udf_lane_stats()
stats = ext.udf_lane_stats()
assert stats["threads_started"] == 2 and stats["users"] == 0, stats
print("ok")
"""


def _run(script, timeout=120):
    return subprocess.run(
        [sys.executable, "-I", "-c", textwrap.dedent(script)],
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def test_interpreter_exit_with_an_open_batch_udf():
    r = _run(_EXIT_OPEN)
    assert r.returncode == 0, r.stderr
    assert "ok" in r.stdout


def test_interpreter_exit_while_a_batch_is_running():
    r = _run(_EXIT_BUSY)
    assert r.returncode == 0, r.stderr
    assert "exiting" in r.stdout


def test_a_daemon_thread_whose_batch_the_exit_cancels_ends_without_a_traceback():
    r = _run(_EXIT_CANCEL_SEEN)
    assert r.returncode == 0, r.stderr
    assert "caught SystemExit ()" in r.stdout, r.stdout + r.stderr
    assert "Traceback" not in r.stderr, r.stderr


def test_queued_batches_fail_as_cancelled_when_the_lane_stops():
    r = _run(_LANE_STOP)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "ok" in r.stdout


def test_the_lane_thread_stops_with_the_last_batch_udf_and_starts_again():
    r = _run(_LANE_LIFECYCLE)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "ok" in r.stdout
