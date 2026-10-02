"""A collect with a memory budget below its size spills later parts to a mapped,
unlinked file in DFTRACER_UTILS_SPILL_DIR and returns the unbudgeted values."""

import os

import pytest

pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402

pytestmark = pytest.mark.skipif(
    not hasattr(_ext, "_LazyFrame"), reason="extension built without Arrow support"
)

N = 3000
MORSEL = 100


def _frame():
    return DataFrame.from_arrow(
        pa.table(
            {
                "i": pa.array([None if i % 5 == 2 else i * 37 - 100 for i in range(N)], pa.int64()),
                "f": pa.array([i * 1.5 for i in range(N)], pa.float64()),
                "b": pa.array([i % 3 != 1 for i in range(N)], pa.bool_()),
                "s": pa.array([None if i % 7 == 3 else "name-%d" % (i % 11) for i in range(N)]),
            }
        )
    )


def test_budgeted_collect_equals_unbudgeted(tmp_path, monkeypatch):
    monkeypatch.setenv("DFTRACER_UTILS_SPILL_DIR", str(tmp_path))
    df = _frame()
    want = df.lazy().collect(MORSEL).to_arrow().to_pydict()
    got = df.lazy().memory_budget(1).collect(MORSEL)
    assert got.to_arrow().to_pydict() == want
    assert list(tmp_path.iterdir()) == []


def test_spill_dir_is_honored(tmp_path, monkeypatch):
    blocker = tmp_path / "file"
    blocker.write_text("x")
    bad = os.path.join(str(blocker), "sub")
    monkeypatch.setenv("DFTRACER_UTILS_SPILL_DIR", bad)
    with pytest.raises(Exception) as err:
        _frame().lazy().memory_budget(1).collect(MORSEL)
    assert bad in str(err.value)
    assert "DFTRACER_UTILS_SPILL_DIR" in str(err.value)
