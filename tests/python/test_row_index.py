# SPDX-License-Identifier: MIT
"""The native row index: ``index_names`` on the frame, carried by the frame ops
and the lazy plan, crossing pandas and Arrow, checked against real pandas."""

import json

import pytest

pd = pytest.importorskip("pandas")
pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame, col  # noqa: E402


def _pdf():
    return pd.DataFrame(
        {
            "k": [1, 1, 2, 2, 3],
            "j": ["a", "b", "a", "b", "a"],
            "v": [5.0, 4.0, 3.0, 2.0, 1.0],
        }
    )


@pytest.mark.parametrize("levels", [[], ["k"], ["k", "j"]])
def test_set_index_matches_pandas_through_to_pandas(levels):
    pdf = _pdf()
    ours = DataFrame.from_pandas(pdf)
    ref = pdf
    if levels:
        ours = ours.set_index(levels)
        ref = pdf.set_index(levels)
    assert ours.index_names() == levels
    got = ours.to_pandas()
    pd.testing.assert_frame_equal(got, ref, check_dtype=False, check_index_type=False)
    assert list(got.index.names) == list(ref.index.names)


@pytest.mark.parametrize("levels", [[], ["k"], ["k", "j"]])
def test_from_pandas_records_a_non_range_index(levels):
    pdf = _pdf()
    if levels:
        pdf = pdf.set_index(levels)
    ours = DataFrame.from_pandas(pdf)
    assert ours.index_names() == levels
    pd.testing.assert_frame_equal(ours.to_pandas(), pdf, check_dtype=False, check_index_type=False)


def test_an_unnamed_index_round_trips_unnamed():
    pdf = _pdf().set_index(pd.Index([10, 20, 35, 40, 50]))
    ours = DataFrame.from_pandas(pdf)
    assert ours.index_names() == ["__index_level_0__"]
    got = ours.to_pandas()
    assert list(got.index.names) == [None]
    pd.testing.assert_frame_equal(got, pdf, check_dtype=False, check_index_type=False)

    multi = _pdf().set_index([pd.Index([1, 1, 2, 2, 3]), pd.Index(list("abcde"))])
    back = DataFrame.from_pandas(multi).to_pandas()
    assert list(back.index.names) == [None, None]
    pd.testing.assert_frame_equal(back, multi, check_dtype=False, check_index_type=False)


def test_nulls_and_duplicates_in_the_index_round_trip():
    pdf = pd.DataFrame(
        {"k": [1.0, None, 1.0, 2.0], "s": ["x", "y", None, "x"], "v": [1, 2, 3, 4]}
    ).set_index(["k", "s"])
    ours = DataFrame.from_pandas(pdf)
    assert ours.index_names() == ["k", "s"]
    got = ours.to_pandas()
    assert got.index.nlevels == 2
    assert got.index.get_level_values("k").isna().tolist() == [False, True, False, False]
    assert got.index.get_level_values("s").isna().tolist() == [False, False, True, False]
    assert got["v"].tolist() == [1, 2, 3, 4]
    dup = pd.DataFrame({"k": [1, 1, 1], "v": [1, 2, 3]}).set_index("k")
    assert DataFrame.from_pandas(dup).to_pandas().index.tolist() == [1, 1, 1]


def test_reset_index_matches_pandas():
    pdf = _pdf().set_index(["k", "j"])
    ours = DataFrame.from_pandas(pdf)
    back = ours.reset_index()
    assert back.index_names() == []
    assert back.columns == ["v", "k", "j"] or sorted(back.columns) == ["j", "k", "v"]
    assert ours.reset_index(drop=True).columns == ["v"]
    assert ours.reset_index(drop=True).index_names() == []


def test_set_index_refuses_what_pandas_refuses_or_the_engine_cannot_hash():
    df = DataFrame({"k": [1, 2], "v": [[1], [2]]})
    with pytest.raises(KeyError):
        df.set_index("missing")
    with pytest.raises(ValueError, match="v"):
        df.set_index("v")
    with pytest.raises(ValueError):
        df.set_index(["k", "k"])


