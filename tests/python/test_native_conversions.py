"""Conversions of every column type without pyarrow.

The columns are built from Python values (or, for a few types with no Python
constructor, imported from pyarrow), and the results are compared with what
pyarrow gives when it is installed. With no pyarrow the tests that need it skip.
"""

import datetime as dt
import decimal
import pickle
import sys

import numpy as np
import pytest

import dftracer.utils as dftu
from dftracer.utils import DType
from dftracer.utils import dftracer_utils_ext as _ext

pytestmark = pytest.mark.skipif(
    not hasattr(_ext, "_series_retype"), reason="extension predates native conversions"
)

D = decimal.Decimal


def _pa():
    return pytest.importorskip("pyarrow")


def _arrow_cases():
    pa = _pa()
    return {
        "date32": pa.array([dt.date(2023, 12, 8), None, dt.date(1969, 7, 20)], pa.date32()),
        "date64": pa.array([dt.date(2023, 12, 8), None], pa.date64()),
        "time32s": pa.array([dt.time(1, 2, 3), None], pa.time32("s")),
        "time32ms": pa.array([dt.time(1, 2, 3, 4000), None], pa.time32("ms")),
        "time64us": pa.array([dt.time(23, 59, 59, 999999), None], pa.time64("us")),
        "time64ns": pa.array([dt.time(1, 2, 3, 4), None], pa.time64("ns")),
        "ts_s": pa.array([dt.datetime(2020, 1, 2, 3, 4, 5), None], pa.timestamp("s")),
        "ts_ms_tz": pa.array(
            [dt.datetime(2020, 1, 2, 3, 4, 5, 678000), None], pa.timestamp("ms", tz="Europe/Paris")
        ),
        "ts_us_utc": pa.array(
            [dt.datetime(1960, 1, 2, 3, 4, 5, 678901), None], pa.timestamp("us", tz="UTC")
        ),
        "ts_ns": pa.array([dt.datetime(2020, 1, 2, 3, 4, 5, 678901), None], pa.timestamp("ns")),
        "dur_s": pa.array(
            [dt.timedelta(days=2, seconds=5), None, dt.timedelta(days=-3, seconds=7)],
            pa.duration("s"),
        ),
        "dur_us": pa.array([dt.timedelta(microseconds=-1), None], pa.duration("us")),
        "dec128": pa.array([D("12.345"), None, D("-0.001")], pa.decimal128(10, 3)),
        "dec256": pa.array(
            [D("12345678901234567890123456789012345678.5"), None], pa.decimal256(40, 1)
        ),
        "f16": pa.array(np.array([1.5, 2.0], dtype=np.float16)),
        "fsb": pa.array([b"ab", None, b"cd"], pa.binary(2)),
        "fsl": pa.array([[1, 2], [3, 4], None], pa.list_(pa.int32(), 2)),
        "map": pa.array([[("a", 1), ("b", 2)], None, []], pa.map_(pa.string(), pa.int64())),
        "list_ts": pa.array([[dt.datetime(2020, 1, 1)], None], pa.list_(pa.timestamp("us"))),
    }


def test_to_list_equals_pyarrow_for_every_type():
    for name, arr in _arrow_cases().items():
        if sys.version_info < (3, 9) and getattr(arr.type, "tz", None):
            continue  # a zoned value needs zoneinfo
        s = dftu.Series.from_arrow(arr)
        assert s._native.to_pylist() == arr.to_pylist(), name
        assert s.to_list() == arr.to_pylist(), name
        assert s[0] == arr.to_pylist()[0], name


def test_physical_shares_the_integers():
    s = dftu.Series.from_arrow(_pa().array([1, None, 3], _pa().timestamp("ms")))
    phys = s._native.physical()
    assert phys.type == int(DType.INT64)
    assert phys.null_count == 1
    assert s._native.nbytes >= 24


def test_to_numpy_temporal():
    arr = _arrow_cases()
    d = dftu.Series.from_arrow(arr["date32"]).to_numpy()
    assert d.dtype == np.dtype("datetime64[D]")
    assert d[0] == np.datetime64("2023-12-08") and np.isnat(d[1])
    t = dftu.Series.from_arrow(arr["ts_ms_tz"]).to_numpy()
    assert t.dtype == np.dtype("datetime64[ms]")
    assert t[0] == np.datetime64("2020-01-02T03:04:05.678") and np.isnat(t[1])
    r = dftu.Series.from_arrow(arr["dur_s"]).to_numpy()
    assert r.dtype == np.dtype("timedelta64[s]")
    assert r[0] == np.timedelta64(2 * 86400 + 5, "s") and np.isnat(r[1])
    obj = dftu.Series.from_arrow(arr["dec128"]).to_numpy()
    assert obj.dtype == object and obj[0] == D("12.345") and obj[1] is None
    f16 = dftu.Series.from_arrow(arr["f16"]).to_numpy()
    assert f16.dtype == np.float16 and f16.tolist() == [1.5, 2.0]


