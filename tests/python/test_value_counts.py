# SPDX-License-Identifier: MIT
"""``Series.value_counts`` and ``DataFrame.value_counts`` in the pandas shape: a
Series of counts labelled by the values. Every form runs on real pandas and on
ours over the same data, and the labels, level names, values and errors are
compared. The tie cases stay under 17 rows, where pandas sorts stably."""

import datetime as dt
import math

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402

from .common import skip_below_pandas3  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

NAN = float("nan")
DAY = dt.datetime(2024, 1, 1)
VALUES = {
    "int": [5, 1, 1, 2, 2, 3, 3, 3, 9],
    "int_nulls": [5, 1, None, 2, 2, None, 3, 3, 3, 9],
    "float": [1.5, 2.5, 2.5, 0.5, 1.5, 2.5],
    "float_nan": [1.5, NAN, NAN, 2.5, None, 2.5, 0.5],
    "string": ["b", "a", "a", "c", "c", "b", "z", "a"],
    "string_nulls": ["b", "a", "a", None, "c", "c", "b", "z", None],
    "bool": [True, False, True, True, False],
    "bool_nulls": [True, None, False, True, None, True],
    "time": [DAY, DAY + dt.timedelta(days=2), DAY, DAY + dt.timedelta(days=1), DAY],
    "time_nulls": [DAY, None, DAY + dt.timedelta(days=1), DAY, None, DAY + dt.timedelta(days=1)],
    "ties": [3, 3, 1, 1, 2, 2, 7],
}
KWARGS = [
    {},
    {"normalize": True},
    {"sort": False},
    {"ascending": True},
    {"sort": False, "ascending": True},
    {"dropna": False},
    {"dropna": False, "normalize": True},
    {"dropna": False, "sort": False},
    {"dropna": False, "ascending": True},
    {"normalize": True, "sort": False, "dropna": False},
]


def _norm(v):
    if v is None or v is pd.NA or v is pd.NaT or (isinstance(v, float) and math.isnan(v)):
        return None
    if isinstance(v, pd.Timestamp):
        return v.to_pydatetime()
    if isinstance(v, tuple):
        return tuple(_norm(i) for i in v)
    return v


def _compare(ours, ref, text_index=False):
    skip_below_pandas3()
    assert isinstance(ours, Series)
    got = ours.to_pandas()
    assert got.name == ref.name
    assert list(got.index.names) == list(ref.index.names)
    index = [str(i) for i in ref.index] if text_index else ref.index.tolist()
    if ref.index.nlevels == 1:
        index = [i[0] if isinstance(i, tuple) else i for i in index]
    labels = [_norm(i) for i in got.index.tolist()]
    assert labels == [_norm(i) for i in index]
    assert got.tolist() == pytest.approx(ref.tolist())


@pytest.mark.parametrize("kwargs", KWARGS, ids=str)
@pytest.mark.parametrize("name", VALUES)
def test_series_value_counts_matches_pandas(name, kwargs):
    values = VALUES[name]
    _compare(
        Series.from_list(values).value_counts(**kwargs), pd.Series(values).value_counts(**kwargs)
    )


@pytest.mark.parametrize("kwargs", [{}, {"normalize": True}, {"dropna": False}])
def test_a_named_series_names_the_labels(kwargs):
    ours = Series.from_list([1, 2, 2, 3, None]).rename("x")
    ref = pd.Series([1, 2, 2, 3, None], name="x")
    _compare(ours.value_counts(**kwargs), ref.value_counts(**kwargs))


def test_the_row_labels_of_the_series_do_not_matter():
    ref = pd.Series([4, 4, 5], index=[7, 8, 9], name="x")
    ours = Series.from_pandas(ref)
    assert [_norm(i) for i in ours.index.to_list()] == [7, 8, 9]
    _compare(ours.value_counts(), ref.value_counts())


def test_an_empty_series():
    _compare(
        Series.from_list([1, 2]).head(0).value_counts(), pd.Series([1, 2]).head(0).value_counts()
    )
    _compare(
        Series.from_list([1, 2]).head(0).value_counts(normalize=True),
        pd.Series([1, 2]).head(0).value_counts(normalize=True),
    )