def test_frame_ops_carry_the_index_and_the_lazy_plan_agrees():
    df = DataFrame.from_pandas(_pdf()).set_index(["k", "j"])
    lazy = df.lazy()
    assert lazy.index_names() == ["k", "j"]
    pairs = [
        (df.filter(df["v"] > 1), lazy.filter(col("v") > 1).collect()),
        (df.sort_by("v"), lazy.sort_by("v").collect()),
        (df.head(2), lazy.head(2).collect()),
        (df.tail(2), lazy.tail(2).collect()),
        (df.reverse(), lazy.reverse().collect()),
        (df.select("k", "v"), lazy.select(["k", "v"]).collect()),
    ]
    for eager, planned in pairs:
        assert eager.index_names() == planned.index_names()
    assert df.filter(df["v"] > 1).index_names() == ["k", "j"]
    assert df.select("k", "v").index_names() == ["k"]
    assert df.select("v").index_names() == []
    assert df.describe().index_names() == []


def test_arrow_carries_the_index_in_the_schema_metadata():
    df = DataFrame.from_pandas(_pdf()).set_index(["k", "j"])
    table = pa.table(df)
    meta = {k.decode(): v.decode() for k, v in (table.schema.metadata or {}).items()}
    assert json.loads(meta["dftracer.utils.index"]) == ["k", "j"]
    assert DataFrame.from_arrow(table).index_names() == ["k", "j"]
    assert DataFrame.from_pandas(_pdf()).to_arrow().schema.metadata is None


def test_arrow_with_pandas_metadata_maps_its_index_columns():
    pdf = _pdf().set_index(["k", "j"])
    table = pa.Table.from_pandas(pdf)
    assert DataFrame.from_arrow(table).index_names() == ["k", "j"]
    range_table = pa.Table.from_pandas(_pdf())
    assert DataFrame.from_arrow(range_table).index_names() == []


def test_to_pandas_index_argument_wins_over_the_frame_index():
    df = DataFrame.from_pandas(_pdf()).set_index(["k"])
    got = df.to_pandas(index="j")
    assert list(got.index.names) == ["j"]
    assert "k" in got.columns


def _indexed(levels):
    pdf = _pdf().set_index(levels)
    return DataFrame.from_pandas(pdf), pdf


@pytest.mark.parametrize("levels", [["k"], ["k", "j"]])
def test_the_pandas_names_hide_the_index_columns_as_pandas_does(levels):
    ours, ref = _indexed(levels)
    assert ours.columns == list(ref.columns)
    assert len(ours.columns) == len(ref.columns)
    assert ours.shape == ref.shape
    assert ours.keys() == list(ref.keys())
    assert len(ours.dtypes) == len(ref.dtypes) == len(ref.columns)
    assert [name for name, _ in ours.items()] == [name for name, _ in ref.items()]
    assert ours.to_numpy().shape == ref.to_numpy().shape
    assert ours.size == ref.size
    assert not ours.empty


@pytest.mark.parametrize("levels", [["k"], ["k", "j"]])
def test_the_polars_names_still_see_every_column(levels):
    ours, _ = _indexed(levels)
    assert sorted(ours.schema) == ["j", "k", "v"]
    assert ours.width == 3
    assert sorted(ours.select("k", "v").schema) == ["k", "v"]
    assert list(ours.select((col("k") + 1).alias("k")).schema) == ["k"]
    assert ours.filter(col("k") > 1).index_names() == levels
    assert ours.with_columns(w=col("k") * 2)["w"].to_list() == [2, 2, 4, 4, 6]
    assert ours.group_by(*levels).agg(col("v").sum().alias("s")).num_rows == (
        3 if levels == ["k"] else 5
    )


def test_a_list_key_takes_the_shown_columns_and_keeps_the_index():
    ours, ref = _indexed(["k", "j"])
    got = ours[["v"]]
    assert got.columns == ["v"]
    assert got.index_names() == ["k", "j"]
    pd.testing.assert_frame_equal(
        got.to_pandas(), ref[["v"]], check_dtype=False, check_index_type=False
    )
    with pytest.raises(KeyError):
        ours[["k"]]
    with pytest.raises(KeyError):
        ref[["k"]]
    assert ours["k"].to_list() == [1, 1, 2, 2, 3]


