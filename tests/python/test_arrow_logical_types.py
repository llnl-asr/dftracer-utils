#!/usr/bin/env python3
"""Python-surface exercise of the 15 Arrow logical types the C++ engine added
(Float16, Date32/64, Time32/64, Timestamp, Duration, Decimal128/256,
FixedSizeBinary, LargeString/Binary, LargeList, FixedSizeList, Map).

Parametrized over ``_TYPE_CASES`` so a type missing a case is visible. Each
case supplies a pyarrow array (or array factory) and the expected DType; the
tests exercise construction, ``.type``, ``to_arrow`` value round-trip,
``to_pandas``/``to_numpy``, and a representative op set, asserting VALUES.
"""

import datetime
from decimal import Decimal

import numpy as np
import pytest

pa = pytest.importorskip("pyarrow")
pq = pytest.importorskip("pyarrow.parquet")

from dftracer.utils.dataframe import DataFrame  # noqa: E402
from dftracer.utils.enums import DType  # noqa: E402
from dftracer.utils.series import Series  # noqa: E402

# name -> (pyarrow array, expected DType, expected pylist).
_TYPE_CASES = {
    "float16": (
        pa.array(np.array([1.0, 2.5, 3.0], dtype=np.float16)),
        DType.FLOAT16,
        [1.0, 2.5, 3.0],
    ),
    "date32": (
        pa.array([datetime.date(2021, 1, 1), datetime.date(2022, 6, 15)], type=pa.date32()),
        DType.DATE32,
        [datetime.date(2021, 1, 1), datetime.date(2022, 6, 15)],
    ),
    "date64": (
        pa.array([datetime.date(2021, 1, 1), datetime.date(2022, 6, 15)], type=pa.date64()),
        DType.DATE64,
        [datetime.date(2021, 1, 1), datetime.date(2022, 6, 15)],
    ),
    "time32": (
        pa.array([datetime.time(1, 2, 3), datetime.time(4, 5, 6)], type=pa.time32("s")),
        DType.TIME32,
        [datetime.time(1, 2, 3), datetime.time(4, 5, 6)],
    ),
    "time64": (
        pa.array([datetime.time(1, 2, 3, 4), datetime.time(5, 6, 7, 8)], type=pa.time64("us")),
        DType.TIME64,
        [datetime.time(1, 2, 3, 4), datetime.time(5, 6, 7, 8)],
    ),
    "timestamp": (
        pa.array(
            [datetime.datetime(2021, 1, 1), datetime.datetime(2022, 1, 1)],
            type=pa.timestamp("us"),
        ),
        DType.TIMESTAMP,
        [datetime.datetime(2021, 1, 1), datetime.datetime(2022, 1, 1)],
    ),
    "duration": (
        pa.array([1, 2, 3], type=pa.duration("us")),
        DType.DURATION,
        [
            datetime.timedelta(microseconds=1),
            datetime.timedelta(microseconds=2),
            datetime.timedelta(microseconds=3),
        ],
    ),
    "decimal128": (
        pa.array([Decimal("1.23"), Decimal("4.56")], type=pa.decimal128(10, 2)),
        DType.DECIMAL128,
        [Decimal("1.23"), Decimal("4.56")],
    ),
    "decimal256": (
        pa.array([Decimal("1.23"), Decimal("4.56")], type=pa.decimal256(20, 4)),
        DType.DECIMAL256,
        [Decimal("1.2300"), Decimal("4.5600")],
    ),
    "fixed_size_binary": (
        pa.array([b"ab", b"cd"], type=pa.binary(2)),
        DType.FIXED_SIZE_BINARY,
        [b"ab", b"cd"],
    ),
    "large_string": (
        pa.array(["hello", "world"], type=pa.large_string()),
        DType.LARGE_STRING,
        ["hello", "world"],
    ),
    "large_binary": (
        pa.array([b"x", b"y"], type=pa.large_binary()),
        DType.LARGE_BINARY,
        [b"x", b"y"],
    ),
    "large_list": (
        pa.array([[1, 2], [3]], type=pa.large_list(pa.int64())),
        DType.LARGE_LIST,
        [[1, 2], [3]],
    ),
    "fixed_size_list": (
        pa.array([[1, 2], [3, 4]], type=pa.list_(pa.int64(), 2)),
        DType.FIXED_SIZE_LIST,
        [[1, 2], [3, 4]],
    ),
    "map": (
        pa.array([[(1, "a"), (2, "b")], [(3, "c")]], type=pa.map_(pa.int64(), pa.string())),
        DType.MAP,
        [[(1, "a"), (2, "b")], [(3, "c")]],
    ),
}

