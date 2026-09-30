"""``count`` of a column counts its non-null values inside an aggregation
(openspec change count-skips-nulls); the no-column ``count()`` stays the row
count, in the dataframe engine and in the trace View."""

import gzip
import json
import random

import pytest

from dftracer.utils import DataFrame, col, count


def _frame():
    return DataFrame.from_dict({"g": ["a", "a", "a", "b"], "x": [1.0, None, 3.0, 5.0]})


def _by_group(frame, column):
    pdf = frame.to_pandas()
    return dict(zip(pdf["g"], pdf[column]))


def test_proposal_repro_group_count_skips_nulls():
    d = _frame()
    assert d["x"].count() == 3
    got = d.group_by("g").agg(n=col("x").count()).to_pandas()
    assert dict(zip(got["g"], got["n"])) == {"a": 2, "b": 1}
    workaround = d.group_by("g").agg(n=col("x").is_not_null().sum()).to_pandas()
    assert dict(zip(workaround["g"], workaround["n"])) == {"a": 2, "b": 1}


def test_default_name_is_count_of_the_column():
    got = _frame().group_by("g").agg(col("x").count()).to_pandas()
    assert list(got.columns) == ["g", "count_x"]


def test_count_of_a_column_of_nulls_is_zero():
    d = DataFrame.from_dict({"g": ["a", "a", "b"], "x": [None, None, 1.0]})
    got = d.group_by("g").agg(n=col("x").count()).to_pandas()
    assert dict(zip(got["g"], got["n"])) == {"a": 0, "b": 1}


def test_no_column_count_is_still_rows():
    got = _frame().group_by("g").agg(n=count()).to_pandas()
    assert dict(zip(got["g"], got["n"])) == {"a": 3, "b": 1}


def test_sum_over_count_equals_mean():
    got = (
        _frame()
        .group_by("g")
        .agg(s=col("x").sum(), n=col("x").count(), m=col("x").mean())
        .to_pandas()
    )
    for s, n, m in zip(got["s"], got["n"], got["m"]):
        assert s / n == pytest.approx(m)
    assert dict(zip(got["g"], got["m"]))["a"] == pytest.approx(2.0)


def test_matches_pandas_on_random_data_with_nulls():
    pd = pytest.importorskip("pandas")
    rng = random.Random(7)
    keys = [rng.choice("abcde") for _ in range(2000)]
    xs = [None if rng.random() < 0.3 else rng.random() * 10 for _ in range(2000)]
    d = DataFrame.from_dict({"g": keys, "x": xs})
    got = d.group_by("g").agg(n=col("x").count(), rows=count()).to_pandas().set_index("g")
    pdf = pd.DataFrame({"g": keys, "x": xs})
    want = pdf.groupby("g")["x"].agg(n="count", rows="size")
    assert got["n"].sort_index().tolist() == want["n"].sort_index().tolist()
    assert got["rows"].sort_index().tolist() == want["rows"].sort_index().tolist()


def test_trace_view_counted_field_counts_present_rows(tmp_path):
    pytest.importorskip("pyarrow")
    import dftracer.utils as dft
    from dftracer.utils import TraceViewer
    from dftracer.utils.columnar import F

    path = tmp_path / "t.pfw.gz"
    with gzip.open(path, "wt") as f:
        for i in range(10):
            args = {"size": 10 * i} if i % 2 == 0 else {}
            f.write(
                json.dumps(
                    {
                        "name": "read",
                        "cat": "POSIX",
                        "pid": 1,
                        "tid": 1,
                        "ts": 100 + i,
                        "dur": 5,
                        "ph": "X",
                        "args": args,
                    }
                )
                + "\n"
            )
    with dft.Indexer(files=[str(path)]) as ix:
        ix.ensure_indexed()

    def run(*specs):
        return TraceViewer(str(path)).group_by("cat").agg(*specs).collect().to_arrow().to_pydict()

    rows = run("count")
    assert rows["count"] == [10]
    present = run(F("args.size").count())
    assert present["count"] == [5]
    assert run(F.any.count())["count"] == [10]
