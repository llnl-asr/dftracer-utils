# SPDX-License-Identifier: MIT
"""The native paths of the methods that once looped over rows in Python, and
the lazy imports that keep ``import dftracer.utils`` short."""

import decimal
import subprocess
import sys

import pytest

from dftracer.utils import DataFrame, DType, Series


def test_import_loads_neither_metadata_nor_asyncio():
    code = (
        "import sys, dftracer.utils;"
        "bad = [m for m in ('importlib.metadata', 'asyncio', 'inspect', 'logging') if m in sys.modules];"
        "print(','.join(bad))"
    )
    out = subprocess.run(
        [sys.executable, "-c", code], capture_output=True, text=True, check=True
    ).stdout
    assert "importlib.metadata" not in out
    assert "asyncio" not in out


def test_version_comes_from_the_generated_file():
    import dftracer.utils as d

    try:
        from dftracer.utils._version import __version__
    except ImportError:
        assert d.__version__ == "0.0.0"
    else:
        assert d.__version__ == __version__


class TestValidIndex:
    def test_series_edges(self):
        s = Series.from_list([None, None, 3, None, 5, None])
        assert s.first_valid_index() == 2
        assert s.last_valid_index() == 4

    def test_series_without_a_value(self):
        s = Series.from_list([None, None], DType.STRING)
        assert s.first_valid_index() is None
        assert s.last_valid_index() is None
        assert Series.from_list([1]).first_valid_index() == 0

    def test_frame_edges_use_any_column(self):
        df = DataFrame({"a": [None, 1, None, None], "b": [None, None, None, 2.0]})
        assert df.first_valid_index() == 1
        assert df.last_valid_index() == 3


class TestEqualsAndCompare:
    def test_equals_null_for_null_and_types(self):
        a = Series.from_list([1, None, 3])
        assert a.equals(Series.from_list([1, None, 3]))
        assert not a.equals(Series.from_list([1, 2, 3]))
        assert not a.equals(Series.from_list([1.0, None, 3.0]))
        assert Series.from_list([True, None]).equals(Series.from_list([True, None]))
        assert not Series.from_list([True, False]).equals(Series.from_list([True, True]))
        assert Series.from_list([[1], [2]]).equals(Series.from_list([[1], [2]]))

    def test_frame_equals(self):
        a = DataFrame({"x": [1, 2], "s": ["a", None]})
        assert a.equals(DataFrame({"x": [1, 2], "s": ["a", None]}))
        assert not a.equals(DataFrame({"x": [1, 2], "s": ["a", "b"]}))
        assert not a.equals(DataFrame({"x": [1], "s": ["a"]}))

    def test_compare_nulls_and_flags(self):
        a = Series.from_list([1, None, 3, None, 5])
        b = Series.from_list([1, 2, 4, None, 5])
        assert a.compare(b).to_dict() == {"index": [1, 2], "self": [None, 3], "other": [2, 4]}
        assert a.compare(b, keep_shape=True).to_dict() == {
            "index": [0, 1, 2, 3, 4],
            "self": [None, None, 3, None, None],
            "other": [None, 2, 4, None, None],
        }
        assert a.compare(b, keep_shape=True, keep_equal=True).to_dict()["self"] == [
            1,
            None,
            3,
            None,
            5,
        ]

    def test_compare_bool_and_string(self):
        a = Series.from_list([True, False, None])
        b = Series.from_list([True, True, None])
        assert a.compare(b).to_dict()["index"] == [1]
        s = Series.from_list(["a", "b", None])
        t = Series.from_list(["a", "c", "d"])
        assert s.compare(t).to_dict() == {"index": [1, 2], "self": ["b", None], "other": ["c", "d"]}


class TestGroups:
    def test_positions_in_input_order_and_first_seen_keys(self):
        n = 5000
        df = DataFrame({"k": [(i * 7) % 13 for i in range(n)], "v": list(range(n))})
        groups = df.groupby("k").groups
        want = {}
        for i in range(n):
            want.setdefault((i * 7) % 13, []).append(i)
        assert groups == want
        assert list(groups) == list(want)

    def test_two_keys_and_null_group(self):
        df = DataFrame({"a": ["x", "y", None, "x", None], "b": [1, 1, 2, 1, 2]})
        assert df.groupby(["a", "b"], dropna=False).groups == {
            ("x", 1): [0, 3],
            ("y", 1): [1],
            (None, 2): [2, 4],
        }
        assert df.groupby("a").groups == {"x": [0, 2], "y": [1]}

    def test_empty_frame(self):
        assert DataFrame({"k": [], "v": []}).groupby("k").groups == {}


class TestRepeatAndDrop:
    def test_repeat(self):
        s = Series.from_list(["a", None, "c"]).rename("n")
        assert s.repeat(2).to_list() == ["a", "a", None, None, "c", "c"]
        assert s.repeat(2).name == "n"
        assert s.repeat(0).to_list() == []
        assert s.repeat(1).to_list() == ["a", None, "c"]
        with pytest.raises(ValueError):
            s.repeat(-1)

    def test_drop(self):
        s = Series.from_list([10, 20, 30, 40]).rename("n")
        assert s.drop([1, 3]).to_list() == [10, 30]
        assert s.drop(0).name == "n"
        with pytest.raises(KeyError):
            s.drop(9)


class TestCompareKeepsEveryDtype:
    @pytest.mark.parametrize(
        "left, right",
        [
            ([[1], [2], None, [4]], [[1], [3], None, None]),
            ([{"a": 1}, {"a": 2}], [{"a": 1}, {"a": 3}]),
            (
                [decimal.Decimal("1.5"), decimal.Decimal("2")],
                [decimal.Decimal("1.5"), decimal.Decimal("3")],
            ),
        ],
    )
    def test_nested_and_decimal_columns(self, left, right):
        a, b = Series.from_list(left), Series.from_list(right)
        out = a.compare(b).to_dict()
        want = [i for i, (x, y) in enumerate(zip(a.to_list(), b.to_list())) if x != y]
        assert out["index"] == want
        assert out["self"] == [a.to_list()[i] for i in want]
        assert out["other"] == [b.to_list()[i] for i in want]
        assert len(a.compare(b, keep_shape=True, keep_equal=True)) == len(left)

    def test_numeric_against_string_differs_everywhere_but_null_with_null(self):
        a = Series.from_list([1, 2, 3])
        b = Series.from_list(["1", "2", "x"])
        assert a.compare(b).to_dict() == {
            "index": [0, 1, 2],
            "self": [1, 2, 3],
            "other": ["1", "2", "x"],
        }
        c = Series.from_list([1.0, None, 3.0])
        d = Series.from_list(["a", None, "x"])
        assert c.compare(d).to_dict() == {"index": [0, 2], "self": [1.0, 3.0], "other": ["a", "x"]}

    @pytest.mark.parametrize(
        "left, right", [([1, 2, 3], ["1", "2", "x"]), ([1.0, None, 3.0], ["a", None, "x"])]
    )
    def test_matches_pandas(self, left, right):
        pd = pytest.importorskip("pandas")
        want = pd.Series(left).compare(pd.Series(right))
        got = Series.from_list(left).compare(Series.from_list(right)).to_dict()
        assert got["index"] == list(want.index)
        assert got["self"] == want["self"].tolist()
        assert got["other"] == want["other"].tolist()
