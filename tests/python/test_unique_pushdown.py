"""The distinct pushdown into the trace scan.

`TraceViewer.filter(...).select(...).unique()` dedups inside the scan workers:
the plan absorbs the UniqueOp, and the result is the first occurrence of each
key in file order, then line order, whatever the worker count. The trace has
several files, several gzip members per file and a small checkpoint size, so
first occurrences of a key land in different scan units. A key column the scan
cannot key from the event (io_cat, acc_pat) is refused and the host cursor
handles it.
"""

import gzip
import json
import random

import pytest

import dftracer.utils as dftu

FILES = 3
MEMBERS = 6
LINES = 40
THREADS = (1, 2, 8)
NAMES = ("read", "write", "open", "lseek")


def _records():
    rng = random.Random(1234)
    out = []
    for i in range(FILES * MEMBERS * LINES):
        mix_num = rng.choice([1, 2.0, 3, 1.5, 2, 3.0, 4])
        mix_all = rng.choice([1, 2.0, "3", "x", 1.5, 2, None])
        out.append(
            {
                "ph": "X",
                "name": NAMES[rng.randrange(4)],
                "cat": "POSIX",
                "pid": 1,
                "tid": 1,
                "ts": 1000 + i,
                "dur": 1,
                "gtype": "counter" if rng.random() < 0.75 else "other",
                "run": "r%d" % rng.randrange(5),
                "metric": None if rng.random() < 0.15 else "m%d" % rng.randrange(8),
                "num": rng.randrange(12),
                "mix_num": mix_num,
                "mix_all": mix_all,
                "seq": i,
                "pad": "p" * 100,
            }
        )
    return out


RECORDS = _records()


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("unique")
    files = []
    per_file = MEMBERS * LINES
    for f in range(FILES):
        path = str(d / ("t%d.pfw.gz" % f))
        with open(path, "wb") as fh:
            for m in range(MEMBERS):
                lo = f * per_file + m * LINES
                lines = "".join(json.dumps(r) + "\n" for r in RECORDS[lo : lo + LINES])
                fh.write(gzip.compress(lines.encode()))
        files.append(path)
    idx = str(d / "index")
    with dftu.Indexer(files=files, index_dir=idx, checkpoint_size="4KB") as ix:
        ix.ensure_indexed()
    return files, idx


def _viewer(trace, rt):
    files, idx = trace
    return dftu.TraceViewer(files, idx, runtime=rt)


def _rows(df):
    cols = list(df.column_names)
    return list(zip(*[df[c].to_pylist() for c in cols]))


def _counters():
    return [r for r in RECORDS if r["gtype"] == "counter"]


def _first_by(rows, key):
    seen = set()
    out = []
    for r in rows:
        k = key(r)
        if k not in seen:
            seen.add(k)
            out.append(r)
    return out


def _check_all_threads(trace, build, expected):
    for threads in THREADS:
        with dftu.Runtime(threads=threads) as rt:
            lf = build(_viewer(trace, rt))
            assert "unique" not in lf.explain()
            assert _rows(lf.collect()) == expected, "threads=%d" % threads


def test_string_key_matches_first_occurrence_in_file_order(trace):
    ctr = dftu.col("gtype") == "counter"
    expected = [
        (r["run"], r["metric"]) for r in _first_by(_counters(), lambda r: (r["run"], r["metric"]))
    ]
    assert len(expected) > 20
    _check_all_threads(trace, lambda tv: tv.filter(ctr).select("run", "metric").unique(), expected)


def test_drop_duplicates_is_unique(trace):
    ctr = dftu.col("gtype") == "counter"
    with dftu.Runtime(threads=2) as rt:
        lf = _viewer(trace, rt).filter(ctr).select("run", "metric")
        assert "unique" not in lf.drop_duplicates().explain()
        assert _rows(lf.drop_duplicates().collect()) == _rows(lf.unique().collect())


def test_subset_keeps_the_first_whole_row(trace):
    ctr = dftu.col("gtype") == "counter"
    expected = [
        (r["run"], r["metric"], r["seq"]) for r in _first_by(_counters(), lambda r: r["run"])
    ]
    for threads in THREADS:
        with dftu.Runtime(threads=threads) as rt:
            lf = _viewer(trace, rt).filter(ctr).select("run", "metric", "seq")
            key = list(lf.columns)[0]
            sub = lf.unique(subset=[key])
            assert "unique" not in sub.explain()
            assert _rows(sub.collect()) == expected, "threads=%d" % threads


def test_null_key_is_one_value(trace):
    ctr = dftu.col("gtype") == "counter"
    expected = [(r["metric"], r["seq"]) for r in _first_by(_counters(), lambda r: r["metric"])]
    assert any(m is None for m, _ in expected)
    for threads in THREADS:
        with dftu.Runtime(threads=threads) as rt:
            lf = _viewer(trace, rt).filter(ctr).select("metric", "seq")
            sub = lf.unique(subset=[list(lf.columns)[0]])
            assert _rows(sub.collect()) == expected, "threads=%d" % threads


def test_numeric_key_matches_first_occurrence(trace):
    ctr = dftu.col("gtype") == "counter"
    expected = [(r["num"], r["seq"]) for r in _first_by(_counters(), lambda r: r["num"])]
    for threads in THREADS:
        with dftu.Runtime(threads=threads) as rt:
            lf = _viewer(trace, rt).filter(ctr).select("num", "seq")
            sub = lf.unique(subset=[list(lf.columns)[0]])
            assert "unique" not in sub.explain()
            assert _rows(sub.collect()) == expected, "threads=%d" % threads


@pytest.mark.parametrize("column", ["mix_num", "mix_all"])
def test_mixed_type_key_matches_host(trace, column):
    ctr = dftu.col("gtype") == "counter"
    with dftu.Runtime(threads=1) as rt:
        lf = _viewer(trace, rt).filter(ctr).select(column, "seq")
        host = _rows(lf.collect().unique(subset=[list(lf.columns)[0]]))
    assert len(host) > 3
    for threads in THREADS:
        with dftu.Runtime(threads=threads) as rt:
            lf = _viewer(trace, rt).filter(ctr).select(column, "seq")
            sub = lf.unique(subset=[list(lf.columns)[0]])
            assert "unique" not in sub.explain()
            assert _rows(sub.collect()) == host, "threads=%d" % threads


def test_io_cat_is_refused_and_matches_host(trace):
    with dftu.Runtime(threads=1) as rt:
        lf = _viewer(trace, rt).select("io_cat")
        host = _rows(lf.collect().unique())
    assert len(host) > 1
    for threads in THREADS:
        with dftu.Runtime(threads=threads) as rt:
            lf = _viewer(trace, rt).select("io_cat").unique()
            assert "unique" in lf.explain()
            assert sorted(_rows(lf.collect())) == sorted(host)


def test_acc_pat_is_refused(trace):
    lf = dftu.TraceViewer(trace[0], trace[1]).select("acc_pat").unique()
    assert "unique" in lf.explain()
