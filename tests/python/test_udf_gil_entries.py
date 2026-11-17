#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Every binding that runs an expression must release the GIL around the engine.

A per-batch Python UDF runs on the Python lane, which needs the GIL. numpy drops
the GIL inside its loops on large arrays, so a calling thread that waits for the
engine while it holds the GIL deadlocks the lane. Each test runs one entry
family over 200k rows or more with a batch UDF, under a faulthandler timeout
that ends the process with a traceback instead of hanging the suite.
"""

import faulthandler
import gzip
import json

import pytest

pa = pytest.importorskip("pyarrow")
np = pytest.importorskip("numpy")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import DataFrame, col  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402

pytestmark = pytest.mark.skipif(
    not hasattr(_ext, "_LazyFrame") or not hasattr(_ext, "udf_register_batch"),
    reason="extension built without Arrow support",
)

ROWS = 300_000
TIMEOUT = 60


@pytest.fixture(autouse=True)
def _guard():
    faulthandler.dump_traceback_later(TIMEOUT, exit=True)
    with dft.udf.scope():
        yield
    faulthandler.cancel_dump_traceback_later()


@pytest.fixture
def twice():
    # A large-array numpy call: it drops the GIL, so it needs it back.
    @dft.udf(batch=True, returns=np.float64)
    def twice(x):
        return np.sort(x * 2.0)[::-1][np.argsort(np.argsort(x))]

    return twice


@pytest.fixture
def df():
    return DataFrame.from_arrow(
        pa.table(
            {
                "k": np.arange(ROWS, dtype=np.int64) % 4,
                "x": np.arange(ROWS, dtype=np.float64),
            }
        )
    )


def _expected_sum():
    return float(np.arange(ROWS, dtype=np.float64).sum() * 2.0)


def test_eager_group_by_agg(df, twice):
    out = df.group_by("k").agg(s=twice(col("x")).sum()).to_arrow().to_pydict()
    assert sum(out["s"]) == pytest.approx(_expected_sum())


def test_lazy_collect_with_columns_filter_and_group_by(df, twice):
    lazy = df.lazy()
    assert lazy.with_columns(r=twice(col("x"))).collect().to_arrow().num_rows == ROWS
    assert lazy.filter(twice(col("x")) >= 0.0).collect().to_arrow().num_rows == ROWS
    grouped = (
        lazy.with_columns(r=twice(col("x")))
        .group_by("k")
        .agg(s=col("r").sum())
        .collect()
        .to_arrow()
        .to_pydict()
    )
    assert sum(grouped["s"]) == pytest.approx(_expected_sum())


def test_collect_all(df, twice):
    plans = [df.lazy().with_columns(r=twice(col("x"))) for _ in range(2)]
    frames = dft.collect_all(plans)
    assert [f.to_arrow().num_rows for f in frames] == [ROWS, ROWS]


def test_collect_async(df, twice):
    handle = df.lazy().with_columns(r=twice(col("x"))).collect_async()
    assert handle.get().to_arrow().num_rows == ROWS


def test_stream(df, twice):
    rows = sum(f.to_arrow().num_rows for f in df.lazy().with_columns(r=twice(col("x"))).stream())
    assert rows == ROWS


def test_arrow_c_stream(df, twice):
    reader = pa.RecordBatchReader.from_stream(df.lazy().with_columns(r=twice(col("x"))))
    assert reader.read_all().num_rows == ROWS


def test_expr_apply_and_eval_many(df, twice):
    assert len(twice(col("x")).apply(df)) == ROWS


def test_trace_viewer_duql(tmp_path, twice):
    events = [
        {"ph": "X", "name": "read", "cat": "POSIX", "pid": 1, "tid": i, "ts": i, "dur": i % 97}
        for i in range(ROWS)
    ]
    path = tmp_path / "t.pfw.gz"
    path.write_bytes(gzip.compress(("\n".join(json.dumps(e) for e in events) + "\n").encode()))
    got = (
        dft.TraceViewer(str(path))
        .duql("derive d = twice(dur) | agg { n = count() }")
        .collect()
        .to_dict()
    )
    assert got["n"] == [ROWS]