def test_to_pandas_and_polars_temporal():
    arr = _arrow_cases()
    pd = pytest.importorskip("pandas")
    tz = dftu.Series.from_arrow(arr["ts_ms_tz"]).to_pandas()
    assert str(tz.dtype).startswith("datetime64") and "Europe/Paris" in str(tz.dtype)
    assert tz[0].hour == 4 and pd.isna(tz[1])
    naive = dftu.Series.from_arrow(arr["ts_s"]).to_pandas()
    assert naive.dt.tz is None and naive[0] == pd.Timestamp("2020-01-02 03:04:05")
    pl = pytest.importorskip("polars")
    out = dftu.Series.from_arrow(arr["ts_ms_tz"]).to_polars()
    assert out.dtype == pl.Datetime("ms", "Europe/Paris")
    assert out[0].hour == 4 and out[1] is None
    assert dftu.Series.from_arrow(arr["date32"]).to_polars().dtype == pl.Date
    assert dftu.Series.from_arrow(arr["dur_s"]).to_polars().dtype == pl.Duration("ms")


def test_frame_to_pandas_with_zone():
    pd = pytest.importorskip("pandas")
    pa = _pa()
    frame = dftu.DataFrame.from_arrow(
        pa.table(
            {
                "t": pa.array([dt.datetime(2020, 1, 1, 12), None], pa.timestamp("us", tz="UTC")),
                "n": pa.array([1, 2]),
            }
        )
    )
    out = frame.to_pandas()
    assert isinstance(out, pd.DataFrame) and "UTC" in str(out["t"].dtype)
    assert out["n"].tolist() == [1, 2]


def _types(s):
    return (DType(s._native.type), s._native.time_unit, s._native.timezone)


def test_from_list_temporal_and_decimal():
    zoneinfo = pytest.importorskip("zoneinfo")
    naive = dftu.Series.from_list([dt.datetime(2020, 1, 2, 3, 4, 5, 6), None])
    assert _types(naive) == (DType.TIMESTAMP, 2, "")
    assert naive.to_list() == [dt.datetime(2020, 1, 2, 3, 4, 5, 6), None]

    paris = zoneinfo.ZoneInfo("Europe/Paris")
    aware = dftu.Series.from_list([dt.datetime(2020, 6, 1, 12, tzinfo=paris), None])
    assert _types(aware) == (DType.TIMESTAMP, 2, "Europe/Paris")
    assert aware.to_list()[0] == dt.datetime(2020, 6, 1, 12, tzinfo=paris)
    utc = dftu.Series.from_list([dt.datetime(2020, 6, 1, tzinfo=dt.timezone.utc)])
    assert _types(utc)[2] == "UTC"
    fixed = dftu.Series.from_list(
        [dt.datetime(2020, 6, 1, tzinfo=dt.timezone(dt.timedelta(hours=5, minutes=30)))]
    )
    assert _types(fixed)[2] == "+05:30"
    with pytest.raises(ValueError, match="naive"):
        dftu.Series.from_list([dt.datetime(2020, 1, 1), dt.datetime(2020, 1, 1, tzinfo=paris)])

    dates = dftu.Series.from_list([dt.date(1969, 7, 20), None, dt.date(2023, 12, 8)])
    assert DType(dates._native.type) == DType.DATE32
    assert dates.to_list() == [dt.date(1969, 7, 20), None, dt.date(2023, 12, 8)]
    times = dftu.Series.from_list([dt.time(1, 2, 3, 4), None])
    assert DType(times._native.type) == DType.TIME64
    assert times.to_list() == [dt.time(1, 2, 3, 4), None]
    deltas = dftu.Series.from_list([dt.timedelta(days=-2, microseconds=5), None])
    assert DType(deltas._native.type) == DType.DURATION
    assert deltas.to_list() == [dt.timedelta(days=-2, microseconds=5), None]

    decs = dftu.Series.from_list([D("1.5"), None, D("-20.125"), 3])
    assert DType(decs._native.type) == DType.DECIMAL128
    assert decs.to_list() == [D("1.500"), None, D("-20.125"), D("3.000")]
    assert dftu.Series.from_list([D("1E+3")]).to_list() == [D("1000")]
    with pytest.raises(ValueError, match="38 digits"):
        dftu.Series.from_list([D("1" + "0" * 40)])
    with pytest.raises(ValueError, match="different types"):
        dftu.Series.from_list([D("1.5"), 2.5])
    with pytest.raises(ValueError, match="different types"):
        dftu.Series.from_list([dt.date(2020, 1, 1), "x"])