def test_an_all_null_series():
    values = [None, None]
    ref = pd.Series(values, dtype="float64")
    ours = Series.from_list(values).astype("float64")
    _compare(ours.value_counts(), ref.value_counts())
    _compare(ours.value_counts(dropna=False), ref.value_counts(dropna=False))


BIN_DATA = [1, 2, 2, 3, None, 9, -5, 1.0, 4.5, 4.5]
BIN_FORMS = [
    {"bins": 3},
    {"bins": 1},
    {"bins": 5},
    {"bins": [0, 2, 3, 4]},
    {"bins": [-6, 0, 1, 4.5, 10]},
    {"bins": 3, "normalize": True},
    {"bins": [0, 2, 3, 4], "normalize": True},
    {"bins": 3, "sort": False},
    {"bins": [0, 2, 3, 4], "sort": False},
    {"bins": 3, "ascending": True},
    {"bins": [0, 2, 3, 4], "ascending": True},
    {"bins": 3, "dropna": False},
    {"bins": [0, 2, 3, 4], "dropna": False, "normalize": True},
]


@pytest.mark.parametrize("kwargs", BIN_FORMS, ids=str)
def test_bins_match_pandas(kwargs):
    _compare(
        Series.from_list(BIN_DATA).value_counts(**kwargs),
        pd.Series(BIN_DATA).value_counts(**kwargs),
        text_index=True,
    )


@pytest.mark.parametrize(
    "values",
    [[3, 3, 3], [0, 0], [10, 20, 30, 40], [0.0001, 0.0002, 0.0007], [1e9, 2e9, 5e9], [-3, -1]],
    ids=str,
)
@pytest.mark.parametrize("bins", [1, 2, 4])
def test_integer_bins_label_the_intervals_as_pandas(values, bins):
    _compare(
        Series.from_list(values).value_counts(bins=bins),
        pd.Series(values).value_counts(bins=bins),
        text_index=True,
    )


@pytest.mark.parametrize(
    "bins, error",
    [(0, ValueError), (-2, ValueError), ([3, 2, 1], ValueError), ([1, 1, 2], ValueError)],
)
def test_bad_bins_raise_as_pandas(bins, error):
    with pytest.raises(error):
        pd.Series([1, 2, 3]).value_counts(bins=bins)
    with pytest.raises(error):
        Series.from_list([1, 2, 3]).value_counts(bins=bins)


def test_bins_of_an_empty_series_raise():
    with pytest.raises(ValueError):
        pd.Series([1.0]).head(0).value_counts(bins=2)
    with pytest.raises(ValueError):
        Series.from_list([1.0]).head(0).value_counts(bins=2)


def test_bins_need_numbers():
    with pytest.raises(TypeError):
        Series.from_list(["a", "b"]).value_counts(bins=2)


FRAME = {
    "a": [1, 1, 2, 2, 1, None, 3, None],
    "b": ["x", "x", "y", "z", "x", "y", None, None],
    "c": [1.5, 1.5, NAN, 2.5, 1.5, 2.5, NAN, NAN],
    "d": [True, True, False, True, True, False, True, True],
}
FRAME_KWARGS = [
    {},
    {"normalize": True},
    {"sort": False},
    {"ascending": True},
    {"dropna": False},
    {"dropna": False, "sort": False},
    {"dropna": False, "ascending": True, "normalize": True},
]


@pytest.mark.parametrize("kwargs", FRAME_KWARGS, ids=str)
@pytest.mark.parametrize(
    "subset", [None, ["a"], ["b"], ["a", "b"], ["b", "c"], ["d", "a", "b"]], ids=str
)
def test_frame_value_counts_matches_pandas(subset, kwargs):
    ours = DataFrame.from_pandas(pd.DataFrame(FRAME)).value_counts(subset, **kwargs)
    ref = pd.DataFrame(FRAME).value_counts(subset, **kwargs)
    _compare(ours, ref)


