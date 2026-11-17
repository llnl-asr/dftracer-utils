# SPDX-License-Identifier: MIT
"""Every JIT compile error names the offending file, line and column, then
shows the source line, in one stable form:

    <file>:<line>:<col>: <reason>
    <source line>

Each test marks the offending line with a trailing ``# MARK:<name>`` comment and
asserts the exact position of the construct the JIT rejected.
"""

import copy
import pickle
from dataclasses import dataclass, field
from pathlib import Path
from typing import List

import pytest

from dftracer.utils import jit

_LINES = Path(__file__).read_text(encoding="utf-8").splitlines()


def _check(err: jit.JitError, mark: str, needle: str) -> None:
    line = next(i for i, text in enumerate(_LINES, 1) if text.rstrip().endswith(f"# MARK:{mark}"))
    source = _LINES[line - 1]
    col = source.index(needle) + 1
    assert err.file is not None and Path(err.file).resolve() == Path(__file__).resolve()
    assert (err.line, err.col, err.source) == (line, col, source)
    assert str(err) == f"{err.file}:{line}:{col}: {err.reason}\n{source}"


class TestEachEventBody:
    def test_unsupported_statement(self):
        with pytest.raises(jit.JitError, match="for loop") as ei:

            @jit.plugin
            class Loop:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    for _ in range(2):  # MARK:for
                        self.hits[(e.pid,)] += 1

        _check(ei.value, "for", "for _ in")

    def test_unsupported_expression_points_at_the_expression(self):
        with pytest.raises(jit.JitError, match="conditional expression") as ei:

            @jit.plugin
            class Cond:
                hits = jit.map(key=(jit.i64,), value=jit.sum())

                @jit.each_event
                def step(self, e):
                    self.hits[(e.pid,)] += e.dur if e.tid else 1  # MARK:ifexp

        _check(ei.value, "ifexp", "e.dur if")

    def test_else_branch(self):
        with pytest.raises(jit.JitError, match="else / elif") as ei:

            @jit.plugin
            class Else:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if e.dur > 1:
                        self.hits[(e.pid,)] += 1
                    else:
                        self.hits[(e.pid,)] += 2  # MARK:else

        _check(ei.value, "else", "self.hits[(e.pid,)] += 2")

    def test_unknown_event_field(self):
        with pytest.raises(jit.JitError, match="event field e.nope") as ei:

            @jit.plugin
            class Field:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    self.hits[(e.nope,)] += 1  # MARK:field

        _check(ei.value, "field", "self.hits")

    def test_unknown_map(self):
        with pytest.raises(jit.JitError, match="unknown map self.missing") as ei:

            @jit.plugin
            class Unknown:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    self.missing[(e.pid,)] += 1  # MARK:unknown

        _check(ei.value, "unknown", "self.missing")

    def test_observe_without_by(self):
        with pytest.raises(jit.JitError, match="requires by=") as ei:

            @jit.plugin
            class NoBy:
                hot = jit.map(key=(jit.i64,), value=jit.argmax(of=jit.str_))

                @jit.each_event
                def step(self, e):
                    self.hot[(e.pid,)].observe(e.name)  # MARK:noby

        _check(ei.value, "noby", "self.hot")

    def test_wrong_element_type(self):
        with pytest.raises(jit.JitError, match="str-valued element") as ei:

            @jit.plugin
            class Elem:
                names = jit.map(key=(jit.i64,), value=jit.set())

                @jit.each_event
                def step(self, e):
                    self.names[(e.pid,)].observe(e.dur)  # MARK:elem

        _check(ei.value, "elem", "e.dur")

    def test_integer_literal_out_of_range(self):
        with pytest.raises(jit.JitError, match="does not fit in 64 bits") as ei:

            @jit.plugin
            class Big:
                hits = jit.map(key=(jit.i64,), value=jit.sum())

                @jit.each_event
                def step(self, e):
                    self.hits[(e.pid,)] += e.dur + 100000000000000000000000  # MARK:big

        _check(ei.value, "big", "self.hits")

    def test_error_inside_a_guard_names_the_inner_line(self):
        with pytest.raises(jit.JitError, match="arithmetic operator") as ei:

            @jit.plugin
            class Guarded:
                hits = jit.map(key=(jit.i64,), value=jit.sum())

                @jit.each_event
                def step(self, e):
                    if e.dur > 1:
                        self.hits[(e.pid,)] += e.dur % 3  # MARK:mod

        _check(ei.value, "mod", "e.dur % 3")


