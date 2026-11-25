# SPDX-License-Identifier: MIT
"""A column of view strings (Arrow ``string_view``) converts to pandas as a
column of plain strings does. pandas and the pyarrow ``take`` and ``filter``
kernels do not take view strings, and pyarrow 17 cannot cast them, so the
column is made flat natively first."""

import pytest

pd = pytest.importorskip("pandas")
pa = pytest.importorskip("pyarrow")

from dftracer.utils import (  # noqa: E402
    DataFrame,
    Series,
    TraceWriteOptions,
)
from dftracer.utils.trace import TraceViewer  # noqa: E402

pytestmark = pytest.mark.skipif(
    not hasattr(pa, "string_view"), reason="pyarrow has no string_view type"
)

TEXT = ["b", None, "a", "b", "c"]


def _view(values):
    return Series.from_arrow(pa.array(values, pa.string_view()))


def _plain(values):
    return Series.from_arrow(pa.array(values, pa.string()))


def _dictionary(dictionary, kind):
    indices = pa.array([0, 1, None, 0, 2], pa.int64())
    return Series.from_arrow(pa.DictionaryArray.from_arrays(indices, pa.array(dictionary, kind)))


def test_a_view_column_converts_as_a_plain_one():
    pd.testing.assert_series_equal(_view(TEXT).to_pandas(), _plain(TEXT).to_pandas())


def test_a_view_column_converts_with_arrow_dtypes():
    got = _view(TEXT).to_pandas(arrow=True)
    want = _plain(TEXT).to_pandas(arrow=True)
    pd.testing.assert_series_equal(got, want)
    # pandas filters the values to show them
    assert repr(got) == repr(want)


def test_a_dictionary_of_view_strings_converts_as_a_plain_one():
    got = _dictionary(["x", "y", "z"], pa.string_view()).to_pandas()
    want = _dictionary(["x", "y", "z"], pa.string()).to_pandas()
    pd.testing.assert_series_equal(got, want)


def test_a_view_dictionary_with_repeated_and_null_labels_converts():
    got = _dictionary(["x", None, "x"], pa.string_view()).to_pandas()
    want = _dictionary(["x", None, "x"], pa.string()).to_pandas()
    pd.testing.assert_series_equal(got, want)


def test_a_frame_with_a_view_index_level_converts_with_arrow_dtypes():
    def frame(kind):
        table = pa.table({"k": pa.array(["a", "b", "c"], kind), "v": [1, 2, 3]})
        return DataFrame.from_arrow(table).set_index("k")

    got = frame(pa.string_view()).to_pandas(arrow=True)
    want = frame(pa.string()).to_pandas(arrow=True)
    pd.testing.assert_frame_equal(got, want)


def test_a_table_read_back_converts_its_strings_and_levels(tmp_path):
    df = pd.DataFrame(
        {"s": pd.array(TEXT, dtype="str"), "v": range(5)},
        index=pd.MultiIndex.from_arrays([range(5), list("abcde")], names=[None, "lvl"]),
    )
    path = str(tmp_path / "t")
    DataFrame.from_pandas(df).write_table(
        path,
        name="t",
        mode="overwrite",
        options=TraceWriteOptions(build_index=False),
    )
    pd.testing.assert_frame_equal(TraceViewer(path).collect().to_pandas(), df)
