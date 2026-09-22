"""``Series.apply`` / ``map`` and ``DataFrame.apply`` over a Python function:
the trace tier runs in the engine with no warning, a ``@jit.series`` /
``@jit.op`` function runs through its own path, and anything else runs in
Python with a warning naming the reason."""

import logging
import math

import pytest

pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame, Series, col, jit  # noqa: E402
from dftracer.utils.jit_op import op  # noqa: E402


def _lst(s):
    return s.to_arrow().to_pylist()


def _dict(df):
    return df.to_arrow().to_pydict()


def test_series_apply_traces_an_expression_body(caplog):
    s = Series([1, 2, 3, None])
    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        out = s.apply(lambda x: x * 2 + 1)
    assert _lst(out) == [3, 5, 7, None]
    assert caplog.records == []
    assert _lst(s.map(lambda x: x > 1)) == [False, True, True, None]
    assert _lst(s.apply(lambda x: x.clip(2, 2))) == [2, 2, 2, None]


def test_series_apply_compiles_control_flow(caplog):
    s = Series([1, 2, 3, None])
    THRESHOLD = 1  # noqa: N806 - a closure constant the transpiler inlines

    def branchy(x):
        return x if x > THRESHOLD else -x

    def staged(x):
        """Statements, elif, a local, min/max/abs and math."""
        y = x * 10
        if y < 15:
            return abs(y - 100)
        elif y < 25:
            return min(y, 22) + max(y, 0)
        return math.floor(y / 4)

    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        assert _lst(s.apply(branchy)) == [-1, 2, 3, None]
        assert _lst(s.apply(staged)) == [90, 40, 7, None]
        assert _lst(s.apply(lambda x: 1 if x >= 2 and x < 3 else 0)) == [0, 1, 0, None]
        assert _lst(s.apply(lambda x: x in (1, 3))) == [True, False, True, None]
        assert _lst(s.map(lambda x: 5 > x)) == [True, True, True, None]
    assert caplog.records == []

    def helper(v, k):
        return v * k if v > 0 else -v

    def outer(x):
        return helper(x, 3) + helper(-x, 2)

    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        # Python floor / mod semantics on negatives, ints stay ints.
        neg = Series([7, -7, 6, None])
        assert _lst(neg.apply(lambda x: x // 2)) == [3, -4, 3, None]
        assert _lst(neg.apply(lambda x: x % 3)) == [1, 2, 0, None]
        assert _lst(neg.apply(lambda x: x % -3)) == [-2, -1, 0, None]
        assert _lst(neg.apply(lambda x: x**2)) == [49, 49, 36, None]
        assert _lst(neg.apply(lambda x: x**0)) == [1, 1, 1, None]
        assert _lst(Series([4.0, 9.0]).apply(lambda x: x**0.5)) == [2.0, 3.0]
        assert _lst(Series([7.5]).apply(lambda x: x // 2)) == [3.0]
        # A call to another plain function is inlined.
        assert _lst(s.apply(outer)) == [4, 8, 12, None]
        assert _lst(s.apply(lambda x: max(x, 2, 0) + min(x, 1, 5))) == [3, 3, 4, None]
    assert caplog.records == []

    names = Series(["read", "Write", "fsync"])
    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        assert _lst(names.apply(lambda n: 1 if n.lower().startswith("w") else 0)) == [0, 1, 0]
        assert _lst(names.apply(lambda n: "ea" in n)) == [True, False, False]
    assert caplog.records == []


def test_series_apply_falls_back_with_a_warning(caplog):
    s = Series([1, 2, 3, None])

    def loops(x):
        total = 0
        for _ in range(x):
            total += 2
        return total

    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        out = s.apply(loops)
    assert _lst(out) == [2, 4, 6, None]
    assert len(caplog.records) == 1
    msg = caplog.records[0].getMessage()
    assert "loops" in msg and "Python" in msg and "per element" in msg
    assert "not compilable" in msg and "For" in msg

    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        out = s.apply(lambda x: str(x) + "!")
    assert _lst(out) == ["1!", "2!", "3!", None]


def test_series_apply_accepts_jit_functions(caplog):
    @jit.series(module="test_apply_mod")
    def tripled(x):
        return x * 3

    @op
    def plus10(x: jit.i64) -> jit.i64:
        return x + 10

    s = Series([1, 2, 3])
    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        assert _lst(s.apply(tripled)) == [3, 6, 9]
        assert _lst(s.apply(plus10)) == [11, 12, 13]
    assert caplog.records == []


def test_dataframe_apply_per_column_and_per_row(caplog):
    df = DataFrame({"a": [1, 2, 3], "b": [10, 20, 30]})
    assert _dict(df.apply(lambda s: s * 2)) == {"a": [2, 4, 6], "b": [20, 40, 60]}
    assert _dict(df.apply(lambda s: s.sum())) == {"a": [6], "b": [60]}
    assert _lst(df.apply(col("a") + col("b"))) == [11, 22, 33]
    with pytest.raises(TypeError):
        df.apply(lambda s: s.sum() if s.sum() > 10 else s)

    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        traced = df.apply(lambda r: r["a"] + r.b * 2, axis=1)
    assert _lst(traced) == [21, 42, 63]
    assert caplog.records == []

    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        compiled = df.apply(lambda r: r["a"] if r["b"] > 15 else r.b, axis=1)
    assert _lst(compiled) == [10, 2, 3]
    assert caplog.records == []

    # A generator over expressions still traces: sum() is just repeated +.
    assert _lst(df.apply(lambda r: sum(v for v in [r["a"], r["b"]]), axis=1)) == [11, 22, 33]

    def python_only(r):
        return f"{r['a']}-{r['b']}"

    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        slow = df.apply(python_only, axis=1)
    assert _lst(slow) == ["1-10", "2-20", "3-30"]
    assert len(caplog.records) == 1
    assert "per row" in caplog.records[0].getMessage()

    with pytest.raises(KeyError):
        df.apply(lambda r: r["nope"], axis=1)
    with pytest.raises(ValueError):
        df.apply(lambda r: r, axis=2)


def test_apply_lowers_is_none_to_the_null_mask(caplog):
    # `is` cannot be traced (Python never hands it to the tracer object), so
    # the source lowering reads `x is None` as the null mask; a traced form
    # would have folded it to False and silently taken the else arm.
    df = DataFrame({"a": [1, None, 2], "b": [10, 20, 30]})

    def is_missing(r):
        return r["a"] is None

    def fill_from_b(r):
        return r["b"] if r["a"] is None else r["a"]

    def present_and_big(r):
        return r["a"] is not None and r["a"] > 1

    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        assert _lst(df.apply(is_missing, axis=1)) == [False, True, False]
        assert _lst(df.apply(fill_from_b, axis=1)) == [1, 20, 2]
        assert _lst(df.apply(present_and_big, axis=1)) == [False, False, True]
        assert _lst(df.apply(lambda r: None is r["a"], axis=1)) == [False, True, False]
    assert caplog.records == []

    # Series.apply keeps its rule: a null element maps to a null result.
    def not_missing(x):
        return x is not None

    assert _lst(df["a"].apply(not_missing)) == [True, None, True]
    # `is` against anything but None has no engine form: the Python tier.
    sentinel = object()
    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        assert _lst(df["a"].apply(lambda x: x is sentinel)) == [False, None, False]
    assert len(caplog.records) == 1
    assert "None only" in caplog.records[0].getMessage()


def test_select_expressions():
    from dftracer.utils.columnar import if_else, when

    df = DataFrame({"a": [1, 5, None, 9], "s": ["x", "y", "z", "w"]})
    assert _lst(df.apply(when(col("a") > 2).then(col("a")).otherwise(0))) == [0, 5, 0, 9]
    assert _lst(df.apply(if_else(col("a") > 2, 1.5, 0))) == [0.0, 1.5, 0.0, 1.5]
    assert _lst(df.apply(col("a").where(col("a") > 2, -1))) == [-1, 5, -1, 9]
    assert _lst(df.apply(col("a").mask(col("a") > 2, -1))) == [1, -1, None, -1]
    assert _lst(df.apply(when(col("a") > 2).then(col("s")).otherwise(col("s").upper()))) == [
        "X",
        "y",
        "Z",
        "w",
    ]
    lazy = df.lazy().with_column("big", when(col("a") > 2).then(1).otherwise(0)).collect()
    assert _dict(lazy)["big"] == [0, 1, 0, 1]

    s = df["a"]
    assert _lst(s.where(s > 2, -1)) == [-1, 5, -1, 9]
    assert _lst(s.where(s > 2, 0.5)) == [0.5, 5.0, 0.5, 9.0]
    assert _lst(s.mask(s > 2, -1)) == [1, -1, None, -1]
    assert _lst(s.where(s > 2, Series([7, 7, 7, 7]))) == [7, 5, 7, 9]
    with pytest.raises(ValueError):
        s.where(s > 2, Series([1.0, 2.0, 3.0, 4.0]))