def test_a_frame_with_complete_rows():
    frame = {"a": [1, 2, 1, 2, 1], "b": ["p", "q", "p", "q", "r"]}
    _compare(DataFrame(frame).value_counts(), pd.DataFrame(frame).value_counts())
    _compare(
        DataFrame(frame).value_counts(ascending=True),
        pd.DataFrame(frame).value_counts(ascending=True),
    )


def test_a_frame_with_labels_counts_the_columns_only():
    ref = pd.DataFrame({"a": [1, 1, 2]}, index=[5, 6, 7])
    ours = DataFrame.from_pandas(ref)
    _compare(ours.value_counts(), ref.value_counts())


@pytest.mark.parametrize("kind", ["int", "string"])
@pytest.mark.parametrize("ascending", [False, True])
def test_ties_keep_the_first_appearance_over_many_values(kind, ascending):
    import random

    rng = random.Random(7)
    values = [rng.randrange(300) for _ in range(20000)]
    if kind == "string":
        values = [f"v{v}" for v in values]
    counts = {}
    for v in values:
        counts[v] = counts.get(v, 0) + 1
    expected = sorted(counts, key=lambda v: counts[v] if ascending else -counts[v])
    got = Series.from_list(values).value_counts(ascending=ascending)
    assert got.index.to_list() == expected
    assert got.to_list() == [counts[v] for v in expected]
    first = Series.from_list(values).value_counts(sort=False)
    assert first.index.to_list() == list(counts)


@pytest.mark.parametrize("dtype", ["bool", "int32", "uint8", "float32", "int64", "str"])
def test_the_labels_keep_the_type_of_the_column(dtype):
    values = {"bool": [True, False, True], "str": ["a", "b", "a"]}.get(dtype, [1, 2, 1])
    ref = pd.DataFrame({"k": values, "j": [1, 2, 1]}).astype({"k": dtype})
    ours = DataFrame.from_pandas(ref)
    for call in (
        lambda o: o.value_counts(),
        lambda o: o.value_counts(["k"], sort=False),
        lambda o: o["k"].value_counts(),
        lambda o: o["k"].value_counts(sort=False),
    ):
        got = call(ours).to_pandas().index
        want = call(ref).index
        levels = [got.get_level_values(i) for i in range(got.nlevels)]
        refs = [want.get_level_values(i) for i in range(want.nlevels)]
        assert [pd.api.types.is_bool_dtype(x) for x in levels] == [
            pd.api.types.is_bool_dtype(x) for x in refs
        ]
        assert [x.dtype.itemsize for x in levels if x.dtype.kind in "iuf"] == [
            x.dtype.itemsize for x in refs if x.dtype.kind in "iuf"
        ]


@pytest.mark.parametrize(
    "name",
    ["count", "proportion", "__dftu_row__", "__dftu_count__", "index", "value", "__dftu_key0__"],
)
@pytest.mark.parametrize(
    "kwargs", [{}, {"normalize": True}, {"sort": False}, {"dropna": False}], ids=str
)
def test_a_key_column_may_have_any_name(name, kwargs):
    raw = {name: [1, 1, 2, 2, 2, None], "other": ["a", "a", "b", "b", "c", "c"]}
    ref = pd.DataFrame(raw)
    ours = DataFrame.from_pandas(ref)
    for subset in (None, [name], [name, "other"], ["other", name]):
        _compare(
            ours.value_counts(subset, **kwargs),
            ref.value_counts(subset, **kwargs),
        )
    named = Series.from_list(raw[name]).rename(name)
    _compare(named.value_counts(**kwargs), pd.Series(raw[name], name=name).value_counts(**kwargs))


def _float(bits):
    import struct

    return struct.unpack("<d", struct.pack("<Q", bits))[0]


ZEROS_AND_NANS = [
    -0.0,
    0.0,
    1.5,
    NAN,
    _float(0xFFF0000000000123),
    _float(0x7FF0000000000007),
    0.0,
    1.5,
    None,
]