def test_from_list_with_a_native_dtype():
    nulls = dftu.Series.from_list([None, None], dtype=DType.STRING)
    assert DType(nulls._native.type) == DType.STRING and nulls.to_list() == [None, None]
    empty = dftu.Series.from_list([], dtype=DType.INT64)
    assert DType(empty._native.type) == DType.INT64 and len(empty) == 0
    stamps = dftu.Series.from_list([None], dtype=DType.TIMESTAMP)
    assert DType(stamps._native.type) == DType.TIMESTAMP
    as_float = dftu.Series.from_list([1, 2], dtype=DType.FLOAT64)
    assert as_float.to_list() == [1.0, 2.0]


def test_from_list_equals_pyarrow():
    pa = _pa()
    values = [
        [dt.datetime(2020, 1, 2, 3), None],
        [dt.date(2020, 1, 2), None],
        [dt.timedelta(seconds=90), None],
        [D("12.5"), D("-0.25")],
    ]
    for v in values:
        assert dftu.Series.from_list(v).to_list() == pa.array(v).to_pylist()


def test_from_numpy_temporal_text_and_masks():
    ns = dftu.Series.from_numpy(np.array(["2020-01-02T03:04:05.123456789", "NaT"], dtype="M8[ns]"))
    assert _types(ns) == (DType.TIMESTAMP, 3, "")
    assert ns.to_numpy().tolist()[1] is None or np.isnat(ns.to_numpy()[1])
    assert ns[0] == dt.datetime(2020, 1, 2, 3, 4, 5, 123456) and ns[1] is None
    days = dftu.Series.from_numpy(np.array(["2020-01-02", "NaT"], dtype="M8[D]"))
    assert DType(days._native.type) == DType.DATE32 and days.to_list() == [
        dt.date(2020, 1, 2),
        None,
    ]
    weeks = dftu.Series.from_numpy(np.array(["2020-01"], dtype="M8[M]"))
    assert weeks.to_list() == [dt.date(2020, 1, 1)]
    spans = dftu.Series.from_numpy(np.array([5, "NaT"], dtype="m8[ms]"))
    assert _types(spans) == (DType.DURATION, 1, "")
    assert spans.to_list() == [dt.timedelta(milliseconds=5), None]
    hours = dftu.Series.from_numpy(np.array([2], dtype="m8[h]"))
    assert hours.to_list() == [dt.timedelta(hours=2)]
    with pytest.raises(ValueError, match="no fixed length"):
        dftu.Series.from_numpy(np.array([1], dtype="m8[M]"))

    assert (
        dftu.Series.from_numpy(np.array([1.5, 2.0], dtype=np.float16)).to_numpy().dtype
        == np.float16
    )
    flags = dftu.Series.from_numpy(np.array([True, False, True]))
    assert DType(flags._native.type) == DType.BOOL and flags.to_list() == [True, False, True]
    assert dftu.Series.from_numpy(np.array(["a", "bc"])).to_list() == ["a", "bc"]
    assert dftu.Series.from_numpy(np.array([b"a", b"bc"])).to_list() == [b"a", b"bc"]
    obj = np.array(["x", None, "y"], dtype=object)
    assert dftu.Series.from_numpy(obj).to_list() == ["x", None, "y"]
    strided = np.arange(10, dtype=np.int64)[::2]
    assert dftu.Series.from_numpy(strided).to_list() == [0, 2, 4, 6, 8]
    masked = np.ma.array([1, 2, 3], mask=[False, True, False])
    assert dftu.Series.from_numpy(masked).to_list() == [1, None, 3]
    big = np.array([2**63, 1], dtype=np.uint64)
    assert dftu.Series.from_numpy(big).to_list() == [2**63, 1]


def _same_as_pyarrow(series, values, arrow_type=None):
    pa = _pa()
    assert series.to_list() == pa.array(values, type=arrow_type).to_pylist()


