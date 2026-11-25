# SPDX-License-Identifier: MIT
"""Python values build columns of the exact type they meet: from_list checks
integer ranges, loc and reindex build keys and fills in a column's full type
(unit, scale, date64, nested), and the native helpers under them keep types,
nulls and chunks."""

import datetime as dt
import decimal

import pytest

np = pytest.importorskip("numpy")
pd = pytest.importorskip("pandas")
pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame, Series, TraceWriteOptions  # noqa: E402
from dftracer.utils._table import _restore_category  # noqa: E402
from dftracer.utils.trace import TraceViewer  # noqa: E402

D = decimal.Decimal


def _labels(categorical):
    return [None if pd.isna(v) else v for v in categorical]


@pytest.mark.parametrize(
    "values, dtype",
    [
        ([300], pa.int8()),
        ([300], "int8"),
        ([-1], "uint8"),
        ([2**40], "int32"),
        ([2**63], "int64"),
        ([1.5], "int64"),
        ([float(2**63)], "int64"),
        ([float(2**64)], "uint64"),
        ([128.0], "int8"),
    ],
)
def test_from_list_refuses_a_value_the_integer_type_cannot_hold(values, dtype):
    with pytest.raises(OverflowError):
        Series.from_list(values, dtype)


def test_from_list_takes_any_dtype_spelling_cast_takes():
    assert Series.from_list([1.0, None], "int8").to_list() == [1, None]
    assert Series.from_list([2**63], "uint64").to_list() == [2**63]
    assert Series.from_list([1], int).dtype.name == "INT64"
    with pytest.raises(ValueError):
        Series.from_list([1], "nope")
    assert Series(Series.from_list([1, 2]), dtype="float64").to_list() == [1.0, 2.0]
    assert pa.array(Series(Series.from_list([1, 2]), dtype=pa.float32())).type == pa.float32()


def test_from_list_reads_numpy_values_at_any_depth():
    assert Series.from_list([np.array([1, 2]), None]).to_list() == [[1, 2], None]
    assert Series.from_list([[np.int64(1)]]).to_list() == [[1]]
    assert Series.from_list([{"a": np.float32(1.5)}]).to_list() == [{"a": 1.5}]
    nanos = Series.from_list([np.datetime64("2020-01-01T00:00:00.000000001"), None])
    assert pa.array(nanos).type == pa.timestamp("ns")
    assert nanos.to_numpy()[0] == np.datetime64("2020-01-01T00:00:00.000000001")


TYPED = {
    "d": pa.array([D("1.50"), D("2.25")], pa.decimal128(10, 2)),
    "t": pa.array([dt.time(1), dt.time(2)], pa.time32("s")),
    "e": pa.array([dt.date(2020, 1, 1), dt.date(2020, 1, 2)], pa.date64()),
    "v": pa.array([1, 2]),
    "f": pa.array([[1, 2], [3, 4]], pa.list_(pa.int64(), 2)),
    "m": pa.array([[("a", 1)], [("b", 2)]], pa.map_(pa.string(), pa.int64())),
    "s": pa.array([{"x": 1}, {"x": 2}]),
}


def test_a_masked_loc_write_keeps_each_column_type():
    table = pa.table(TYPED)
    df = DataFrame.from_arrow(table)
    df.loc[df["v"] == 1, "d"] = D("9")
    df.loc[df["v"] == 1, "t"] = dt.time(5)
    df.loc[df["v"] == 1, "e"] = dt.date(2000, 1, 1)
    for name in "fms":
        df.loc[df["v"] == 1, name] = None
    out = df.to_arrow()
    assert out.schema == table.schema
    assert out.to_pylist()[0] == {
        "d": D("9.00"),
        "t": dt.time(5),
        "e": dt.date(2000, 1, 1),
        "v": 1,
        "f": None,
        "m": None,
        "s": None,
    }
    assert out.to_pylist()[1] == table.to_pylist()[1]


