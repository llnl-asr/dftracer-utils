#!/usr/bin/env python3
"""@jit.op: author a compose op in Python and run it two ways - standalone in
plain Python (JIT-compiled, driven on a standalone compose host) and inlined
into a @jit.each_event plugin body."""

import collections
import gzip
import math
import os
import shutil

import pytest

from dftracer.utils import jit, jit_op
from dftracer.utils.jit_op import JitOpError
from dftracer.utils.plugins import Plugins

_HAS_CXX = bool(shutil.which("c++") or shutil.which("clang++") or shutil.which("g++"))
_needs_cxx = pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler for the jit backend")


@jit.op
def double(x: jit.i64) -> jit.i64:
    return x * 2


@jit.op
def plus10(x: jit.i64) -> jit.i64:
    return x + 10


@jit.op
def half(x: jit.f64) -> jit.f64:
    return x / 2


@_needs_cxx
class TestStandalone:
    def test_single_op(self):
        assert double(5) == 10

    def test_piped_ops(self):
        assert (double | plus10)(5) == 20

    def test_nested_call(self):
        assert plus10(double(5)) == 20

    def test_float_op(self):
        assert half(9.0) == 4.5


class TestAuthoring:
    def test_pipe_type_mismatch_raises(self):
        with pytest.raises(JitOpError):
            _ = double | half  # i64 -> f64 does not chain

    def test_unknown_name_in_body_raises(self):
        with pytest.raises(JitOpError):

            @jit.op
            def bad(x: jit.i64) -> jit.i64:
                return x + y  # noqa: F821 - y is not the input

    def test_non_return_body_raises(self):
        with pytest.raises(JitOpError):

            @jit.op
            def bad(x: jit.i64) -> jit.i64:
                x += 1
                return x


