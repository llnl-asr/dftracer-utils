"""The op registry exposed to Python (dftracer.utils.jit.ops): discovery and one
generic runner that marshals Python args per an op's signature.

A built-in's registry key (what run()/get()/info()/names() take and return) is
the full `dftu.series.<op>` form; the attribute path (`ops.series.<op>`) elides
the redundant `dftu.` since `ops` is already that namespace."""

import numpy as np
import pytest

from dftracer.utils.jit import ops
from dftracer.utils.series import Series


def _col(vals):
    return Series.from_numpy(np.array(vals, dtype=np.int64))


def _vals(series):
    return np.asarray(series).tolist()


class TestDiscovery:
    def test_info_kind_covers_all_three(self):
        """kind must distinguish frame ops, not fold them into aggregate.

        dftu_op_kind_of has three values; a two-way ternary reported every
        dftu.frame.* op as "aggregate".
        """
        assert ops.info("dftu.series.add")["kind"] == "series"
        frame_ops = [n for n in ops.list() if n.startswith("dftu.frame.")]
        assert frame_ops, "expected registered dftu.frame.* ops"
        kinds = set()
        for n in frame_ops:
            info = ops.info(n)
            # kind is the RETURN token, not the name prefix: a frame -> series
            # op (mask, is_unique) is kind series while still being a
            # DataFrame method, and dftu.frame.value_counts is a series ->
            # frame op.
            assert info["kind"] == info["signature"].rsplit("-> ", 1)[1], n
            kinds.add(info["kind"])
        assert "frame" in kinds

    def test_list_covers_the_surface(self):
        names = ops.list()
        for n in (
            "dftu.series.add",
            "dftu.series.sub",
            "dftu.series.mul",
            "dftu.series.div",
            "dftu.series.add_scalar",
            "dftu.series.compare",
            "dftu.series.cast",
            "dftu.series.count",
            "dftu.series.reduce",
            "dftu.series.str_contains",
            "dftu.series.mode",
        ):
            assert n in names

    def test_bare_name_no_longer_registered(self):
        for n in ("add", "sub", "mul", "count", "head", "select"):
            assert n not in ops.list()

    def test_info(self):
        info = ops.info("dftu.series.add")
        assert info["kind"] == "series"
        assert info["arity"] == 2
        assert info["signature"] == "(series, series) -> series"

        red = ops.info("dftu.series.count")
        assert red["kind"] == "aggregate"

    def test_unknown_op(self):
        with pytest.raises(KeyError):
            ops.info("no_such_op")
        with pytest.raises(AttributeError):
            _ = ops.definitely_not_an_op


class TestAttributePathElision:
    def test_elided_path_resolves_the_built_in(self):
        assert _vals(ops.series.add(_col([1, 2, 3]), _col([10, 20, 30]))) == [11, 22, 33]

    def test_un_elided_path_does_not_resolve(self):
        with pytest.raises(AttributeError):
            ops.dftu


class TestRun:
    def test_binary_as_nested_attribute(self):
        out = ops.series.add(_col([1, 2, 3]), _col([10, 20, 30]))
        assert isinstance(out, Series)
        assert _vals(out) == [11, 22, 33]

    def test_run_by_name(self):
        assert _vals(ops.run("dftu.series.mul", _col([1, 2, 3]), _col([4, 5, 6]))) == [
            4,
            10,
            18,
        ]

    def test_get_bound_callable(self):
        sub = ops.get("dftu.series.sub")
        assert _vals(sub(_col([5, 7]), _col([1, 2]))) == [4, 5]

    def test_scalar_operand(self):
        assert _vals(ops.series.add_scalar(_col([1, 2, 3]), 100)) == [101, 102, 103]

    def test_reducer_returns_scalar(self):
        col = _col([1, 5, 2])
        assert ops.series.count(col) == 3
        assert ops.series.arg_max(col) == 1

    def test_missing_argument_raises(self):
        with pytest.raises(TypeError):
            ops.series.add(_col([1, 2]))  # add needs two columns


class TestExtendedSurface:
    def test_unary_and_cumulative(self):
        assert _vals(ops.series.abs(_col([1, -2, 3, -4]))) == [1, 2, 3, 4]
        assert _vals(ops.run("dftu.series.cumsum", _col([1, 2, 3, 4]))) == [1, 3, 6, 10]

    def test_two_scalar_op_clip(self):
        assert _vals(ops.series.clip(_col([1, 5, 9]), 2, 8)) == [2, 5, 8]

    def test_f64_reducer_with_flag_and_operand(self):
        var = ops.run("dftu.series.variance", _col([1, 2, 3, 4]), 1)  # i32 sample flag
        assert abs(var - 5.0 / 3.0) < 1e-9
        q = ops.run("dftu.series.quantile", _col([1, 2, 3, 4]), 0.5)  # f64 operand
        assert isinstance(q, float)

    def test_u64_scalar_operand_above_int64_max(self):
        big = 2**63 + 5  # only representable as u64
        col = Series.from_numpy(np.array([10, 20], dtype=np.uint64))
        out = ops.series.add_scalar(col, big)  # must not raise OverflowError
        assert np.asarray(out).tolist()[0] == 10 + big