def test_the_reducers_skip_the_index_columns_as_pandas_does():
    ours, ref = _indexed(["k"])
    assert set(ours.sum().to_dict()) == set(ref.sum(numeric_only=True).index)
    assert ours.mean().to_dict()["v"] == [ref["v"].mean()]
    assert ours.max().to_dict()["v"] == [ref["v"].max()]
    assert "k" not in ours.mean().to_dict()


def test_pandas_methods_keep_the_index_of_the_frame():
    ours, ref = _indexed(["k", "j"])
    for got, want in [
        (ours.fillna(0), ref.fillna(0)),
        (ours.copy(), ref.copy()),
        (ours.sort_values("v"), ref.sort_values("v")),
        (ours.head(2), ref.head(2)),
        (ours.drop(columns="v"), ref.drop(columns="v")),
        (ours.rename(columns={"v": "w"}), ref.rename(columns={"v": "w"})),
        (ours.astype({"v": "float32"}), ref.astype({"v": "float32"})),
        (ours.filter(items=["v"]), ref.filter(items=["v"])),
        (ours.add(1), ref + 1),
        (ours.gt(2), ref > 2),
    ]:
        assert got.index_names() == ["k", "j"]
        assert got.columns == list(want.columns)
        pd.testing.assert_frame_equal(
            got.to_pandas(), want, check_dtype=False, check_index_type=False
        )


def test_a_frame_with_no_index_changes_in_nothing():
    df = DataFrame.from_pandas(_pdf())
    assert df.columns == ["k", "j", "v"]
    assert df.shape == (5, 3)
    assert df.keys() == ["k", "j", "v"]
    assert df[["k", "v"]].columns == ["k", "v"]
    assert df[["k", "v"]].index_names() == []


def _leaky():
    pdf = pd.DataFrame(
        {
            "k": [1.5, -2.5, None, 4.4, 1.5],
            "v": [10.0, 10.0, 20.0, None, 10.0],
            "w": [-1.0, -1.0, 2.0, 3.0, -1.0],
        }
    )
    return DataFrame.from_pandas(pdf).set_index("k"), pdf.set_index("k")


@pytest.mark.parametrize("name", ["unpivot", "melt"])
def test_unpivot_and_melt_give_no_index_as_pandas_melt_does(name):
    frame = DataFrame({"k": [1, 2], "x": [1, 2], "y": [3, 4]}).set_index("k")
    ref = pd.DataFrame({"k": [1, 2], "x": [1, 2], "y": [3, 4]}).set_index("k")
    method = getattr(frame, name)
    got = method(index=["x"], on=["y"]) if name == "unpivot" else method(["x"], ["y"])
    assert got.index_names() == []
    want = ref.melt(id_vars=["x"], value_vars=["y"])
    assert isinstance(want.index, pd.RangeIndex)
    assert got.to_pandas().index.equals(want.index)
    assert frame.lazy().unpivot(["x"], ["y"]).collect().index_names() == []


def test_the_pandas_methods_read_the_shown_columns_only():
    ours, ref = _leaky()
    for got, want in [
        (ours.drop_duplicates(), ref.drop_duplicates()),
        (ours.dropna(), ref.dropna()),
        (ours.fillna(0), ref.fillna(0)),
    ]:
        assert got.index_names() == ["k"]
        pd.testing.assert_frame_equal(
            got.to_pandas(), want, check_dtype=False, check_index_type=False
        )
    assert ours.to_dict() == {
        c: [None if pd.isna(x) else x for x in ref[c].tolist()] for c in ref.columns
    }
    assert [c for c in ours.describe().columns if c != "statistic"] == list(ref.describe().columns)
    assert ours.to_csv().splitlines()[0] == ref.to_csv().splitlines()[0] == "k,v,w"
    assert ours.reset_index(drop=True).to_csv().splitlines()[0] == "v,w"
    assert DataFrame({"a": [1]}).to_csv().splitlines()[0] == "a"


