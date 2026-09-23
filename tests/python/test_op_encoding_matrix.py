"""Every registered column op, run through the registry (`op_run`) on every
column encoding the engine produces: FLAT, a filter's SELECTION view, a
DICTIONARY view, a view over a view, and the empty and all-null columns. A
kernel reads FLAT buffers, so a view must give the same answer as its
materialized copy, and no input shape may crash the process. The operands
are generated from each op's signature tokens, so a new op is covered the
day it is registered."""

import re

import pytest

pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame, Series  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402

_REFUSALS = (TypeError, ValueError, RuntimeError, IndexError, KeyError, OverflowError)

# One default operand per signature token; a column op's later `series`
# operands take the same column again (same type and length).
_TOKEN_ARGS = {
    "i64": 1,
    "u64": 1,
    "i32": 1,
    "f64": 0.5,
    "char": "x",
    "str": "a",
    "scalar": 1,
    "cmp": 0,
    "prim": 0,
    "logical": 0,
    "dtype": 10,
    "reduce": 0,
    "rank": 0,
    "rolling": 0,
    "i64list": [0, 1],
    "strlist": ["x"],
}


def _bases():
    return {
        "float": Series([1.0, None, 3.0, 4.0, 5.0, 6.0]),
        "int": Series([1, None, 3, 4, 5, 6]),
        "string": Series(["a", None, "c", "d", "e", "f"]),
    }


def _views(base):
    mask = Series([True, True, False, True, True, True])
    arr = base.to_arrow()
    yield "selection", base.filter(mask)
    yield "nested", base.filter(mask).filter(Series([True, False, True, True, True]))
    yield (
        "dictionary",
        Series(pa.DictionaryArray.from_arrays(pa.array([0, 1, 2, 3, 4]), arr.slice(0, 5))),
    )
    yield "sorted", base.sort()
    yield "empty", base.head(0)
    yield "all_null", Series(pa.array([None] * 3, type=_value_type(base)))


def _value_type(series):
    t = series.to_arrow().type
    return t.value_type if pa.types.is_dictionary(t) else t


def _flat_twin(view):
    """The same values as a fresh FLAT column."""
    return Series(pa.array(view.to_list(), type=_value_type(view)))


def _operands(signature):
    tokens = re.match(r"\((.*)\) -> (\w+)", signature).groups()
    names = [t.strip() for t in tokens[0].split(",") if t.strip()]
    return names, tokens[1]


def _run(name, column, tokens):
    args = []
    for tok in tokens[1:]:
        args.append(column._native if tok == "series" else _TOKEN_ARGS[tok])
    try:
        out = _ext.op_run(name, column._native, *args)
    except _REFUSALS as e:
        return ("refused", type(e).__name__)
    if isinstance(out, _ext._Series):
        return ("series", Series(out).to_list())
    return ("scalar", out)


def _same(got, want):
    if got[0] != want[0]:
        return False
    if got[0] == "refused":
        return got == want
    a, b = got[1], want[1]
    if got[0] == "scalar":
        a, b = [a], [b]
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if isinstance(x, float) and isinstance(y, float) and x != x and y != y:
            continue
        if x != y and not (isinstance(x, float) and isinstance(y, float) and x == pytest.approx(y)):
            return False
    return True


def _column_ops():
    for name in _ext.op_list():
        info = _ext.op_info(name)
        tokens, _ = _operands(info["signature"])
        if info["kind"] in ("series", "aggregate") and tokens and tokens[0] == "series":
            if all(t in _TOKEN_ARGS or t == "series" for t in tokens[1:]):
                yield name, tokens


@pytest.mark.parametrize("name,tokens", list(_column_ops()))
def test_every_column_op_reads_a_view_as_its_flat_twin(name, tokens):
    for base_name, base in _bases().items():
        for view_name, view in _views(base):
            twin = _flat_twin(view)
            got = _run(name, view, tokens)
            want = _run(name, twin, tokens)
            assert _same(got, want), (name, base_name, view_name, got, want)


def test_frame_ops_over_a_filtered_frame_match_the_flat_frame():
    base = DataFrame(
        {
            "k": ["a", None, "a", "b", "b", "c"],
            "x": [1.0, None, 3.0, 4.0, 5.0, 6.0],
            "n": [1, 2, None, 4, 5, 6],
        }
    )
    mask = Series([True, True, False, True, True, True])
    view = base.filter(mask)
    flat = DataFrame.from_arrow(view.to_arrow())
    checks = [
        lambda f: f.sort_by("x"),
        lambda f: f.sort_by_multi(["k", "x"]),
        lambda f: f.group_by("k").sum(),
        lambda f: f.group_by("k", dropna=False).size(),
        lambda f: f.group_by().agg(sx=("x", "sum"), mn=("n", "max")),
        lambda f: f.unique("k"),
        lambda f: f.drop_nulls(),
        lambda f: f.with_row_index("r").join(f.with_row_index("r"), on="r"),
        lambda f: f.window(partition_by=["k"], order_by=["x"], specs=[("row_number", "rn")]),
        lambda f: f.describe(),
        lambda f: f.filter(f["x"].notna()).take([0, 1]),
        lambda f: f.head(2).concat(f.tail(2)),
        lambda f: f.select("x").fillna(0.0),
        lambda f: f.rename({"x": "y"}).sort_by("y", descending=True),
    ]
    for i, check in enumerate(checks):
        assert check(view).to_arrow().to_pydict() == check(flat).to_arrow().to_pydict(), i


def test_take_refuses_an_index_past_the_end():
    for s in (
        Series([1.0, 2.0]),
        Series(["a", "b"]),
        Series([1.0, 2.0, 3.0]).filter(Series([True, False, True])),
    ):
        with pytest.raises(IndexError):
            s.take([5])
        with pytest.raises(IndexError):
            s.take([0, 2])
    # A negative index gathers a null (the join's outer fill).
    assert Series([1.0, 2.0]).take([-1, 1]).to_list() == [None, 2.0]
