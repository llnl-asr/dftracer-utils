# SPDX-License-Identifier: MIT
"""A nested column of several Arrow batches stays chunked, and each op gives
on it what it gives on the same column in one piece."""

import pickle

import pytest

pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame, Series  # noqa: E402

ROWS = [
    ([1, 2], [1, 2], [1, 2], [("a", 1)], {"a": 1, "b": "x"}),
    (None, None, None, None, None),
    ([], [], [3, 4], [], {"a": 2, "b": None}),
    ([5], [5], [5, 6], [("b", 2), ("c", 3)], {"a": None, "b": "y"}),
]
TYPES = {
    "list": pa.list_(pa.int64()),
    "large_list": pa.large_list(pa.int64()),
    "fixed_size_list": pa.list_(pa.int64(), 2),
    "map": pa.map_(pa.string(), pa.int64()),
    "struct": pa.struct([("a", pa.int64()), ("b", pa.string())]),
}


def _batch(rows, start):
    cols = {"id": pa.array(range(start, start + len(rows)), pa.int64())}
    for i, (name, t) in enumerate(TYPES.items()):
        cols[name] = pa.array([r[i] for r in rows], t)
    return pa.table(cols)


TABLE = pa.concat_tables([_batch(ROWS, 0), _batch(ROWS[::-1], 4), _batch(ROWS[1:3], 8)])
N = TABLE.num_rows


def _arrow(x):
    arr = pa.chunked_array(x)
    t = arr.type
    if pa.types.is_dictionary(t):
        t = t.value_type  # a selection exports as a dictionary
    return arr.to_pylist(), t


OPS = {
    "to_arrow": lambda s: (s.to_arrow().to_pylist(), s.to_arrow().type),
    "item": lambda s: [s.item(i) for i in range(N)],
    "to_list": lambda s: s.to_list(),
    "take": lambda s: _arrow(s.take([9, 0, 5, 3])),
    "filter": lambda s: _arrow(s.filter(Series([i % 3 == 0 for i in range(N)]))),
    "where": lambda s: _arrow(s.where(Series([i % 2 == 0 for i in range(N)]), s._nulls_like(N))),
    "is_null": lambda s: s.is_null().to_list(),
    "pickle": lambda s: _arrow(pickle.loads(pickle.dumps(s))),
    "to_pandas": lambda s: s.to_pandas().tolist(),
}


@pytest.mark.parametrize("name", list(TYPES))
def test_a_nested_column_of_several_batches_stays_chunked(name):
    chunked = DataFrame.from_arrow(TABLE)[name]
    assert chunked.encoding == 5
    out = pa.table(DataFrame.from_arrow(TABLE)).column(name)
    assert out.type == TYPES[name]
    assert out.num_chunks == TABLE.column(name).num_chunks


@pytest.mark.parametrize("op", list(OPS))
@pytest.mark.parametrize("name", list(TYPES))
def test_an_op_on_chunks_matches_the_op_in_one_piece(name, op):
    chunked = DataFrame.from_arrow(TABLE)[name]
    flat = DataFrame.from_arrow(TABLE.combine_chunks())[name]
    assert flat.encoding == 0
    assert OPS[op](chunked) == OPS[op](flat)


@pytest.mark.parametrize("name", list(TYPES))
def test_frame_ops_on_chunked_nested_columns_match(name):
    chunked = DataFrame.from_arrow(TABLE).select(["id", name])
    flat = DataFrame.from_arrow(TABLE.combine_chunks()).select(["id", name])
    for op in (
        lambda df: df.sort_by("id", descending=True),
        lambda df: df.join(df.select(["id"]), on="id"),
        lambda df: df.filter(df["id"] % 2 == 0),
        lambda df: df.head(5),
    ):
        assert op(chunked).to_arrow().equals(op(flat).to_arrow())


@pytest.mark.parametrize("budget", [None, 1, 1 << 20])
def test_lazy_explode_of_a_chunked_list_matches_the_list_in_one_piece(budget):
    def run(table):
        lazy = DataFrame.from_arrow(table).select(["id", "list"]).lazy()
        if budget is not None:
            lazy = lazy.memory_budget(budget)
        return lazy.explode("list").collect().to_arrow()

    assert run(TABLE).equals(run(TABLE.combine_chunks()))


def test_head_by_a_chunked_struct_key_matches_the_key_in_one_piece():
    def run(table):
        out = DataFrame.from_arrow(table).select(["id", "struct"]).lazy().head_by(["struct"], 1)
        return sorted(out.collect()["id"].to_list())

    assert run(TABLE) == run(TABLE.combine_chunks())


@pytest.mark.parametrize("name", list(TYPES))
def test_children_of_a_chunked_or_selected_column_match_the_column_in_one_piece(name):
    flat = DataFrame.from_arrow(TABLE.combine_chunks())[name]
    picks = Series([i % 2 == 0 for i in range(N)])
    for other in (DataFrame.from_arrow(TABLE)[name], flat.filter(picks)):
        base = flat if other.length == N else flat.filter(picks).materialize()
        assert other.num_children() == base.num_children()
        for i in range(base.num_children()):
            assert other.child(i).to_list() == base.child(i).to_list()


def test_plain_text_reaches_view_strings_under_a_nested_column():
    t = pa.list_(pa.string_view())
    s = Series.from_arrow(
        pa.chunked_array([pa.array([["a long string value", None]], t), pa.array([["b"]], t)])
    )
    assert pa.chunked_array(s._plain_text()).type == pa.list_(pa.string())
    assert s.to_pandas(arrow=True).tolist() == [["a long string value", None], ["b"]]
