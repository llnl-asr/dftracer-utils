"""set_union(typed=True) returns a list of the values in their own type; the
default stays the separator-joined text."""

import numpy as np
import pandas as pd

from dftracer.utils import DataFrame, col

SEP = "\x1e"


def frame(**cols):
    return DataFrame.from_pandas(pd.DataFrame(cols))


def agg(df, c, typed):
    return df.group_by("k").agg(u=col(c).set_union(typed=typed)).sort("k").to_pandas()


def test_integers_ordered_by_value():
    df = frame(k=[1, 1, 1, 2], n=[10, 9, 10, 5])
    out = agg(df, "n", True)
    assert [list(x) for x in out["u"]] == [[9, 10], [5]]


def test_default_text_is_unchanged():
    df = frame(k=[1, 1, 1], n=[10, 9, 10])
    assert list(agg(df, "n", False)["u"]) == ["10" + SEP + "9"]
    assert list(df.group_by("k").agg(u=col("n").set_union()).to_pandas()["u"]) == ["10" + SEP + "9"]


def test_string_with_separator_is_kept():
    df = frame(k=[1, 1, 1], s=["b" + SEP + "c", "a", "a"])
    assert [list(x) for x in agg(df, "s", True)["u"]] == [["a", "b" + SEP + "c"]]


def test_floats_bools_and_unsigned():
    df = frame(
        k=[1, 1, 1],
        f=[0.1, 1e300, 0.1],
        b=[True, False, True],
        u=np.array([2**64 - 1, 3, 3], dtype="uint64"),
    )
    out = (
        df.group_by("k")
        .agg(
            f=col("f").set_union(typed=True),
            b=col("b").set_union(typed=True),
            u=col("u").set_union(typed=True),
        )
        .to_pandas()
    )
    assert list(out["f"][0]) == [0.1, 1e300]
    assert list(out["b"][0]) == [False, True]
    assert list(out["u"][0]) == [3, 2**64 - 1]


def test_nulls_only_group_is_an_empty_list():
    df = frame(k=[1, 2], n=pd.array([5, None], dtype="Int64"))
    out = agg(df, "n", True)
    assert [list(x) for x in out["u"]] == [[5], []]


def test_lazy_schema_and_collect_agree():
    df = frame(k=[1, 1, 2], n=[10, 9, 7])
    lazy = df.lazy().group_by("k").agg(u=col("n").set_union(typed=True))
    assert lazy.schema["u"].name == "LIST"
    assert lazy.schema["k"].name == "INT64"
    out = lazy.collect().to_pandas().sort_values("k")
    assert [list(x) for x in out["u"]] == [[9, 10], [7]]
    assert df.lazy().group_by("k").agg(u=col("n").set_union()).schema["u"].name == "STRING"
