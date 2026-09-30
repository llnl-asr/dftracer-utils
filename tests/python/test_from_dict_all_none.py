"""A list with no value imports as a string column of nulls
(openspec change from-dict-all-none)."""

import pandas as pd

from dftracer.utils import DataFrame, Series
from dftracer.utils.enums import DType


def _types(frame):
    return {name: dtype for name, dtype in zip(frame.columns, frame.dtypes)}


def test_proposal_repro_dict_of_none():
    f = DataFrame.from_dict({"x": [None, None]})
    assert _types(f) == {"x": DType.STRING}
    assert len(f) == 2 and f["x"].null_count == 2 and f["x"].to_list() == [None, None]


def test_next_to_a_typed_column():
    f = DataFrame.from_dict({"x": [None, None], "y": [1, 2]})
    assert _types(f) == {"x": DType.STRING, "y": DType.INT64}
    assert f["x"].null_count == 2 and f["y"].to_list() == [1, 2]


def test_empty_list_is_a_string_column_of_no_rows():
    f = DataFrame.from_dict({"x": []})
    assert _types(f) == {"x": DType.STRING} and len(f) == 0
    s = Series.from_list([])
    assert s.dtype == DType.STRING and len(s) == 0


def test_the_entry_points_agree():
    from_dict = DataFrame.from_dict({"x": [None, None]})
    constructor = DataFrame({"x": [None, None]})
    from_series = Series.from_list([None, None])
    from_pandas = DataFrame.from_pandas(
        pd.DataFrame({"x": pd.Series([None, None], dtype="object")})
    )
    assert from_series.dtype == DType.STRING
    for frame in (from_dict, constructor, from_pandas):
        assert _types(frame) == {"x": DType.STRING}
        assert frame["x"].null_count == 2


def test_tuples_and_a_single_none():
    assert DataFrame.from_dict({"x": (None,)})["x"].dtype == DType.STRING
    assert Series.from_list((None,)).null_count == 1


def test_an_explicit_type_wins():
    s = Series.from_list([None, None], dtype=DType.INT64)
    assert s.dtype == DType.INT64 and s.null_count == 2
    s = Series.from_list([], dtype=DType.FLOAT64)
    assert s.dtype == DType.FLOAT64 and len(s) == 0


def test_a_later_cast_gives_another_type():
    col = DataFrame.from_dict({"x": [None, None, None]})["x"].astype("float64")
    assert col.dtype == DType.FLOAT64 and len(col) == 3 and col.null_count == 3


def test_a_column_with_a_value_is_typed_as_before():
    f = DataFrame.from_dict({"x": [None, 3], "s": [None, "a"], "f": [None, 1.5]})
    assert _types(f) == {"x": DType.INT64, "s": DType.STRING, "f": DType.FLOAT64}
    assert f["x"].to_list() == [None, 3]


def test_the_frame_is_usable_after_import():
    f = DataFrame.from_dict({"x": [None, None], "y": [1, 2]})
    out = f.with_columns(z=7)
    assert out["z"].to_list() == [7, 7] and out["x"].null_count == 2
    assert len(f.concat(f)) == 4