@pytest.mark.parametrize(
    "kwargs", [{}, {"sort": False}, {"dropna": False}, {"normalize": True}], ids=str
)
def test_negative_zero_is_zero_and_every_nan_is_one_value(kwargs):
    ref = pd.Series(ZEROS_AND_NANS).value_counts(**kwargs)
    ours = Series.from_list(ZEROS_AND_NANS).value_counts(**kwargs)
    # pandas keeps the first of -0.0 and 0.0 as the label; both print as zero
    _compare(ours, ref)


def test_a_frame_counts_negative_zero_and_nans_as_pandas():
    raw = {"a": ZEROS_AND_NANS, "b": [1, 1, 1, 1, 1, 1, 1, 1, 1]}
    ref = pd.DataFrame(raw)
    ours = DataFrame.from_pandas(ref)
    for kwargs in ({}, {"dropna": False}):
        _compare(ours.value_counts(**kwargs), ref.value_counts(**kwargs))


def test_group_by_keys_negative_zero_and_nans_as_pandas_and_polars():
    from dftracer.utils import col

    values = [-0.0, 0.0, 1.5, NAN, _float(0xFFF0000000000123), 0.0, 1.5]
    ours = DataFrame({"k": values, "v": list(range(len(values)))})
    grouped = ours.group_by("k").agg(col("v").count().alias("n")).to_dict()
    got = {("nan" if k != k else k): n for k, n in zip(grouped["k"], grouped["n"])}
    assert got == {0.0: 3, 1.5: 2, "nan": 2}
    sized = ours.groupby("k").size().to_list()
    assert sorted(sized) == [2, 3]  # the pandas groupby drops the NaN key
    pl = pytest.importorskip("polars")
    ref = pl.DataFrame({"k": values}).group_by("k").len().to_dict(as_series=False)
    assert sorted(ref["len"]) == [2, 2, 3]


@pytest.mark.parametrize(
    "values, bins",
    [
        ([0.004805, 0.0], 2),
        ([0.004805, 0.0, 0.002], 3),
        ([0.125, 0.0], [0, 0.125, 0.25]),
        ([2.675, 0.0], [0, 2.675, 5.0]),
        ([0.0049, 0.0], 7),
        ([1.0005, 2.0015], 3),
    ],
    ids=str,
)
def test_bin_labels_round_as_numpy(values, bins):
    _compare(
        Series.from_list(values).value_counts(bins=bins),
        pd.Series(values).value_counts(bins=bins),
        text_index=True,
    )


@pytest.mark.parametrize(
    "values, bins, kwargs",
    [
        ([1, 2], [10, 20], {}),
        ([1, 2], [10, 20], {"normalize": True}),
        ([1, 2], [10, 20, 30], {"sort": False}),
        ([1, 2], [10, 20], {"dropna": False}),
        ([None, None], [1, 2], {}),
        ([None, None], [1, 2], {"dropna": False}),
        ([1.0, None], [10, 20], {"dropna": False}),
        ([1.0, None, 2.0], [10, 20], {"dropna": False}),
        ([1.0, None, 2.0], [10, 20], {}),
        ([1.0, 15.0, None], [10, 20], {"dropna": False}),
        ([1.0, 15.0, None], [10, 20], {}),
    ],
    ids=str,
)
def test_bins_that_catch_nothing_give_an_empty_series(values, bins, kwargs):
    ref = pd.Series(values, dtype="float64").value_counts(bins=bins, **kwargs)
    ours = Series.from_list(values).astype("float64").value_counts(bins=bins, **kwargs)
    _compare(ours, ref, text_index=True)


@pytest.mark.parametrize("dropna", [True, False])
@pytest.mark.parametrize("values", [[NAN, NAN], [None, None], [NAN, None]], ids=str)
def test_integer_bins_of_a_series_with_no_value_raise(values, dropna):
    with pytest.raises(ValueError, match="Bin edges must be unique"):
        pd.Series(values, dtype="float64").value_counts(bins=2, dropna=dropna)
    with pytest.raises(ValueError, match="Bin edges must be unique"):
        Series.from_list(values).astype("float64").value_counts(bins=2, dropna=dropna)
