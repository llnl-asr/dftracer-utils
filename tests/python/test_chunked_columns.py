"""Collected chunked columns give the same values through every conversion."""

import numpy as np
import pytest

from dftracer.utils import DataFrame, Series

pa = pytest.importorskip("pyarrow")
pd = pytest.importorskip("pandas")
pl = pytest.importorskip("polars")

N = 7
MORSEL = 2


def _frame(nulls=False):
    a = list(range(N))
    s = [None if nulls and i % 3 == 1 else "v%d" % i for i in range(N)]
    return DataFrame({"a": Series(pa.array(a, type=pa.int64())), "s": Series(pa.array(s))})


def _chunked(df):
    out = df.lazy().collect(morsel_rows=MORSEL)
    assert out["a"].encoding == 5
    return out


@pytest.mark.parametrize("nulls", [False, True])
def test_series_conversions_match_joined(nulls):
    df = _frame(nulls)
    ch = _chunked(df)
    for name in ("a", "s"):
        c, f = ch[name], df[name]
        assert c.to_list() == f.to_list()
        assert c.to_arrow().equals(f.to_arrow())
        assert list(c.to_numpy()) == list(f.to_numpy())
        assert c.to_pandas().equals(f.to_pandas())
        assert c.to_polars().equals(f.to_polars())


def test_buffer_protocol_falls_back():
    c = _chunked(_frame())["a"]
    with pytest.raises(BufferError):
        memoryview(c._native)
    assert np.asarray(c).tolist() == list(range(N))


def _batches(df):
    return list(pa.RecordBatchReader.from_stream(df))


def test_arrow_stream_one_batch_per_chunk():
    df = _frame(nulls=True)
    ch = _chunked(df)
    batches = _batches(ch)
    assert len(batches) == (N + MORSEL - 1) // MORSEL
    assert sum(b.num_rows for b in batches) == N
    assert pa.Table.from_batches(batches).equals(df.to_arrow())
    assert pa.table(ch).equals(df.to_arrow())


def test_arrow_stream_mismatched_bounds_is_one_batch():
    df = _frame()
    ch = _chunked(df)
    other = df.lazy().collect(morsel_rows=3)
    mixed = DataFrame({"a": ch["a"], "s": other["s"]})
    batches = _batches(mixed)
    assert len(batches) == 1
    assert pa.Table.from_batches(batches).equals(df.to_arrow())
    flat = DataFrame({"a": ch["a"], "s": df["s"]})
    assert len(_batches(flat)) == 1


def test_jit_series_op_over_chunked_equals_joined():
    from dftracer.utils import jit
    from dftracer.utils.jit import ops

    @jit.series
    def chunk_triple(a):
        return a * 3

    df = _frame()
    c, f = _chunked(df)["a"], df["a"]
    got = ops.run(__name__ + ".chunk_triple", c)
    assert got.to_list() == ops.run(__name__ + ".chunk_triple", f).to_list()
    assert got.to_list() == [3 * i for i in range(N)]