_TYPE_NAMES = sorted(_TYPE_CASES)


# ---------------------------------------------------------------------------
# 1. DType <-> TypeId ordinal parity.
# ---------------------------------------------------------------------------


def test_dtype_covers_every_type_id_ordinal():
    # include/dftracer/utils/dataframe/types.h TypeId, in enum order.
    expected = [
        ("UNKNOWN", 0),
        ("BOOL", 1),
        ("INT8", 2),
        ("INT16", 3),
        ("INT32", 4),
        ("INT64", 5),
        ("UINT8", 6),
        ("UINT16", 7),
        ("UINT32", 8),
        ("UINT64", 9),
        ("FLOAT32", 10),
        ("FLOAT64", 11),
        ("STRING", 12),
        ("BINARY", 13),
        ("LIST", 14),
        ("STRUCT", 15),
        ("FLOAT16", 16),
        ("DATE32", 17),
        ("DATE64", 18),
        ("TIME32", 19),
        ("TIME64", 20),
        ("TIMESTAMP", 21),
        ("DURATION", 22),
        ("DECIMAL128", 23),
        ("DECIMAL256", 24),
        ("FIXED_SIZE_BINARY", 25),
        ("LARGE_STRING", 26),
        ("LARGE_BINARY", 27),
        ("LARGE_LIST", 28),
        ("FIXED_SIZE_LIST", 29),
        ("MAP", 30),
    ]
    actual = [(d.name, int(d)) for d in DType]
    assert actual == expected


def test_dtype_covers_every_new_logical_type_case():
    # A 16th logical type added here without a matching DType member fails
    # this loudly.
    for name, (_, dtype, _) in _TYPE_CASES.items():
        assert isinstance(dtype, DType), name


# ---------------------------------------------------------------------------
# 2. Series.from_arrow / .type / to_arrow round-trip, per type.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("name", _TYPE_NAMES)
def test_series_from_arrow_type_and_roundtrip(name):
    arr, dtype, expected = _TYPE_CASES[name]
    s = Series.from_arrow(arr)
    assert s.type == int(dtype), f"{name}: Series.type does not name {dtype.name}"
    assert len(s) == len(arr)
    rt = s.to_arrow().to_pylist()
    assert rt == expected, f"{name}: to_arrow round-trip lost values"


@pytest.mark.parametrize("name", _TYPE_NAMES)
def test_series_to_pandas(name):
    arr, _, expected = _TYPE_CASES[name]
    s = Series.from_arrow(arr)
    pdser = s.to_pandas()
    assert len(pdser) == len(expected)


@pytest.mark.parametrize("name", _TYPE_NAMES)
def test_series_to_numpy_does_not_crash(name):
    arr, _, _ = _TYPE_CASES[name]
    s = Series.from_arrow(arr)
    arr_np = s.to_numpy()
    assert len(arr_np) == len(s)


@pytest.mark.parametrize("name", _TYPE_NAMES)
def test_series_repr_does_not_crash(name):
    arr, _, _ = _TYPE_CASES[name]
    s = Series.from_arrow(arr)
    assert isinstance(repr(s), str)
    assert isinstance(str(s), str)


# gather_column has no case for FixedSizeList/Map; head()/take() on them
# raises RuntimeError rather than crashing or returning a broken column.
_HEAD_UNSUPPORTED = {"fixed_size_list", "map"}