def test_from_pandas_without_pyarrow():
    pd = pytest.importorskip("pandas")
    frame = pd.DataFrame(
        {
            "i": np.array([1, 2, 3], dtype=np.int64),
            "f": [1.5, np.nan, 3.0],
            "n": pd.array([1, None, 3], dtype="Int64"),
            "b": pd.array([True, None, False], dtype="boolean"),
            "s": pd.array(["a", None, "c"], dtype="string"),
            "o": ["x", None, "z"],
            "t": pd.to_datetime(["2020-01-01", None, "2020-01-03"]),
            "z": pd.to_datetime(["2020-01-01", None, "2020-01-03"]).tz_localize("Europe/Paris"),
            "d": pd.to_timedelta([1, None, 3], unit="s"),
            "c": pd.Categorical(["u", "v", None]),
        }
    )
    out = dftu.DataFrame.from_pandas(frame)
    assert out.columns == list(frame.columns)
    assert out["i"].to_list() == [1, 2, 3]
    assert out["f"].to_list() == [1.5, None, 3.0]
    assert out["n"].to_list() == [1, None, 3]
    assert out["b"].to_list() == [True, None, False]
    assert out["s"].to_list() == ["a", None, "c"]
    assert out["o"].to_list() == ["x", None, "z"]
    assert out["t"].to_list()[1] is None and out["t"].to_list()[0] == dt.datetime(2020, 1, 1)
    assert _types(out["z"])[2] == "Europe/Paris"
    if sys.version_info >= (3, 9):  # a zoned value needs zoneinfo
        assert out["z"].to_list()[0].hour == 0 and out["z"].to_list()[1] is None
    assert out["d"].to_list() == [dt.timedelta(seconds=1), None, dt.timedelta(seconds=3)]
    assert out["c"].to_list() == ["u", "v", None]


def test_from_pandas_index_becomes_columns():
    pd = pytest.importorskip("pandas")
    frame = pd.DataFrame({"a": [1, 2]}, index=pd.Index(["x", "y"], name="key"))
    out = dftu.DataFrame.from_pandas(frame)
    assert out.columns == ["a", "key"] and out["key"].to_list() == ["x", "y"]
    assert dftu.DataFrame.from_pandas(pd.DataFrame({"a": [1]})).columns == ["a"]
    multi = pd.DataFrame({"a": [1]}, index=pd.MultiIndex.from_tuples([(1, "p")]))
    assert dftu.DataFrame.from_pandas(multi).columns == [
        "a",
        "__index_level_0__",
        "__index_level_1__",
    ]


def test_from_pandas_equals_pyarrow():
    pa = _pa()
    pd = pytest.importorskip("pandas")
    frame = pd.DataFrame(
        {
            "f": [1.5, np.nan],
            "n": pd.array([1, None], dtype="Int64"),
            "t": pd.to_datetime(["2020-01-01", None]).tz_localize("UTC"),
            "o": ["a", None],
        }
    )
    ours = dftu.DataFrame.from_pandas(frame)
    theirs = dftu.DataFrame.from_arrow(pa.Table.from_pandas(frame))
    for name in frame.columns:
        assert ours[name].to_list() == theirs[name].to_list(), name


def test_from_polars_without_pyarrow():
    pl = pytest.importorskip("polars")
    frame = pl.DataFrame(
        {
            "i": [1, None, 3],
            "f": [1.5, None, 3.0],
            "b": [True, None, False],
            "s": ["a", None, "c"],
            "d": [dt.date(2020, 1, 2), None, dt.date(2021, 2, 3)],
            "t": pl.Series([dt.datetime(2020, 1, 2, 3), None, dt.datetime(2020, 1, 3)]),
            "u": pl.Series([dt.timedelta(seconds=1), None, dt.timedelta(seconds=2)]),
            "e": pl.Series([D("1.5"), None, D("2.25")]),
            "c": pl.Series(["u", "v", None], dtype=pl.Categorical),
        }
    )
    out = dftu.DataFrame.from_polars(frame)
    assert out["i"].to_list() == [1, None, 3]
    assert out["f"].to_list() == [1.5, None, 3.0]
    assert out["b"].to_list() == [True, None, False]
    assert out["s"].to_list() == ["a", None, "c"]
    assert out["d"].to_list() == [dt.date(2020, 1, 2), None, dt.date(2021, 2, 3)]
    assert out["t"].to_list() == [dt.datetime(2020, 1, 2, 3), None, dt.datetime(2020, 1, 3)]
    assert out["u"].to_list() == [dt.timedelta(seconds=1), None, dt.timedelta(seconds=2)]
    assert out["e"].to_list() == [D("1.50"), None, D("2.25")]
    assert out["c"].to_list() == ["u", "v", None]
    zoned = pl.Series([dt.datetime(2020, 1, 2, 3)]).dt.replace_time_zone("Europe/Paris")
    z = dftu.Series.from_polars(zoned)
    assert _types(z)[2] == "Europe/Paris" and z.to_list()[0].hour == 3
    nested = dftu.Series.from_polars(pl.Series([[1, 2], [3], None]))
    assert nested.to_list() == [[1, 2], [3], None]
    struct = dftu.Series.from_polars(pl.Series([{"a": 1, "b": "x"}, {"a": 2, "b": None}]))
    assert struct.to_list() == [{"a": 1, "b": "x"}, {"a": 2, "b": None}]


