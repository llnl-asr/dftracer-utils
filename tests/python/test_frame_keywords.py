# SPDX-License-Identifier: MIT
"""Native frame methods that replaced Python forwarders keep the keyword names
of the forwarders' signatures.

The list is frozen from the signatures before the forwarders were deleted; it
is not computed from the running classes.
"""

import gzip
import json

import pytest

from dftracer.utils import DataFrame, LazyFrame, TraceViewer
from dftracer.utils import dftracer_utils_ext as _ext

# Accepted losses: `DataFrame.__contains__(name=)` and `__getitem__(key=)` no
# longer take their keyword. They are slot wrappers of the C type, which cannot
# take keywords, and pandas and polars do not take them either.

# (native class, method, keyword names of the old Python signature, in order)
OLD_KEYWORDS = [
    ("DataFrame", "__arrow_c_stream__", ("requested_schema",)),
    ("DataFrame", "column_index", ("name",)),
    ("DataFrame", "partition_id", ("keys", "n_parts")),
    ("TraceViewer", "register_materialized", ("dir",)),
]

# Forwarders deleted with no argument besides self.
OLD_NO_ARGUMENTS = [
    ("DataFrame", "to_ipc"),
    ("DataFrame", "keys"),
    ("LazyFrame", "explain"),
    ("TraceViewer", "materialize_dir"),
]


@pytest.fixture
def instances(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    with gzip.open(path, "wt") as f:
        f.write(
            json.dumps({"ph": 1, "name": "a", "cat": "C", "pid": 1, "tid": 1, "ts": 1, "dur": 2})
            + "\n"
        )
    df = DataFrame({"a": [1, 2, 3], "b": [4, 5, 6]})
    return {"DataFrame": df, "LazyFrame": df.lazy(), "TraceViewer": TraceViewer(path)}


@pytest.mark.parametrize(
    "cls,method,keywords", OLD_KEYWORDS, ids=[f"{c}.{m}" for c, m, _ in OLD_KEYWORDS]
)
def test_native_frame_method_accepts_the_old_keyword_names(instances, cls, method, keywords):
    native = getattr(_ext, cls)
    assert method in native.__dict__, f"{cls}.{method} is not a native method"
    assert method not in type(instances[cls]).__dict__, (
        f"{cls}.{method} is a Python forwarder again"
    )
    for count in range(1, len(keywords) + 1):
        try:
            getattr(instances[cls], method)(**{k: 1 for k in keywords[:count]})
        except TypeError as exc:
            assert "keyword" not in str(exc), (cls, method, keywords[:count], str(exc))
        except Exception:
            pass


@pytest.mark.parametrize(
    "cls,method", OLD_NO_ARGUMENTS, ids=[f"{c}.{m}" for c, m in OLD_NO_ARGUMENTS]
)
def test_deleted_forwarders_are_the_native_methods(instances, cls, method):
    assert method in getattr(_ext, cls).__dict__
    assert method not in type(instances[cls]).__dict__ or cls == "TraceViewer"
    getattr(instances[cls], method)


def test_keyword_calls_give_the_positional_results(instances):
    df = instances["DataFrame"]
    assert df.column_index(name="b") == df.column_index("b") == 1
    assert df.partition_id(keys="a", n_parts=2).to_list() == df.partition_id("a", 2).to_list()
    assert df.query(dsl="a > 1").to_dict() == df.query("a > 1").to_dict()
    assert df.query(select=["a"], order_by="a", descending=True).to_dict() == {"a": [3, 2, 1]}


def test_query_takes_every_old_keyword_name(instances):
    # DataFrame.query is a Python method since a predicate-only query labels
    # its rows (pandas), so it is not in OLD_KEYWORDS, which needs a native one.
    df = instances["DataFrame"]
    out = df.query(
        dsl="a > 1",
        group_by=None,
        aggs=None,
        select=None,
        order_by=None,
        descending=False,
        limit=None,
    )
    assert out.index.to_list() == [1, 2]
    assert df.query(select=["a"], order_by="a", limit=1).to_dict() == {"a": [1]}


def test_a_wrong_keyword_is_still_an_error(instances):
    df = instances["DataFrame"]
    with pytest.raises(TypeError, match="unexpected keyword argument 'nope'"):
        df.column_index("a", nope=1)
    with pytest.raises(TypeError, match="keyword"):
        df.partition_id(keys="a", n_parts=2, extra=1)
    assert isinstance(instances["LazyFrame"], LazyFrame)
