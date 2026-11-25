# SPDX-License-Identifier: MIT
"""Conversion and table edges found in review: chunked dictionaries, field
metadata through ``where``, timestamp units, wide dates, MultiIndex levels with
missing labels, parametric label types and the nested types that
``values_like`` builds."""

import datetime
import decimal

import numpy as np
import pytest

pd = pytest.importorskip("pandas")
pa = pytest.importorskip("pyarrow")

from dftracer.utils import (  # noqa: E402
    DataFrame,
    DType,
    Series,
    TraceWriteOptions,
    _convert,
    _reindex,
    _series_index,
    _table,
    col,
    where,
)
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402

CHUNKED = 5
DICTIONARY = 2


def chunked_category() -> DataFrame:
    table = pa.table({"c": pa.array(["a", "b", "a", "c", "b", "a"]).dictionary_encode()})
    frame = DataFrame.from_arrow(table).lazy().collect(batch_size=2)
    assert _ext.DataFrame.__getitem__(frame, "c").encoding == CHUNKED
    return frame


def test_chunked_dictionary_column_is_a_category_in_the_table_header():
    frame = chunked_category()
    field = _table._prepare_native(frame, "t", None)["fields"][0]
    assert field["dtype"] == "category"
    assert field["categories"] == ["a", "b", "c"]


def test_chunked_dictionary_column_writes_a_category_table(tmp_path):
    from dftracer.utils.trace import TraceViewer

    chunked_category().write_table(
        str(tmp_path / "t"),
        name="t",
        mode="overwrite",
        options=TraceWriteOptions(build_index=False),
    )
    back = TraceViewer(str(tmp_path / "t")).collect().to_pandas()
    assert isinstance(back["c"].dtype, pd.CategoricalDtype)
    assert back["c"].astype(object).tolist() == ["a", "b", "a", "c", "b", "a"]


def test_chunked_dictionary_restores_to_an_arrow_dtype_of_plain_values():
    native = _ext.DataFrame.__getitem__(chunked_category(), "c")
    field = {"name": "c", "dtype": "string[pyarrow]", "arrow_type": "string"}
    restored = _table._restore("c", native, field)
    assert restored.dtype.pyarrow_dtype == pa.string()
    assert restored.tolist() == ["a", "b", "a", "c", "b", "a"]


def test_chunked_dictionary_converts_to_a_polars_categorical():
    pl = pytest.importorskip("polars")
    native = _ext.DataFrame.__getitem__(chunked_category(), "c")
    out = _convert._polars_native(native, pl)
    assert out.dtype == pl.Categorical
    assert out.cast(pl.String).to_list() == ["a", "b", "a", "c", "b", "a"]


def test_where_on_a_pyarrow_table_keeps_field_metadata_and_nullability():
    schema = pa.schema(
        [
            pa.field("x", pa.int64(), nullable=False, metadata={"unit": "ms"}),
            pa.field("y", pa.string(), nullable=True, metadata={"note": "n"}),
        ]
    )
    table = pa.table({"x": [1, 2, 3], "y": ["a", "b", "c"]}, schema=schema)
    out = where(table, col("x") > 1)
    assert out.num_rows == 2
    for name in ("x", "y"):
        got, want = out.schema.field(name), schema.field(name)
        assert got.nullable == want.nullable, name
        assert got.metadata == want.metadata, name
    assert out.column("x").to_pylist() == [2, 3]


def test_series_of_a_timestamp_applies_the_unit_and_zone_of_the_dtype():
    base = Series.from_arrow(pa.array([1_000_000, 2_000_000], pa.timestamp("us")))
    out = Series(base, dtype=pa.timestamp("ns", tz="UTC"))
    assert out.dtype == DType.TIMESTAMP
    assert out.to_arrow().type == pa.timestamp("ns", tz="UTC")
    assert out.to_arrow().to_pylist() == [
        pa.scalar(1_000_000_000, pa.timestamp("ns", tz="UTC")).as_py(),
        pa.scalar(2_000_000_000, pa.timestamp("ns", tz="UTC")).as_py(),
    ]


def test_series_of_a_duration_applies_the_unit_of_the_dtype():
    base = Series.from_arrow(pa.array([1, 2], pa.duration("s")))
    out = Series(base, dtype=pa.duration("ms"))
    assert out.to_arrow().type == pa.duration("ms")
    assert out.to_arrow().cast(pa.int64()).to_pylist() == [1000, 2000]