def test_from_list_nested():
    lists = dftu.Series.from_list([[1, 2], None, [], (3,)])
    assert DType(lists._native.type) == DType.LIST
    assert lists.to_list() == [[1, 2], None, [], [3]]
    deep = dftu.Series.from_list([[[1], [2, 3]], [[]]])
    assert deep.to_list() == [[[1], [2, 3]], [[]]]
    recs = dftu.Series.from_list([{"a": 1, "b": "x"}, {"a": 2}, None, {"c": [dt.date(2020, 1, 2)]}])
    assert DType(recs._native.type) == DType.STRUCT
    assert recs.to_list() == [
        {"a": 1, "b": "x", "c": None},
        {"a": 2, "b": None, "c": None},
        None,
        {"a": None, "b": None, "c": [dt.date(2020, 1, 2)]},
    ]
    with pytest.raises(ValueError, match="different types"):
        dftu.Series.from_list([[1], 2])
    with pytest.raises(ValueError, match="str"):
        dftu.Series.from_list([{1: 2}])
    frame = dftu.DataFrame.from_dict({"t": [["a"], ["b", "c"]]})
    assert frame["t"].to_list() == [["a"], ["b", "c"]]


def _mixed_frame():
    return dftu.DataFrame(
        {
            "i": dftu.Series.from_list([1, None, 3]),
            "s": dftu.Series.from_list(["a", None, "c"]),
            "t": dftu.Series.from_list([dt.datetime(2020, 1, 1), None, dt.datetime(2021, 1, 1)]),
            "d": dftu.Series.from_list([D("1.25"), None, D("-3.5")]),
            "p": dftu.Series.from_list([dt.date(2020, 1, 2), None, dt.date(2020, 1, 3)]),
        }
    )


def test_pickle_round_trip():
    frame = _mixed_frame()
    back = pickle.loads(pickle.dumps(frame))
    assert back.columns == frame.columns
    for name in frame.columns:
        assert back[name].to_list() == frame[name].to_list(), name
        assert DType(back[name]._native.type) == DType(frame[name]._native.type)
    col = pickle.loads(pickle.dumps(frame["t"]))
    assert col.to_list() == frame["t"].to_list() and _types(col) == _types(frame["t"])
    with pytest.raises(ValueError):
        dftu.Series(_ext._series_from_bytes(b"not a series"))


def test_wrappers_run_without_arrow():
    frame = _mixed_frame()
    assert frame.memory_usage().to_dict()["i"][0] > 0
    words = dftu.Series.from_list(["ab", "cd", None])
    assert words.str.get(0).to_list() == [97, 99, None] or words.str.get(0).to_list()[2] is None
    days = dftu.Series.from_list([dt.datetime(2021, 1, 4), dt.datetime(2020, 12, 31)])
    iso = days.dt.isocalendar().to_dict()
    assert iso["year"] == [2021, 2020] and iso["week"] == [1, 53] and iso["day"] == [1, 4]
    diff = dftu.Series.from_list([1, 2, 3]).compare(dftu.Series.from_list([1, 5, 3])).to_dict()
    assert diff == {"index": [1], "self": [2], "other": [5]}
    same = dftu.Series.from_list([1, 2]).compare(dftu.Series.from_list([1, 2])).to_dict()
    assert same == {"index": [], "self": [], "other": []}
    grid = dftu.DataFrame.from_dict({"t": [1, 4], "v": [1.0, 2.0]}).asfreq(1, on="t")
    assert grid["t"].to_list() == [1, 2, 3, 4]
    dummies = dftu.Series.from_list(["a|b", "b", None]).str.get_dummies().to_dict()
    assert dummies["b"] == [1, 1, 0]