def test_a_loc_write_that_does_not_fit_the_scale_fails():
    df = DataFrame.from_arrow(pa.table({"d": TYPED["d"], "v": TYPED["v"]}))
    with pytest.raises((TypeError, ValueError)):
        df.loc[df["v"] == 1, "d"] = D("1.234")


@pytest.mark.parametrize(
    "name, key", [("d", D("2.25")), ("t", dt.time(2)), ("e", dt.date(2020, 1, 2))]
)
def test_loc_finds_a_label_of_a_typed_level(name, key):
    df = DataFrame.from_arrow(pa.table({name: TYPED[name], "v": TYPED["v"]})).set_index(name)
    assert df.loc[key]["v"] == 2


def test_reindex_on_date64_and_decimal_labels():
    days = DataFrame.from_arrow(
        pa.table(
            {"k": pa.array([dt.date(2020, 1, 1), dt.date(2020, 1, 3)], pa.date64()), "v": [1, 2]}
        )
    ).set_index("k")
    near = days.reindex([dt.date(2020, 1, 2)], method="ffill", tolerance=pd.Timedelta("1D"))
    assert near.to_pandas()["v"].tolist() == [1]
    far = days.reindex([dt.date(2020, 1, 2)], method="ffill", tolerance=pd.Timedelta("1h"))
    assert far.to_pandas()["v"].isna().tolist() == [True]
    money = DataFrame.from_arrow(pa.table({"k": TYPED["d"], "v": [1, 2]})).set_index("k")
    assert money.reindex([D("2.25"), D("3")]).to_pandas()["v"].isna().tolist() == [False, True]


def test_an_arrow_null_column_imports_as_string_nulls():
    col = Series.from_arrow(pa.nulls(3))
    assert col.dtype.name == "STRING" and col.to_list() == [None] * 3
    frame = DataFrame.from_arrow(pa.table({"n": pa.nulls(2), "v": [1, 2]}))
    assert frame["n"].to_list() == [None, None]


def test_nulls_like_keeps_a_nested_type():
    col = Series.from_arrow(TYPED["f"])
    nulls = col._nulls_like(3)
    assert pa.array(nulls).type == TYPED["f"].type
    assert nulls.to_list() == [None] * 3


def test_plain_text_flattens_each_view_chunk():
    chunks = pa.chunked_array(
        [pa.array(["a", None], pa.string_view()), pa.array(["c"], pa.string_view())]
    )
    plain = Series.from_arrow(chunks)._plain_text()
    assert pa.chunked_array(plain).type == pa.string()
    assert plain.to_list() == ["a", None, "c"]


def test_a_category_with_unspecified_codes_under_its_nulls_restores():
    # the code under the null row is 99, past the dictionary
    codes = pa.Array.from_buffers(
        pa.int32(),
        3,
        [
            pa.py_buffer(np.packbits([1, 0, 1], bitorder="little")),
            pa.py_buffer(np.array([0, 99, 1], np.int32)),
        ],
    )
    native = Series.from_arrow(pa.DictionaryArray.from_arrays(codes, pa.array(["x", "y"])))
    got = _restore_category("c", native, {"categories": ["y", "x"], "categories_dtype": "object"})
    assert _labels(got) == ["x", None, "y"]


def test_a_category_over_a_repeated_and_null_dictionary_restores():
    indices = pa.array([0, 1, 2, None], pa.int32())
    native = Series.from_arrow(pa.DictionaryArray.from_arrays(indices, pa.array(["x", None, "x"])))
    got = _restore_category("c", native, {"categories": ["x"], "categories_dtype": "object"})
    assert _labels(got) == ["x", None, "x", None]