def test_the_polars_names_still_read_every_column():
    ours, _ = _leaky()
    assert ours.unique().num_rows == 4
    assert ours.is_duplicated().to_list() == [True, False, False, False, True]
    assert ours.drop_nulls().num_rows == 3
    assert sorted(ours.drop("k").schema) == ["v", "w"]


def test_drop_columns_of_an_index_column_is_a_key_error_as_pandas():
    ours, ref = _leaky()
    with pytest.raises(KeyError):
        ref.drop(columns=["k"])
    with pytest.raises(KeyError):
        ours.drop(columns=["k"])
    assert ours.drop(columns=["v"]).index_names() == ["k"]


def test_the_cell_wise_pandas_methods_leave_the_index_alone():
    ours, ref = _leaky()
    for got, want in [
        (ours.round(0), ref.round(0)),
        (ours.clip(0, 5), ref.clip(0, 5)),
        (ours.abs(), ref.abs()),
        (ours.replace(1.5, 9.0), ref.replace(1.5, 9.0)),
        (ours.astype({"w": "float32"}), ref.astype({"w": "float32"})),
        (ours.cumsum(), ref.cumsum()),
        (ours.diff(), ref.diff()),
        (ours.rank(), ref.rank()),
    ]:
        assert got.index_names() == ["k"]
        pd.testing.assert_frame_equal(
            got.to_pandas(), want, check_dtype=False, check_index_type=False
        )


def test_groupby_indexes_by_the_keys_as_pandas_and_group_by_does_not():
    pdf = pd.DataFrame({"k": [1, 2, 1, 2], "v": [1.0, 2.0, 3.0, 4.0]})
    ours = DataFrame.from_pandas(pdf)
    got = ours.groupby("k").sum()
    assert got.index_names() == ["k"] and got.columns == ["v"]
    pd.testing.assert_frame_equal(
        got.to_pandas(), pdf.groupby("k").sum(), check_dtype=False, check_index_type=False
    )
    flat = ours.groupby("k", as_index=False).sum()
    assert flat.index_names() == [] and flat.columns == ["k", "v"]
    pd.testing.assert_frame_equal(
        flat.to_pandas(), pdf.groupby("k", as_index=False).sum(), check_dtype=False
    )
    assert ours.groupby("k", "sum:v").index_names() == ["k"]
    polars_style = ours.group_by("k").sum()
    assert polars_style.index_names() == [] and polars_style.columns == ["k", "v"]


def _gb():
    pdf = pd.DataFrame(
        {
            "k": [1.0, 2.0, 1.0, None, 3.0, 2.0],
            "g": list("aabbab"),
            "v": [10, 20, 30, 40, 50, 60],
            "w": [1.0, float("nan"), 3.0, 4.0, float("nan"), 6.0],
        }
    )
    return DataFrame.from_pandas(pdf).set_index("k"), pdf.set_index("k"), pdf


def _eq(got, want):
    assert list(got.columns) == list(want.columns)
    assert list(got.to_pandas().index.names) == list(want.index.names)
    pd.testing.assert_frame_equal(
        got.to_pandas().reset_index(), want.reset_index(), check_dtype=False, check_index_type=False
    )


@pytest.mark.parametrize(
    "name", ["sum", "mean", "min", "max", "count", "first", "last", "median", "std"]
)
def test_groupby_leaves_the_input_index_out_of_the_values(name):
    ours, ref, _ = _gb()
    got = getattr(ours.groupby("g"), name)()
    want = getattr(ref.groupby("g"), name)()
    if name in ("mean", "median", "std"):
        want = want[["v", "w"]]
    assert "k" not in got.columns and "k" not in got.to_pandas().columns
    _eq(got, want)