@pytest.mark.parametrize("name", _TYPE_NAMES)
def test_series_head_and_filter(name):
    arr, _, expected = _TYPE_CASES[name]
    s = Series.from_arrow(arr)
    if name in _HEAD_UNSUPPORTED:
        with pytest.raises(Exception):
            s.head(1)
    else:
        h = s.head(1)
        assert h.to_arrow().to_pylist() == expected[:1]

    mask = Series.from_arrow(pa.array([True] + [False] * (len(arr) - 1)))
    f = s.filter(mask)
    assert f.to_arrow().to_pylist() == expected[:1]


def test_head_preserves_time_unit_on_non_default_unit():
    arr = pa.array(
        [datetime.datetime(2021, 1, 1), datetime.datetime(2022, 1, 1)],
        type=pa.timestamp("s"),
    )
    s = Series.from_arrow(arr)
    h = s.head(1)
    assert h.to_arrow().to_pylist() == [datetime.datetime(2021, 1, 1)]


@pytest.mark.parametrize("name", _TYPE_NAMES)
def test_series_sort_or_clear_error(name):
    # Nested container types have no per-row orderable value and must
    # refuse loudly, not silently drop/garble rows.
    arr, _, _ = _TYPE_CASES[name]
    s = Series.from_arrow(arr)
    nested = name in ("large_list", "fixed_size_list", "map")
    if nested:
        with pytest.raises(Exception):
            s.sort()
    else:
        sorted_s = s.sort()
        assert len(sorted_s) == len(s)


# ---------------------------------------------------------------------------
# 3. DataFrame-level ops: construction, sort_by, filter, unique, group_by.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("name", _TYPE_NAMES)
def test_dataframe_from_arrow_column_type_and_roundtrip(name):
    arr, dtype, expected = _TYPE_CASES[name]
    table = pa.table({"id": pa.array(range(len(arr)), type=pa.int64()), "v": arr})
    df = DataFrame.from_arrow(table)
    assert len(df) == len(arr)
    col = df["v"]
    assert col.type == int(dtype)
    assert col.to_arrow().to_pylist() == expected


@pytest.mark.parametrize("name", _TYPE_NAMES)
def test_dataframe_filter_and_head(name):
    arr, _, expected = _TYPE_CASES[name]
    table = pa.table({"id": pa.array(range(len(arr)), type=pa.int64()), "v": arr})
    df = DataFrame.from_arrow(table)
    mask = Series.from_arrow(pa.array([True] + [False] * (len(arr) - 1)))

    if name in _HEAD_UNSUPPORTED:
        h = df.head(1)
        with pytest.raises(Exception):
            h["v"]
        f = df.filter(mask)
        with pytest.raises(Exception):
            f["v"]
    else:
        h = df.head(1)
        assert h["v"].to_arrow().to_pylist() == expected[:1]
        f = df.filter(mask)
        assert f["v"].to_arrow().to_pylist() == expected[:1]


# Types the group-key hash path supports today; the rest raise a ValueError
# naming the type. See src/dftracer/utils/dataframe/agg/state.cpp
# is_group_key_type.
_GROUP_KEY_SUPPORTED = {
    "float16",
    "date32",
    "date64",
    "time32",
    "time64",
    "timestamp",
    "duration",
    "decimal128",
    "decimal256",
    "fixed_size_binary",
    "large_string",
    "large_binary",
}


@pytest.mark.parametrize("name", _TYPE_NAMES)
def test_dataframe_group_by_supported_or_clear_error(name):
    arr, _, _ = _TYPE_CASES[name]
    if len(arr) < 2:
        pytest.skip("case too short to exercise grouping")
    key = pa.concat_arrays([arr.slice(0, 1), arr.slice(0, 1)])
    table = pa.table({"k": key, "v": pa.array([1, 2], type=pa.int64())})
    df = DataFrame.from_arrow(table)
    if name in _GROUP_KEY_SUPPORTED:
        g = df.group_by("k", "count")
        d = g.to_arrow().to_pydict()
        assert d["count"] == [2]
    else:
        with pytest.raises(ValueError, match="group key"):
            df.group_by("k", "count")


