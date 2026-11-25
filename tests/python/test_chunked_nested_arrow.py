# SPDX-License-Identifier: MIT
"""Columns of several Arrow batches export under a schema that describes them,
join binary, dictionary and nested children, and give null rows of their own
nested type."""

import pytest

pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame, Series  # noqa: E402

LONG_A = "hello world long str"
LONG_B = "another rather long string"
SELECTION = 3
CHUNKED = 5


def _two(first, second, type_):
    return pa.chunked_array([pa.array(first, type=type_), pa.array(second, type=type_)])


def test_a_list_of_string_views_in_two_batches_keeps_its_text():
    t = pa.list_(pa.string_view())
    col = _two([[LONG_A], None], [["x"]], t)
    out = Series.from_arrow(col).to_arrow()
    assert out.to_pylist() == [[LONG_A], None, ["x"]]


def test_a_struct_of_string_views_in_two_batches_keeps_its_text():
    t = pa.struct([("a", pa.string_view())])
    col = _two([{"a": LONG_A}, None], [{"a": LONG_B}], t)
    out = Series.from_arrow(col).to_arrow()
    assert out.to_pylist() == [{"a": LONG_A}, None, {"a": LONG_B}]


def test_a_table_of_two_batches_with_string_views_round_trips():
    st = pa.struct([("a", pa.string_view())])
    first = pa.table(
        {
            "s": pa.array([LONG_A, "short"], pa.string_view()),
            "st": pa.array([{"a": LONG_A}, None], st),
        }
    )
    second = pa.table(
        {
            "s": pa.array([LONG_B], pa.string_view()),
            "st": pa.array([{"a": LONG_B}], st),
        }
    )
    table = pa.concat_tables([first, second])
    assert table.column("s").num_chunks == 2
    out = pa.table(DataFrame.from_arrow(table))
    assert out.column("s").to_pylist() == [LONG_A, "short", LONG_B]
    assert out.column("st").to_pylist() == [{"a": LONG_A}, None, {"a": LONG_B}]


@pytest.mark.parametrize(
    "type_, first, second",
    [
        (pa.list_(pa.binary()), [[b"ab", None], None], [[b"cd"]]),
        (pa.list_(pa.large_binary()), [[b"ab"], None], [[b"cd", b""]]),
        (pa.list_(pa.binary(4)), [[b"abcd", None], None], [[b"wxyz"]]),
        (pa.binary(4), [b"abcd", None], [b"wxyz"]),
    ],
    ids=["list_binary", "list_large_binary", "list_fixed_size_binary", "fixed_size_binary"],
)
def test_binary_children_of_two_batches_join(type_, first, second):
    s = Series.from_arrow(_two(first, second, type_))
    want = first + second
    assert s.encoding == CHUNKED
    out = s.to_arrow()
    assert out.type == type_
    assert out.to_pylist() == want
    assert s.to_list() == want


def test_a_null_column_like_a_filtered_list_has_the_list_type():
    s = Series.from_arrow(pa.array([[1, 2], [3, 4], [5]]))
    picked = s.filter(Series([True, False, True]))
    assert picked._nulls_like(2).to_arrow().type == pa.list_(pa.int64())


def test_a_null_column_like_a_filtered_struct_has_the_struct_type():
    t = pa.struct([("a", pa.int64())])
    s = Series.from_arrow(pa.array([{"a": 1}, {"a": 2}, {"a": 3}], t))
    picked = s.filter(Series([True, False, True]))
    assert picked._nulls_like(2).to_arrow().type == t


def test_a_null_column_like_a_filtered_map_has_the_map_type():
    t = pa.map_(pa.string(), pa.int64())
    s = Series.from_arrow(pa.array([[("a", 1)], [("b", 2)], [("c", 3)]], t))
    picked = s.filter(Series([True, False, True]))
    assert picked._nulls_like(2).to_arrow().type == t


def test_a_null_column_like_a_fixed_size_binary_keeps_the_width():
    s = Series.from_arrow(pa.array([b"abcd", b"efgh"], pa.binary(4)))
    assert s._nulls_like(3).to_arrow().type == pa.binary(4)


def test_a_diagonal_concat_of_an_all_null_part_and_a_list_works():
    nulls = DataFrame({"x": [None, None]})
    lists = DataFrame({"x": [[1], [2]]})
    out = nulls.concat(lists, how="diagonal").to_arrow().column("x").to_pylist()
    assert out == [None, None, [1], [2]]
    back = lists.concat(nulls, how="diagonal").to_arrow().column("x").to_pylist()
    assert back == [[1], [2], None, None]