@_needs_cxx
class TestInlineInPlugin:
    def test_op_inlined_into_each_event(self, tmp_path):
        tr = os.path.join(str(tmp_path), "t.pfw.gz")
        n = 250
        with gzip.open(tr, "wt", encoding="utf-8") as f:
            for i in range(n):
                f.write(
                    f'{{"name":"read","cat":"POSIX","pid":1,"tid":1,'
                    f'"ts":{1000 + i},"dur":{i},"ph":"X","args":{{}}}}\n'
                )

        pa = pytest.importorskip("pyarrow")

        @jit.op
        def bucket(x: jit.i64) -> jit.i64:
            return x / 100 * 100

        @jit.plugin
        class WithOp:
            counts = jit.map(key=jit.i64, value=jit.count())

            @jit.each_event
            def step(self, e):
                self.counts[(bucket(e.dur),)] += 1

        # The op lands as a static C function and is called from on_batch.
        src = WithOp._jit_plugin.source
        assert "static int64_t dftu_jitop_bucket" in src
        assert "dftu_jitop_bucket(" in src.split("on_batch")[1]

        plugins = Plugins([WithOp])
        table = pa.table(plugins.run([tr]).results["counts"])
        got = dict(zip(table.column("k0").to_pylist(), table.column("value").to_pylist()))
        ref = collections.Counter((i // 100) * 100 for i in range(n))
        assert got == dict(ref)


@jit.op
def triple(x: jit.i64) -> jit.i64:
    return x * 3 - 1


@jit.op
def scale_half(x: jit.i64) -> jit.f64:
    return x * 0.5


@jit.op
def abs_offset(x: jit.i64) -> jit.i64:
    return abs(x - 100)


@jit.op
def mod3(x: jit.i64) -> jit.i64:
    return x % 3


@jit.op
def bucket100(x: jit.i64) -> jit.i64:
    return x / 100 * 100


@jit.op
def recip(x: jit.f64) -> jit.f64:
    return 1.0 / x


@jit.op
def div_by_zero(x: jit.f64) -> jit.f64:
    return x / 0.0


@jit.op
def add_self(x: jit.i64) -> jit.i64:
    return x + x


@_needs_cxx
class TestFusedExprLowering:
    """@jit.op over an array: the fused columnar Expr path vs. the existing
    per-element compose path, and the inspectable fuse/fallback decision."""

    def test_fuses_pure_arithmetic(self):
        assert jit_op.can_fuse(triple)

    def test_fuses_float_literal_promotion(self):
        assert jit_op.can_fuse(scale_half)

    def test_fuses_abs_intrinsic(self):
        assert jit_op.can_fuse(abs_offset)

    def test_falls_back_on_mod(self):
        # BinaryOp has no Mod code; % never lowers.
        assert not jit_op.can_fuse(mod3)

    def test_falls_back_on_integer_division(self):
        # C truncates x/100 before the *100; Expr's Div always promotes to
        # Float64 first, so fusing this would silently change the bucketing.
        assert not jit_op.can_fuse(bucket100)

    def test_equivalence_arithmetic(self):
        values = list(range(-20, 20))
        fused, fused_ran = jit_op.run_op_array(triple, values)
        scalar, scalar_ran = jit_op.run_op_array(triple, values, force_scalar=True)
        assert fused_ran and not scalar_ran
        assert fused == scalar

    def test_equivalence_mixed_int_float(self):
        values = list(range(-10, 10))
        fused, fused_ran = jit_op.run_op_array(scale_half, values)
        scalar, scalar_ran = jit_op.run_op_array(scale_half, values, force_scalar=True)
        assert fused_ran and not scalar_ran
        assert fused == scalar

    def test_equivalence_math_intrinsic(self):
        values = list(range(0, 200, 7))
        fused, fused_ran = jit_op.run_op_array(abs_offset, values)
        scalar, scalar_ran = jit_op.run_op_array(abs_offset, values, force_scalar=True)
        assert fused_ran and not scalar_ran
        assert fused == scalar

    def test_equivalence_with_null(self):
        values = [1, None, 3, None, -5]
        fused, fused_ran = jit_op.run_op_array(triple, values)
        scalar, scalar_ran = jit_op.run_op_array(triple, values, force_scalar=True)
        assert fused_ran and not scalar_ran
        assert fused == scalar
        assert fused[1] is None and fused[3] is None

    def test_falls_back_on_scalar_minus_column(self):
        # `1.0 / x`: the engine has a column-scalar Div kernel, not scalar-
        # column (Add/Mul commute to it; Sub/Div do not).
        assert not jit_op.can_fuse(recip)

    def test_fallback_is_correct_for_unfusable_op(self):
        values = list(range(-5, 15))
        result, fused = jit_op.run_op_array(mod3, values)
        assert not fused
        # C truncating % (sign follows the dividend), not Python's floored %.
        assert result == [int(math.fmod(v, 3)) for v in values]

        result, fused = jit_op.run_op_array(bucket100, [0, 99, 100, 250, 999])
        assert not fused
        assert result == [0, 0, 100, 200, 900]

        result, fused = jit_op.run_op_array(recip, [2.0, -2.0, 0.0])
        assert not fused
        assert result == [0.5, -0.5, math.inf]

    def test_edge_integer_overflow_matches(self):
        values = [2**62, 2**63 - 1, -(2**63), -1, 0]
        fused, fused_ran = jit_op.run_op_array(add_self, values)
        scalar, scalar_ran = jit_op.run_op_array(add_self, values, force_scalar=True)
        assert fused_ran and not scalar_ran
        assert fused == scalar

    def test_edge_float_division_by_zero_matches(self):
        values = [2.0, -2.0, 0.0]
        fused, fused_ran = jit_op.run_op_array(div_by_zero, values)
        scalar, scalar_ran = jit_op.run_op_array(div_by_zero, values, force_scalar=True)
        assert fused_ran and not scalar_ran
        for f, s in zip(fused, scalar):
            if math.isnan(f):
                assert math.isnan(s)
            else:
                assert f == s
        assert fused[0] == math.inf and fused[1] == -math.inf and math.isnan(fused[2])

    def test_force_scalar_is_sensitive(self):
        _, fused = jit_op.run_op_array(triple, [1, 2, 3])
        assert fused is True
        _, fused = jit_op.run_op_array(triple, [1, 2, 3], force_scalar=True)
        assert fused is False


@jit.op
def is_posix(x: jit.str_) -> jit.i64:
    return x == "POSIX"


@jit.op
def not_posix(x: jit.str_) -> jit.i64:
    return x != "POSIX"


class TestStrOps:
    """DFTU_T_STR (an interned-string id) supports only == and != against a
    string literal, never arithmetic or ordering - id order is intern order,
    not string order. DFTU_T_BYTES is not exposed at all: it crosses the
    compose ABI as an opaque, variable-length record, not a fixed-size value
    this DSL can compare."""

    @_needs_cxx
    def test_str_equality_produces_correct_result(self):
        # jit_run_op builds a fresh compose host per call and interns "POSIX"
        # (this op's only literal) before running, so it always lands at id 0.
        assert jit_op.run_op(is_posix, 0) == 1
        assert jit_op.run_op(is_posix, 1) == 0

    @_needs_cxx
    def test_str_inequality_produces_correct_result(self):
        assert jit_op.run_op(not_posix, 0) == 0
        assert jit_op.run_op(not_posix, 1) == 1

    def test_ordering_on_str_refused_naming_operator_and_type(self):
        with pytest.raises(JitOpError, match=r"'<'.*DFTU_T_STR"):

            @jit.op
            def bad(x: jit.str_) -> jit.i64:
                return x < "POSIX"

    def test_arithmetic_on_str_refused_naming_operator_and_type(self):
        with pytest.raises(JitOpError, match=r"'\+'.*DFTU_T_STR"):

            @jit.op
            def bad(x: jit.str_) -> jit.i64:
                return x + x

    def test_comparison_on_numeric_type_refused_naming_operator_and_type(self):
        with pytest.raises(JitOpError, match=r"'=='.*DFTU_T_I64"):

            @jit.op
            def bad(x: jit.i64) -> jit.i64:
                return x == 1

    def test_str_comparison_must_return_integer(self):
        with pytest.raises(JitOpError):

            @jit.op
            def bad(x: jit.str_) -> jit.f64:
                return x == "POSIX"

    def test_str_op_never_fuses(self):
        # No Eq/NotEq opcode exists in the columnar Expr AST this module
        # emits (_EXPR_BIN is arithmetic only) and DFTU_T_STR is not in
        # _FUSABLE_TYPE_ID, so a str op always takes the scalar path.
        assert not jit_op.can_fuse(is_posix)

    @_needs_cxx
    def test_str_op_array_scalar_path_is_correct(self):
        result, fused = jit_op.run_op_array(is_posix, [0, 1, 0])
        assert not fused
        assert result == [1, 0, 1]
