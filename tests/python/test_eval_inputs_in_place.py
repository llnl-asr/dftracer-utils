"""Column expressions over chunked collect results equal the joined ones."""

import pytest

from dftracer.utils import DataFrame, Series, col

pa = pytest.importorskip("pyarrow")

N = 300
MORSEL = 40


def _frame():
    a = [None if i % 7 == 3 else i - 100 for i in range(N)]
    b = [None if i % 11 == 5 else i * 0.5 for i in range(N)]
    s = [None if i % 5 == 2 else "name%d" % (i % 9) for i in range(N)]
    return DataFrame(
        {
            "a": Series(pa.array(a, type=pa.int64())),
            "b": Series(pa.array(b, type=pa.float64())),
            "s": Series(pa.array(s)),
        }
    )


def _chunked(df):
    out = df.lazy().collect(morsel_rows=MORSEL)
    assert out["a"].encoding == 5
    return out


def _rows(df):
    return {n: df[n].materialize().to_list() for n in df.columns}


def test_with_columns_equal_joined():
    df = _frame()
    ch = _chunked(df)
    exprs = {
        "sum": col("a") + col("b"),
        "big": col("a") > 50,
        "has": col("s").str.contains("name1"),
        "len": col("s").str.len_chars(),
    }
    got = _rows(ch.lazy().with_columns(**exprs).collect())
    want = _rows(df.lazy().with_columns(**exprs).collect())
    assert got == want


@pytest.mark.parametrize("threshold", [50, 10**6])
def test_filter_equal_joined(threshold):
    df = _frame()
    ch = _chunked(df)
    got = _rows(ch.lazy().filter(col("a") > threshold).collect())
    want = _rows(df.lazy().filter(col("a") > threshold).collect())
    assert got == want
    if threshold > N:
        assert all(v == [] for v in got.values())