# ---------------------------------------------------------------------------
# 4. Unsupported-source errors: the from_arrow_type false-return regression.
# ---------------------------------------------------------------------------


def test_series_from_arrow_unsupported_type_raises_not_silent():
    # A column type the engine genuinely does not import (Arrow null type)
    # must raise, never hand back an invalid Series with no exception.
    with pytest.raises(ValueError):
        Series.from_arrow(pa.array([None, None], type=pa.null()))


def test_dataframe_from_arrow_unsupported_column_raises_not_silent():
    table = pa.table({"ok": pa.array([1, 2]), "bad": pa.array([None, None], type=pa.null())})
    with pytest.raises(ValueError):
        DataFrame.from_arrow(table)


def test_series_scalar_compare_on_unsupported_domain_raises():
    # Float16/Decimal128/256 have no numeric-dispatchable scalar compare path
    # yet (dataframe/kernels/comparison.cpp gates on is_numeric_dispatchable);
    # calling compare()/eq() must raise, not return a broken Series.
    s = Series.from_arrow(pa.array(np.array([1.0, 2.0], dtype=np.float16)))
    with pytest.raises(Exception):
        s.eq(1.0)

    dec = Series.from_arrow(pa.array([Decimal("1.23")], type=pa.decimal128(10, 2)))
    with pytest.raises(Exception):
        dec.eq(1)


# ---------------------------------------------------------------------------
# 5. Parquet round-trip: the real user path (from_parquet).
# ---------------------------------------------------------------------------


def test_from_parquet_roundtrip_all_writable_types(tmp_path):
    # Map and full-precision Date64/Time64[ns] are not exercised here:
    # pyarrow's Parquet writer itself downcasts date64 -> date32 and loses
    # sub-microsecond time precision, which is a pyarrow/Parquet format
    # limitation, not something this test can observe as our defect.
    columns = {
        "id": pa.array([1, 2, 3], type=pa.int64()),
        "f16": pa.array(np.array([1.5, 2.5, 3.5], dtype=np.float16)),
        "date32": pa.array(
            [datetime.date(2021, 1, 1), datetime.date(2022, 2, 2), datetime.date(2023, 3, 3)],
            type=pa.date32(),
        ),
        "time32": pa.array([datetime.time(1, 2, 3)] * 3, type=pa.time32("ms")),
        "time64": pa.array([datetime.time(1, 2, 3, 4)] * 3, type=pa.time64("us")),
        "ts": pa.array([datetime.datetime(2021, 1, 1, 1, 1, 1)] * 3, type=pa.timestamp("us")),
        "ts_tz": pa.array(
            [datetime.datetime(2021, 1, 1, 1, 1, 1)] * 3, type=pa.timestamp("us", tz="UTC")
        ),
        "dur": pa.array([1, 2, 3], type=pa.duration("us")),
        "dec128": pa.array([Decimal("1.23")] * 3, type=pa.decimal128(10, 2)),
        "dec256": pa.array([Decimal("1.23")] * 3, type=pa.decimal256(20, 4)),
        "fsb": pa.array([b"ab", b"cd", b"ef"], type=pa.binary(2)),
        "lstr": pa.array(["a", "b", "c"], type=pa.large_string()),
        "lbin": pa.array([b"x", b"y", b"z"], type=pa.large_binary()),
        "llist": pa.array([[1, 2], [3], [4, 5, 6]], type=pa.large_list(pa.int64())),
        "fslist": pa.array([[1, 2], [3, 4], [5, 6]], type=pa.list_(pa.int64(), 2)),
        "m": pa.array(
            [[(1, "a"), (2, "b")], [(3, "c")], []], type=pa.map_(pa.int64(), pa.string())
        ),
    }
    table = pa.table(columns)
    path = tmp_path / "arrow_types.parquet"
    pq.write_table(table, str(path))

    df = DataFrame.from_parquet(str(path))
    assert len(df) == 3
    for name in columns:
        expected = table.column(name).combine_chunks().to_pylist()
        actual = df[name].to_arrow().to_pylist()
        assert actual == expected, f"parquet round-trip lost values for {name!r}"