class TestDeclarations:
    def test_no_each_event_method_points_at_the_class(self):
        with pytest.raises(jit.JitError, match="exactly one @jit.each_event") as ei:

            @jit.plugin
            class NoEach:  # MARK:noeach
                hits = jit.map(key=(jit.i64,), value=jit.count())

        _check(ei.value, "noeach", "class NoEach")

    def test_string_product_component_points_at_the_map(self):
        with pytest.raises(jit.JitError, match="string-valued reduction") as ei:

            @jit.plugin
            class StrProduct:
                bad = jit.map(key=(jit.i64,), value=(jit.count(), jit.set()))  # MARK:strprod

                @jit.each_event
                def step(self, e):
                    self.bad[(e.pid,), 0] += 1

        _check(ei.value, "strprod", "bad = ")

    def test_raw_body_must_return_a_string(self):
        with pytest.raises(jit.JitError, match="must return a C\\+\\+ body string") as ei:

            @jit.plugin
            class Raw:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event(raw=True)
                def step(self):  # MARK:raw
                    return 123

        _check(ei.value, "raw", "def step")

    def test_map_key_must_be_a_jit_type(self):
        with pytest.raises(jit.JitError, match="map key components") as ei:
            jit.map(key=(1,), value=jit.count())  # MARK:key

        _check(ei.value, "key", "jit.map(")

    def test_map_value_must_be_a_reduction(self):
        with pytest.raises(jit.JitError, match="map value must be") as ei:
            jit.map(key=(jit.i64,), value=3)  # MARK:value

        _check(ei.value, "value", "jit.map(")

    def test_min_of_needs_a_fixed_width_type(self):
        with pytest.raises(jit.JitError, match="fixed-width") as ei:
            jit.min(of=jit.str_)  # MARK:minof

        _check(ei.value, "minof", "jit.min(")

    def test_topk_needs_a_positive_k(self):
        with pytest.raises(jit.JitError, match="positive integer") as ei:
            jit.topk(0)  # MARK:topk

        _check(ei.value, "topk", "jit.topk(")

    def test_quantile_outside_unit_interval(self):
        with pytest.raises(jit.JitError, match="outside") as ei:
            jit.quantiles((1.5,))  # MARK:quant

        _check(ei.value, "quant", "jit.quantiles(")

    def test_consume_needs_a_publish_port(self):
        with pytest.raises(jit.JitError, match="jit.publish attribute") as ei:
            jit.consume(object())  # type: ignore[arg-type]  # MARK:consume

        _check(ei.value, "consume", "jit.consume(")

    def test_package_namespace_is_checked(self):
        with pytest.raises(jit.JitError, match="reserved dftu namespace") as ei:
            jit.JitPackage("dftu")  # MARK:pkg

        _check(ei.value, "pkg", "jit.JitPackage(")


class TestVfoldAndSeries:
    def test_vfold_body_statement_is_located(self):
        with pytest.raises(jit.JitError, match="@jit.vfold body must be") as ei:

            @jit.vfold
            class Bad:
                total = jit.sum()

                @jit.each_batch
                def step(self, df):
                    self.total = df["dur"].sum()  # MARK:vbody

        _check(ei.value, "vbody", "self.total")

    def test_vfold_unknown_accumulator(self):
        with pytest.raises(jit.JitError, match="not a declared accumulator") as ei:

            @jit.vfold
            class Unknown:
                total = jit.sum()

                @jit.each_batch
                def step(self, df):
                    self.other += df["dur"].sum()  # MARK:vunknown

        _check(ei.value, "vunknown", "self.other")

    def test_vfold_without_each_batch_points_at_the_class(self):
        with pytest.raises(jit.JitError, match="exactly one @jit.each_batch") as ei:

            @jit.vfold
            class NoBatch:  # MARK:vnobatch
                total = jit.sum()

        _check(ei.value, "vnobatch", "class NoBatch")

    def test_series_needs_an_argument(self):
        with pytest.raises(jit.JitError, match="at least one column argument") as ei:

            @jit.series(module="locs")  # MARK:series
            def nothing():
                return None

        _check(ei.value, "series", "jit.series(")


class TestOpAndSameNameClasses:
    def test_op_body_error_points_at_the_node(self):
        with pytest.raises(jit.JitError, match="only \\+ - \\* / % are allowed") as ei:

            @jit.op
            def shifted(x: int) -> int:
                return x << 2  # MARK:opbody

        assert type(ei.value).__name__ == "JitOpError"
        _check(ei.value, "opbody", "x << 2")

    def test_op_missing_annotation_points_at_the_function(self):
        with pytest.raises(jit.JitError, match="needs annotations") as ei:

            @jit.op
            def bare(x):  # MARK:opann
                return x

        _check(ei.value, "opann", "def bare")

    def test_same_class_name_twice_locates_the_right_one(self):
        def first():
            @dataclass(frozen=True)
            class Cfg:
                ok: int = 0

            return Cfg

        def second():
            @dataclass(frozen=True)
            class Cfg:
                names: List[str] = field(default_factory=list)  # MARK:samename

            return Cfg

        first()
        cfg = second()
        with pytest.raises(jit.JitError, match="config field 'names'") as ei:

            @jit.plugin
            class Twice:
                config: cfg
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    self.hits[(e.pid,)] += 1

        _check(ei.value, "samename", "names")


class TestCopyAndPickle:
    def _errors(self):
        with pytest.raises(jit.JitError) as plain:
            jit.min(of=jit.str_)  # MARK:copy

        with pytest.raises(jit.JitError) as op:

            @jit.op
            def shifted(x: int) -> int:
                return x << 2

        return [plain.value, op.value]

    def test_copy_and_pickle_keep_one_location(self):
        for err in self._errors():
            assert err.file is not None
            for clone in (copy.copy(err), pickle.loads(pickle.dumps(err))):
                assert type(clone) is type(err)
                assert str(clone) == str(err)
                assert (clone.file, clone.line, clone.col, clone.source, clone.reason) == (
                    err.file,
                    err.line,
                    err.col,
                    err.source,
                    err.reason,
                )
                assert str(clone).count(err.reason) == 1