def test_series_of_a_timestamp_into_a_duration_is_refused():
    base = Series.from_arrow(pa.array([1], pa.timestamp("us")))
    with pytest.raises(TypeError):
        Series(base, dtype=pa.duration("us"))


def test_wide_numpy_dates_build_in_their_own_unit():
    far = np.datetime64("2500-01-01", "s")
    out = _convert.temporal_from_values([far, None], DType.TIMESTAMP, "s")
    assert out.to_arrow().type == pa.timestamp("s")
    assert out.to_arrow().cast(pa.int64()).to_pylist() == [int(far.astype(np.int64)), None]
    span = np.timedelta64(10**12, "s")
    out = _convert.temporal_from_values([span], DType.DURATION, "s")
    assert out.to_arrow().cast(pa.int64()).to_pylist() == [10**12]


def test_multiindex_level_with_missing_labels_has_one_dtype_on_both_paths():
    # 2 * len(values) <= len(codes) takes the gather path, the other the pandas one
    few = pd.MultiIndex(levels=[[1, 2]], codes=[[0, 1, -1, 0, 1, 0]], names=["a"])
    many = pd.MultiIndex(levels=[[1, 2, 3, 4, 5, 6]], codes=[[0, 1, -1]], names=["a"])
    a = _series_index._pandas_level(few, 0)
    b = _series_index._pandas_level(many, 0)
    assert a.dtype == b.dtype == DType.INT64
    assert a.to_list() == [1, 2, None, 1, 2, 1]
    assert b.to_list() == [1, 2, None]


def test_decimal256_labels_are_a_parametric_type():
    assert _reindex._PARAMETRIC >= {DType.DECIMAL128, DType.DECIMAL256}


def test_values_like_says_a_decimal256_has_no_native_build():
    like = Series.from_arrow(pa.array([decimal.Decimal("1.50")], pa.decimal256(40, 2)))
    with pytest.raises(TypeError, match="decimal256"):
        _convert.values_like([decimal.Decimal("2.50")], like)


def test_values_like_keeps_the_int32_list_type():
    like = Series.from_arrow(pa.array([[1, 2], [3]], pa.list_(pa.int32())))
    out = _convert.values_like([[4, 5], None], like)
    assert out.to_arrow().type == pa.list_(pa.int32())
    assert out.to_arrow().to_pylist() == [[4, 5], None]


def test_values_like_keeps_the_fixed_size_binary_type():
    like = Series.from_arrow(pa.array([b"ab", b"cd"], pa.binary(2)))
    out = _convert.values_like([b"ef", None], like)
    assert out.to_arrow().type == pa.binary(2)
    assert out.to_arrow().to_pylist() == [b"ef", None]


def test_values_like_refuses_a_nested_value_that_does_not_fit():
    like = Series.from_arrow(pa.array([[1, 2]], pa.list_(pa.int32())))
    with pytest.raises(ValueError):
        _convert.values_like([[2**40]], like)


def test_one_helper_gives_the_missing_scalars():
    for value in (None, pd.NA, pd.NaT, float("nan"), np.datetime64("NaT", "s"), np.float32("nan")):
        assert _convert.missing(value), value
    for value in (0, "", datetime.timedelta(0), np.float64(0)):
        assert not _convert.missing(value), value


def test_timedelta_nanos_is_exact_for_python_and_pandas_spans():
    assert _convert.timedelta_nanos(datetime.timedelta(days=1, seconds=2, microseconds=3)) == (
        86_402_000_003_000
    )
    assert _convert.timedelta_nanos(pd.Timedelta(5, "ns")) == 5
    assert _convert.timedelta_nanos(datetime.timedelta(microseconds=-1)) == -1000


def test_dtype_is_text_names_the_string_types():
    texts = {d for d in DType if d.is_text}
    assert texts == {DType.STRING, DType.LARGE_STRING}


def _round_trip(tmp_path, frame):
    from dftracer.utils.trace import TraceViewer

    DataFrame.from_pandas(frame).write_table(
        str(tmp_path / "t"),
        name="t",
        mode="overwrite",
        options=TraceWriteOptions(build_index=False),
    )
    return TraceViewer(str(tmp_path / "t")).collect()


def test_table_multiindex_integer_level_is_built_from_the_distinct_values():
    column = Series.from_list([30, 10, 30, 20, 10])
    field = {"name": "k", "dtype": "int64"}
    level, codes = _table._index_level("k", column, field, set)
    want = pd.MultiIndex.from_arrays([[30, 10, 30, 20, 10]])
    assert level.equals(want.levels[0])
    assert codes.tolist() == want.codes[0].tolist()