def test_a_diagonal_concat_of_a_nested_and_a_scalar_column_fails():
    big = pa.table({"x": pa.array([[1], [2]], pa.large_list(pa.int64()))})
    with pytest.raises(Exception, match="incompatible"):
        DataFrame.from_arrow(big).concat(DataFrame({"x": [1, 2]}), how="diagonal")


def test_filtered_struct_parts_of_a_small_batch_collect_are_flat_chunks():
    t = pa.struct([("a", pa.int64())])
    column = Series.from_arrow(pa.array([{"a": i} for i in range(6)], t))
    picked = column.filter(Series([True, True, True, False, True, True]))
    assert picked.encoding == SELECTION
    df = DataFrame({"s": picked})
    assert df["s"].encoding == SELECTION
    out = df.lazy().collect(batch_size=1)
    assert out["s"].encoding == CHUNKED
    assert pa.table(out).column("s").to_pylist() == [{"a": i} for i in range(6) if i != 3]


DICT = pa.dictionary(pa.int32(), pa.string())


def _dictionary_children():
    return [
        pa.list_(DICT),
        pa.large_list(DICT),
        pa.list_(DICT, 2),
        pa.map_(pa.string(), DICT),
    ]


def _rows(type_, first):
    if pa.types.is_fixed_size_list(type_):
        return [["p", None]] if first else [["q", "q"]]
    if pa.types.is_map(type_):
        return [[("k", "p"), ("l", None)]] if first else [[("k", "q"), ("l", "q")]]
    return [["p", None]] if first else [["q", "q"]]


@pytest.mark.parametrize("type_", _dictionary_children(), ids=str)
def test_a_nested_dictionary_child_exports_a_schema_that_matches(type_):
    one = pa.chunked_array([pa.array(_rows(type_, True), type_)])
    out = Series.from_arrow(one).to_arrow()
    out.validate(full=True)
    assert out.to_pylist() == _rows(type_, True)
    two = pa.chunked_array(
        [pa.array(_rows(type_, True), type_), pa.array(_rows(type_, False), type_)]
    )
    out = Series.from_arrow(two).to_arrow()
    out.validate(full=True)
    assert out.to_pylist() == _rows(type_, True) + _rows(type_, False)


def test_struct_dictionary_children_of_two_batches_keep_their_text_type():
    t = pa.struct([("a", DICT)])
    col = _two([{"a": "p"}, None], [{"a": "q"}], t)
    out = Series.from_arrow(col).to_arrow()
    out.validate(full=True)
    assert out.to_pylist() == [{"a": "p"}, None, {"a": "q"}]
    assert out.type.field("a").type.value_type == pa.string()


def test_a_chunked_dictionary_exports_as_one_dictionary():
    col = _two(["x", "y", None], ["z", "x"], DICT)
    out = Series.from_arrow(col).to_arrow()
    out.validate(full=True)
    assert pa.types.is_dictionary(out.type)
    assert out.to_pylist() == ["x", "y", None, "z", "x"]


def _lazy_concat(first, second):
    return first.lazy().concat(second.lazy())


def test_a_lazy_concat_of_lists_with_two_child_layouts_streams_one_schema():
    rows = 65536
    views = DataFrame.from_arrow(
        pa.table({"l": pa.array([[LONG_A]] * rows, pa.list_(pa.string_view()))})
    )
    plain = DataFrame.from_arrow(pa.table({"l": pa.array([["x"]] * 10, pa.list_(pa.string()))}))
    for first, second in ((views, plain), (plain, views)):
        table = pa.table(_lazy_concat(first, second))
        table.validate(full=True)
        assert table.num_rows == rows + 10
        values = table.column("l").to_pylist()
        assert sorted({v[0] for v in values}) == sorted({LONG_A, "x"})
        assert values[-1] == ([LONG_A] if first is plain else ["x"])