def test_groupby_agg_and_quantile_leave_the_input_index_out():
    ours, ref, _ = _gb()
    _eq(ours.groupby("g").agg("sum"), ref.groupby("g").agg("sum"))
    _eq(ours.groupby("g").agg({"v": "sum"}), ref.groupby("g").agg({"v": "sum"}))
    _eq(
        ours.groupby("g").quantile(0.5),
        ref[["g", "v", "w"]].groupby("g").quantile(0.5),
    )
    flat = ours.groupby("g", as_index=False).sum()
    assert flat.columns == ["g", "v", "w"] and flat.index_names() == []
    assert ours.groupby("g").size().columns == ["size"]
    assert "k" not in ours.groupby("g").nunique().to_pandas().columns
    assert ours.groupby("g").describe().columns[0].startswith("v_")


def test_groupby_cumsum_keeps_the_input_index_and_skips_it_as_a_value():
    ours, ref, _ = _gb()
    got = ours.groupby("g").cumsum()
    want = ref.groupby("g").cumsum()
    assert got.columns == list(want.columns) and got.index_names() == ["k"]
    pd.testing.assert_frame_equal(
        got.to_pandas().reset_index(), want.reset_index(), check_dtype=False, check_index_type=False
    )


def test_an_index_level_named_as_a_key_stays_a_key():
    ours, ref, pdf = _gb()
    num = DataFrame.from_pandas(pdf[["k", "v", "w"]]).set_index("k")
    refn = pdf[["k", "v", "w"]].set_index("k")
    for by in ("k", ["k"]):
        got = num.groupby(by).sum()
        assert got.index_names() == ["k"]
        _eq(got, refn.groupby(by).sum())
    two = ours.groupby(["g", "k"]).sum()
    assert two.index_names() == ["g", "k"]
    _eq(two, ref.groupby(["g", "k"]).sum())


def test_subset_names_are_checked_against_the_shown_columns():
    ours, ref, _ = _gb()
    for call in (
        lambda f: f.drop_duplicates(subset=["k"]),
        lambda f: f.drop_duplicates(subset=["zz"]),
        lambda f: f.dropna(subset=["k"]),
        lambda f: f.dropna(subset=["zz"]),
        lambda f: f.duplicated(subset=["k"]),
    ):
        with pytest.raises(KeyError):
            call(ref)
        with pytest.raises(KeyError):
            call(ours)
    _eq(ours.drop_duplicates(subset=["g"]), ref.drop_duplicates(subset=["g"]))
    _eq(ours.drop_duplicates(subset="g"), ref.drop_duplicates(subset="g"))


@pytest.mark.parametrize(
    "kwargs",
    [
        {},
        {"subset": ["w"]},
        {"subset": "w"},
        {"how": "all"},
        {"how": "all", "subset": ["v", "w"]},
        {"how": "any", "subset": ["w"]},
        {"thresh": 3},
        {"axis": 1},
        {"axis": "columns"},
        {"axis": 1, "how": "all"},
        {"axis": 1, "thresh": 5},
    ],
)
def test_dropna_takes_the_pandas_arguments(kwargs):
    ours, ref, _ = _gb()
    got, want = ours.dropna(**kwargs), ref.dropna(**kwargs)
    assert got.index_names() == ["k"]
    _eq(got, want)


def test_dropna_refuses_what_pandas_refuses():
    ours, ref, _ = _gb()
    for kwargs, exc in [
        ({"how": "x"}, ValueError),
        ({"how": "any", "thresh": 1}, TypeError),
        ({"axis": 2}, ValueError),
    ]:
        with pytest.raises(exc):
            ref.dropna(**kwargs)
        with pytest.raises(exc):
            ours.dropna(**kwargs)
    with pytest.raises(KeyError):
        ref.dropna(axis=1, subset=["v"])
    with pytest.raises(KeyError, match="row labels"):
        ours.dropna(axis=1, subset=["v"])
    pdf = _gb()[2]
    got = DataFrame.from_pandas(pdf).dropna(subset=["w"]).to_pandas()
    pd.testing.assert_frame_equal(
        got.reset_index(drop=True),
        pdf.dropna(subset=["w"]).reset_index(drop=True),
        check_dtype=False,
    )


