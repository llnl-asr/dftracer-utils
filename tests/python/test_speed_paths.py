# SPDX-License-Identifier: MIT
"""The native paths of the methods that once looped over rows in Python, and
the lazy imports that keep ``import dftracer.utils`` short."""

import decimal
import subprocess
import sys

import pytest

from dftracer.utils import DataFrame, DType, Series


def _with_index(frame):
    """The columns of a compare result and, as ``index``, its row labels."""
    return {"index": frame.index.to_list(), **frame.to_dict()}


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
        assert _with_index(a.compare(b)) == {"index": [1, 2], "self": [None, 3], "other": [2, 4]}
        assert _with_index(a.compare(b, keep_shape=True)) == {
            "index": [0, 1, 2, 3, 4],
            "self": [None, None, 3, None, None],
            "other": [None, 2, 4, None, None],
        }
        assert _with_index(a.compare(b, keep_shape=True, keep_equal=True))["self"] == [
            1,
            None,
            3,
            None,
            5,
        ]

    def test_compare_bool_and_string(self):
        a = Series.from_list([True, False, None])
        b = Series.from_list([True, True, None])
        assert _with_index(a.compare(b))["index"] == [1]
        s = Series.from_list(["a", "b", None])
        t = Series.from_list(["a", "c", "d"])
        assert _with_index(s.compare(t)) == {
            "index": [1, 2],
            "self": ["b", None],
            "other": ["c", "d"],
        }


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
        assert df.groupby("a").groups == {"x": [0, 3], "y": [1]}

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
        out = _with_index(a.compare(b))
        want = [i for i, (x, y) in enumerate(zip(a.to_list(), b.to_list())) if x != y]
        assert out["index"] == want
        assert out["self"] == [a.to_list()[i] for i in want]
        assert out["other"] == [b.to_list()[i] for i in want]
        assert len(a.compare(b, keep_shape=True, keep_equal=True)) == len(left)

    def test_numeric_against_string_differs_everywhere_but_null_with_null(self):
        a = Series.from_list([1, 2, 3])
        b = Series.from_list(["1", "2", "x"])
        assert _with_index(a.compare(b)) == {
            "index": [0, 1, 2],
            "self": [1, 2, 3],
            "other": ["1", "2", "x"],
        }
        c = Series.from_list([1.0, None, 3.0])
        d = Series.from_list(["a", None, "x"])
        assert _with_index(c.compare(d)) == {
            "index": [0, 2],
            "self": [1.0, 3.0],
            "other": ["a", "x"],
        }

    @pytest.mark.parametrize(
        "left, right", [([1, 2, 3], ["1", "2", "x"]), ([1.0, None, 3.0], ["a", None, "x"])]
    )
    def test_matches_pandas(self, left, right):
        pd = pytest.importorskip("pandas")
        want = pd.Series(left).compare(pd.Series(right))
        got = _with_index(Series.from_list(left).compare(Series.from_list(right)))
        assert got["index"] == list(want.index)
        assert got["self"] == want["self"].tolist()
        assert got["other"] == want["other"].tolist()


def _typed(values, kind, unit_code, zone=""):
    from dftracer.utils import dftracer_utils_ext as ext

    return ext._series_retype(Series.from_list(values, DType.INT64), int(kind), unit_code, zone)


class TestTemporalUnits:
    def test_duration_units_differ_with_the_same_ints(self):
        s, ms = _typed([1, 2], DType.DURATION, 0), _typed([1, 2], DType.DURATION, 1)
        assert not s.equals(ms)
        assert _with_index(s.compare(ms))["index"] == [0, 1]

    def test_timestamp_units_equal_at_the_same_instants(self):
        us, ms = _typed([1000, 2000], DType.TIMESTAMP, 2), _typed([1, 2], DType.TIMESTAMP, 1)
        assert us.equals(ms)
        assert _with_index(us.compare(ms)) == {"index": [], "self": [], "other": []}
        other = _typed([1, 3], DType.TIMESTAMP, 1)
        assert _with_index(us.compare(other))["index"] == [1]

    def test_aware_differs_from_naive(self):
        naive, aware = _typed([1, 2], DType.TIMESTAMP, 2), _typed([1, 2], DType.TIMESTAMP, 2, "UTC")
        assert not naive.equals(aware)
        assert _with_index(naive.compare(aware))["index"] == [0, 1]

    @pytest.mark.parametrize("kind", [DType.DURATION, DType.TIME64])
    def test_units_below_a_microsecond_are_exact(self, kind):
        ns = _typed([1500, 1000, 2000, 7], kind, 3)
        us = _typed([1, 1, 2, 0], kind, 2)
        assert ns.compare(us).index.to_list() == [0, 3]
        assert us.compare(ns).index.to_list() == [0, 3]
        assert not ns.equals(us)
        assert _typed([1000, 2000], kind, 3).equals(_typed([1, 2], kind, 2))

    def test_scaling_cannot_overflow_into_equal(self):
        big = 2**62
        ns, s = _typed([big], DType.DURATION, 3), _typed([big], DType.DURATION, 0)
        assert not ns.equals(s)
        assert ns.compare(s).index.to_list() == [0]

    def test_unit_mismatch_keeps_nulls(self):
        ns = _typed([1000, None], DType.DURATION, 3)
        us = _typed([1, None], DType.DURATION, 2)
        assert ns.equals(us)
        assert not ns.equals(_typed([1, 2], DType.DURATION, 2))

    def test_same_unit_and_zone_still_native(self):
        a, b = _typed([1, 2], DType.DURATION, 1), _typed([1, 2], DType.DURATION, 1)
        assert a.equals(b)


class TestGroupsEdges:
    def test_a_user_column_named_row(self):
        df = DataFrame({"_row": [2, 1, 2], "v": [1, 2, 3]})
        assert df.groupby("_row").groups == {2: [0, 2], 1: [1]}
        assert DataFrame({"k": [1, 2, 1], "_row": [9, 8, 7]}).groupby("k").groups == {
            1: [0, 2],
            2: [1],
        }

    def test_signed_zero_keys_share_a_group(self):
        df = DataFrame({"k": Series.from_list([0.0, 1.0, -0.0, 0.0])})
        assert df.groupby("k").groups == {0.0: [0, 2, 3], 1.0: [1]}

    def test_bool_keys_are_bool_labels(self):
        df = DataFrame({"k": [True, False, True]})
        groups = df.groupby("k").groups
        assert groups == {True: [0, 2], False: [1]}
        assert all(type(k) is bool for k in groups)

    def test_dropna_keeps_original_positions(self):
        df = DataFrame({"a": ["x", "y", None, "x", None, "z", "y"], "v": list(range(7))})
        assert df.groupby("a").groups == {"x": [0, 3], "y": [1, 6], "z": [5]}
        assert df.groupby("a")[["v"]].groups == {"x": [0, 3], "y": [1, 6], "z": [5]}


class TestNaNParity:
    def test_equals_aligned_nan(self):
        nan = float("nan")
        assert Series.from_list([1.0, nan]).equals(Series.from_list([1.0, nan]))
        assert not Series.from_list([1.0, nan]).equals(Series.from_list([1.0, 2.0]))
        assert not Series.from_list([1.0, nan]).equals(Series.from_list([1.0, None]))
        a = DataFrame({"x": [1.0, nan]})
        assert a.equals(DataFrame({"x": [1.0, nan]}))

    def test_compare_skips_aligned_nan(self):
        nan = float("nan")
        got = Series.from_list([1.0, nan, nan, None]).compare(
            Series.from_list([1.0, nan, 2.0, nan])
        )
        assert got.index.to_list() == [2, 3]