def test_table_multiindex_integer_level_keeps_a_null_as_a_missing_code():
    column = Series.from_list([2, None, 1, 2])
    field = {"name": "k", "dtype": "Int64"}
    level, codes = _table._index_level("k", column, field, set)
    assert level.tolist() == [1, 2]
    assert str(level.dtype) == "Int64"
    assert codes.tolist() == [1, -1, 0, 1]


def test_table_multiindex_with_integer_and_string_levels_round_trips(tmp_path):
    frame = pd.DataFrame(
        {"v": [1.5, 2.5, 3.5, 4.5]},
        index=pd.MultiIndex.from_arrays(
            [np.array([7, 3, 7, 3], dtype=np.int64), ["b", "a", "a", "b"]], names=["n", "s"]
        ),
    )
    back = _round_trip(tmp_path, frame).to_pandas()
    pd.testing.assert_frame_equal(back, frame)


def test_values_like_builds_large_lists_and_takes_the_item_type_of_an_empty_list():
    for like in (
        Series.from_arrow(pa.array([], pa.large_list(pa.int32()))),
        Series.from_arrow(pa.array([None], pa.list_(pa.int32()))),
    ):
        out = _convert.values_like([[4], None], like)
        assert out.to_arrow().type == like.to_arrow().type
        assert out.to_arrow().to_pylist() == [[4], None]


def test_values_like_reads_the_fixed_size_binary_width_from_the_type():
    like = Series.from_arrow(pa.array([None], pa.binary(2)))
    assert _convert.values_like([b"ab"], like).to_arrow().to_pylist() == [b"ab"]
    with pytest.raises(TypeError, match="3 bytes"):
        _convert.values_like([b"abc"], Series.from_arrow(pa.array([None], pa.binary(3))))


def _big_span():
    # pandas 2.0 overflows on Timedelta(days=200_000); a NumPy value keeps its unit.
    return pd.Timedelta(np.timedelta64(200_000 * 86_400, "s"))


def test_a_span_past_int64_nanoseconds_keeps_its_exact_value():
    span = _big_span()
    assert _convert.timedelta_nanos(span) == 200_000 * 86_400 * 10**9
    assert (
        _convert.timedelta_nanos(datetime.timedelta(days=1, microseconds=3))
        == 86_400 * 10**9 + 3_000
    )


def test_a_dictionary_of_lists_skips_the_codes_under_null_rows():
    validity = pa.py_buffer(bytes([0b10]))
    codes = pa.array([999, 0], pa.int32()).buffers()[1]
    indices = pa.Array.from_buffers(pa.int32(), 2, [validity, codes], null_count=1)
    d = pa.DictionaryArray.from_arrays(indices, pa.array([[22]], pa.list_(pa.int32())))
    d.validate(full=True)
    s = Series.from_arrow(d)
    assert s.child(0).to_list() == [22]
    assert s.materialize().to_list() == [None, [22]]
    empty = pa.DictionaryArray.from_arrays(
        pa.array([None, None], pa.int32()), pa.array([], pa.list_(pa.int32()))
    )
    assert Series.from_arrow(empty).materialize().to_list() == [None, None]


def test_a_chunked_numeric_dictionary_restores_to_plain_arrow_values():
    table = pa.table({"x": pa.array([1, 2, 1, 2]).dictionary_encode()})
    frame = DataFrame.from_arrow(table).lazy().collect(batch_size=2)
    restored = _table._restore(
        "x", _ext.DataFrame.__getitem__(frame, "x"), {"name": "x", "dtype": "int64[pyarrow]"}
    )
    assert str(restored.dtype) == "int64[pyarrow]"
    assert list(restored) == [1, 2, 1, 2]


def test_pandas_points_and_spans_past_int64_nanoseconds_build_in_their_unit():
    span = _convert.temporal_from_values([_big_span()], DType.DURATION, "s")
    assert span.to_list() == [datetime.timedelta(days=200_000)]
    point = _convert.temporal_from_values(
        [pd.Timestamp("2500-01-01").as_unit("s")], DType.TIMESTAMP, "s"
    )
    assert point.to_list() == [datetime.datetime(2500, 1, 1)]


def test_a_pandas_timestamp_in_seconds_past_the_microsecond_range_keeps_its_value():
    point = pd.Timestamp(np.datetime64(9_000_000_000_000_000, "s"))
    built = _convert.temporal_from_values([point], DType.TIMESTAMP, "s")
    assert built.to_numpy().astype("int64").tolist() == [9_000_000_000_000_000]