@pytest.mark.parametrize(
    "value",
    [0, {"w": 0}, {"w": 0, "v": 5}, {"k": 0}, {"zz": 0}],
)
def test_fillna_takes_a_scalar_or_a_mapping(value):
    ours, ref, _ = _gb()
    got, want = ours.fillna(value), ref.fillna(value)
    assert got.index_names() == ["k"]
    _eq(got, want)
    assert ours.fillna(value=value).columns == got.columns


def test_fillna_axis_forms():
    ours, ref, _ = _gb()
    _eq(ours.fillna(0, axis=1), ref.fillna(0, axis=1))
    # pandas 2 and 3 differ on a mapping with axis=1; ours refuses it plainly.
    with pytest.raises(NotImplementedError, match="axis=1"):
        ours.fillna({"w": 0}, axis=1)
    with pytest.raises(ValueError):
        ours.fillna(0, axis=2)


def _gl():
    pdf = pd.DataFrame(
        {
            "k": [10, 20, 30, 40, 50, 60],
            "g": list("aabbab"),
            "v": [1.0, 2.0, 3.0, 4.0, 5.0, 6.0],
            "w": [6.0, 5.0, 4.0, 3.0, 2.0, 1.0],
        }
    )
    return DataFrame.from_pandas(pdf).set_index("k"), pdf.set_index("k")


def _same_rows(got, want):
    assert got.index_names() == ["k"]
    frame = got.to_pandas()
    assert frame.index.tolist() == want.index.tolist()
    assert list(got.columns) == list(want.columns)
    pd.testing.assert_frame_equal(frame, want, check_dtype=False, check_index_type=False)


@pytest.mark.parametrize(
    "call",
    [
        lambda g: g.head(1),
        lambda g: g.head(2),
        lambda g: g.tail(1),
        lambda g: g.nth(0),
        lambda g: g.nth(-1),
        lambda g: g.nth([0, 1]),
        lambda g: g.cumsum(),
        lambda g: g.shift(),
        lambda g: g.ffill(),
    ],
)
def test_group_methods_that_return_rows_keep_the_original_labels(call):
    ours, ref = _gl()
    got = call(ours.groupby("g"))
    want = call(ref.groupby("g"))
    _same_rows(got, want[list(got.columns)] if set(got.columns) <= set(want.columns) else want)


@pytest.mark.parametrize("func", ["sum", "mean", "max"])
def test_group_transform_keeps_the_original_labels(func):
    ours, ref = _gl()
    _same_rows(ours.groupby("g").transform(func), ref.groupby("g")[["v", "w"]].transform(func))


def test_group_filter_and_apply_keep_the_original_labels():
    ours, ref = _gl()
    _same_rows(
        ours.groupby("g").filter(lambda x: x["v"].sum() > 8),
        ref.groupby("g").filter(lambda x: x["v"].sum() > 8),
    )
    _same_rows(
        ours.groupby("g").filter(lambda x: len(x) > 2 and True),
        ref.groupby("g").filter(lambda x: len(x) > 2 and True),
    )
    got = ours.groupby("g").apply(lambda x: x.head(1))
    assert got.index_names() == ["g", "k"]
    assert got.to_pandas().index.tolist() == [("a", 10), ("b", 30)]
    taken = ours.groupby("g").take([0, 1])
    assert taken.index_names() == ["g", "k"]
    assert taken.to_pandas().index.tolist() == [("a", 10), ("a", 20), ("b", 30), ("b", 40)]
    # a group-by that does not index by the keys keeps the labels the rows had
    assert ours.groupby("g", as_index=False).take([0, 1]).to_pandas().index.tolist() == [
        10,
        20,
        30,
        40,
    ]
    assert ours.groupby("g").sample(1, random_state=0).index_names() == ["k"]


def test_group_head_with_a_null_key_keeps_the_labels():
    pdf = pd.DataFrame(
        {"k": [10, 20, 30, 40], "g": ["a", None, "b", "a"], "v": [1.0, 2.0, 3.0, 4.0]}
    )
    ours = DataFrame.from_pandas(pdf).set_index("k")
    ref = pdf.set_index("k")
    _same_rows(ours.groupby("g").head(1), ref.groupby("g").head(1).dropna(subset=["g"]))