def test_a_lazy_concat_of_structs_with_two_child_layouts_streams_one_schema():
    views = DataFrame.from_arrow(
        pa.table({"s": pa.array([{"a": LONG_A}, {"a": "x"}], pa.struct([("a", pa.string_view())]))})
    )
    plain = DataFrame.from_arrow(
        pa.table({"s": pa.array([{"a": LONG_B}, {"a": "y"}], pa.struct([("a", pa.string())]))})
    )
    table = pa.table(_lazy_concat(views, plain))
    table.validate(full=True)
    assert [r["a"] for r in table.column("s").to_pylist()] == [LONG_A, "x", LONG_B, "y"]


def _nested_frame(shape, values, type_):
    if shape == "struct":
        column = pa.array([{"a": v} for v in values], pa.struct([("a", type_)]))
    else:
        column = pa.array([[v] for v in values], pa.list_(type_))
    return DataFrame.from_arrow(pa.table({"c": column}))


@pytest.mark.parametrize("shape", ["list", "struct"])
def test_a_collected_concat_of_dictionary_and_plain_children_exports(shape):
    rows = 65536
    dictionary = _nested_frame(shape, ["p"] * rows, DICT)
    plain = _nested_frame(shape, ["q", "r"], pa.string())
    for first, second in ((dictionary, plain), (plain, dictionary)):
        collected = _lazy_concat(first, second).collect()
        table = pa.table(collected.to_arrow())
        table.validate(full=True)
        assert table.num_rows == rows + 2
        texts = [v["a"] if shape == "struct" else v[0] for v in table.column("c").to_pylist()]
        assert texts == (
            ["p"] * rows + ["q", "r"] if first is dictionary else ["q", "r"] + ["p"] * rows
        )


def test_a_lazy_concat_of_dictionary_and_plain_text_streams_one_schema():
    rows = 65536
    dictionary = DataFrame.from_arrow(pa.table({"s": pa.array(["p"] * rows, DICT)}))
    plain = DataFrame.from_arrow(pa.table({"s": pa.array(["q", "r"], pa.string())}))
    for first, second in ((dictionary, plain), (plain, dictionary)):
        table = pa.table(_lazy_concat(first, second))
        table.validate(full=True)
        got = table.column("s").to_pylist()
        assert got == (
            ["p"] * rows + ["q", "r"] if first is dictionary else ["q", "r"] + ["p"] * rows
        )


def test_chunks_that_are_slices_of_one_dictionary_array_export_it():
    values = pa.array([LONG_A, "y", LONG_B, "y"]).dictionary_encode()
    col = pa.chunked_array([values.slice(0, 1), values.slice(1, 2), values.slice(3)])
    out = Series.from_arrow(col).to_arrow()
    out.validate(full=True)
    assert pa.types.is_dictionary(out.type)
    assert out.to_pylist() == [LONG_A, "y", LONG_B, "y"]
    nested = pa.chunked_array(
        [
            pa.array([[LONG_A], None, [LONG_B]], pa.list_(DICT)).slice(0, 2),
            pa.array([[LONG_B]], pa.list_(DICT)),
        ]
    )
    got = Series.from_arrow(nested).to_arrow()
    got.validate(full=True)
    assert got.to_pylist() == [[LONG_A], None, [LONG_B]]


def test_a_diagonal_concat_of_binary_and_large_binary_keeps_the_bytes():
    narrow = DataFrame.from_arrow(pa.table({"x": pa.array([b"ab", b"cd"], pa.binary())}))
    wide = DataFrame.from_arrow(pa.table({"x": pa.array([b"ef"], pa.large_binary())}))
    for first, second, want in (
        (narrow, wide, [b"ab", b"cd", b"ef"]),
        (wide, narrow, [b"ef", b"ab", b"cd"]),
    ):
        out = first.concat(second, how="diagonal").to_arrow().column("x")
        assert out.to_pylist() == want


def test_a_diagonal_concat_of_fixed_size_binary_and_int64_is_a_type_error():
    fixed = DataFrame.from_arrow(pa.table({"x": pa.array([b"abcd"], pa.binary(4))}))
    ints = DataFrame({"x": [1, 2]})
    for first, second in ((fixed, ints), (ints, fixed)):
        with pytest.raises(Exception, match="incompatible"):
            first.concat(second, how="diagonal")


def test_where_refuses_fixed_size_binary_of_different_widths():
    wide = Series.from_arrow(pa.array([b"abcd", b"efgh"], pa.binary(4)))
    narrow = Series.from_arrow(pa.array([b"ab", b"cd"], pa.binary(2)))
    with pytest.raises(Exception):
        wide.where(Series([True, False]), narrow)
