"""LazyFrame.drop and LazyFrame.rename(mapping) by name, including the columns
a trace scan adds that the schema does not list."""

import gzip
import json

import pytest

import dftracer.utils as dftu
from dftracer.utils import DataFrame

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

N = 90
BASE = 'gtype == "func"'


@pytest.fixture(scope="module")
def genesis(tmp_path_factory):
    p = str(tmp_path_factory.mktemp("dr") / "g.ndjson.gz")
    with gzip.open(p, "wt") as f:
        for i in range(N):
            rec = {
                "gtype": "func",
                "run": "ab",
                "ts": i,
                "v": {"p50": i, "p99": i + 5},
                "count": i % 7,
            }
            f.write(json.dumps(rec) + "\n")
    return p


def _view(path):
    return dftu.TraceViewer(path, record_schema="genesis").duql(BASE)


def test_trace_viewer_drop_keeps_undeclared_columns(genesis):
    tv = _view(genesis)
    full = tv.collect().to_dict()
    assert {"v.p50", "v.p99", "count"} <= set(full)
    got = tv.drop("v.p50").collect().to_dict()
    assert set(got) == set(full) - {"v.p50"}
    assert got["v.p99"] == full["v.p99"]


def test_trace_viewer_drop_ignores_unknown_name(genesis):
    tv = _view(genesis)
    assert set(tv.drop("nope").collect().to_dict()) == set(tv.collect().to_dict())


def test_trace_viewer_rename_mapping_reaches_undeclared_columns(genesis):
    tv = _view(genesis)
    full = tv.collect().to_dict()
    got = tv.rename({"count": "cnt", "v.p99": "p"}).collect().to_dict()
    assert set(got) == (set(full) - {"count", "v.p99"}) | {"cnt", "p"}
    assert got["cnt"] == full["count"]
    assert got["p"] == full["v.p99"]


def test_trace_viewer_rename_collision_is_an_error(genesis):
    with pytest.raises(Exception, match="two columns"):
        _view(genesis).rename({"count": "v.p50"}).collect()


def _frame():
    return DataFrame({"a": [1, 2], "b": [3, 4], "c": [5, 6]}).lazy()


def test_plain_drop():
    assert _frame().drop("b").collect().to_dict() == {"a": [1, 2], "c": [5, 6]}
    assert list(_frame().drop(["a", "c"]).collect().to_dict()) == ["b"]


def test_plain_drop_unknown_name_is_a_key_error():
    with pytest.raises(KeyError):
        _frame().drop("nope")


def test_plain_rename_mapping():
    got = _frame().rename({"a": "x", "c": "z"}).collect().to_dict()
    assert got == {"x": [1, 2], "b": [3, 4], "z": [5, 6]}


def test_plain_rename_unknown_name_is_a_key_error():
    with pytest.raises(KeyError):
        _frame().rename({"nope": "x"})


def test_plain_rename_positional_list_stays_positional():
    got = _frame().rename(["x", "y", "z"]).collect().to_dict()
    assert list(got) == ["x", "y", "z"]