def test_a_table_read_back_gives_writable_numpy_columns(tmp_path):
    path = str(tmp_path / "t")
    DataFrame.from_pandas(pd.DataFrame({"i": np.arange(3), "f": [0.5, 1.5, 2.5]})).write_table(
        path, name="t", mode="overwrite", options=TraceWriteOptions(build_index=False)
    )
    back = TraceViewer(path).collect().to_pandas()
    back.loc[0, "i"] = 9
    assert back["i"].tolist() == [9, 1, 2]


def test_a_pyarrow_where_keeps_the_schema_metadata():
    from dftracer.utils._expr import col, where

    table = pa.table({"a": [1, 2, 3]}).replace_schema_metadata({"k": "v"})
    kept = where(table, col("a") > 1)
    assert kept.schema.metadata == {b"k": b"v"}
    assert kept.column("a").to_pylist() == [2, 3]


def test_from_dict_takes_a_range_and_from_arrow_takes_a_bare_stream():
    assert DataFrame.from_dict({"a": range(3)})["a"].to_list() == [0, 1, 2]

    class Stream:
        def __init__(self, table):
            self._table = table

        def __arrow_c_stream__(self, requested_schema=None):
            return self._table.__arrow_c_stream__(requested_schema)

    frame = DataFrame.from_arrow(Stream(pa.table({"a": [1, 2]})))
    assert frame["a"].to_list() == [1, 2]


def test_a_chunked_dictionary_stays_chunked_and_converts_to_one_category():
    s = Series(pa.array(["x", None, "y", "x", "z", "x"]).dictionary_encode())
    chunked = DataFrame({"s": s}).lazy().collect(batch_size=2)["s"]
    assert chunked.encoding == 5
    for got in (chunked.to_pandas(), chunked.to_pandas(nullable=True)):
        assert got.dtype == "category"
        assert _labels(got) == ["x", None, "y", "x", "z", "x"]
    assert DataFrame({"s": chunked}).to_pandas()["s"].dtype == "category"


def test_a_loc_write_keeps_a_negative_decimal_scale_and_wide_values():
    df = DataFrame.from_arrow(pa.table({"x": pa.array([D("1E2")], pa.decimal128(38, -2))}))
    df.loc[0, "x"] = D("3.4E3")
    assert pa.table(df).column("x").to_pylist() == [D("3.4E3")]
    wide = D("1" * 35 + ".25")
    df = DataFrame.from_arrow(pa.table({"x": pa.array([D("0.00")], pa.decimal128(38, 2))}))
    df.loc[0, "x"] = wide
    assert pa.table(df).column("x").to_pylist() == [wide]


def _dictionary(codes, values):
    return pa.DictionaryArray.from_arrays(
        pa.array(codes, pa.int32()), pa.array(values, pa.string())
    )


def test_a_joined_dictionary_keeps_the_nulls_of_an_empty_dictionary_chunk():
    table = pa.concat_tables(
        [
            pa.table({"x": _dictionary([0, 1], ["a", "b"])}),
            pa.table({"x": _dictionary([None, None], [])}),
        ]
    )
    s = DataFrame.from_arrow(table)["x"]
    assert s._joined_dictionary().to_list() == ["a", "b", None, None]
    assert _labels(s.to_pandas()) == ["a", "b", None, None]


def test_a_nested_dictionary_join_keeps_null_entries_and_skips_codes_under_nulls():
    validity = pa.py_buffer(bytes([0b01]))
    codes = pa.array([0, -100000000], pa.int32()).buffers()[1]
    under_null = pa.DictionaryArray.from_arrays(
        pa.Array.from_buffers(pa.int32(), 2, [validity, codes], null_count=1),
        pa.array(["a"], pa.string()),
    )
    null_entry = _dictionary([0, 1], ["a", None])
    tail = _dictionary([0], ["b"])
    for head in (under_null, null_entry):
        chunks = [pa.StructArray.from_arrays([d], ["k"]) for d in (head, tail)]
        joined = Series.from_arrow(pa.chunked_array(chunks)).materialize().to_list()
        assert joined == [{"k": "a"}, {"k": None}, {"k": "b"}]
