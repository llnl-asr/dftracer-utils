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
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, FrozenSet, List, Set, Tuple

import pytest

from dftracer.utils import Series, jit

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


class TestPlainState:
    def test_plain_dict_plus_equals_is_a_key_error(self):
        with pytest.raises(jit.JitError, match="raises KeyError for a missing key") as ei:

            @jit.plugin
            class PlusEquals:
                counts: Dict[int, int] = {}

                @jit.each_event
                def step(self, e):
                    self.counts[e.pid] += 1  # MARK:plus_equals

        _check(ei.value, "plus_equals", "self.counts")

    def test_plain_dict_max_of_index_is_a_key_error(self):
        with pytest.raises(jit.JitError, match="raises KeyError for a missing key") as ei:

            @jit.plugin
            class IndexMax:
                top: Dict[int, int] = {}

                @jit.each_event
                def step(self, e):
                    self.top[e.pid] = max(self.top[e.pid], e.dur)  # MARK:index_max

        _check(ei.value, "index_max", "self.top[e.pid], e.dur")

    def test_plain_assignment_has_no_inferable_merge(self):
        with pytest.raises(jit.JitError, match="cannot infer how worker slices merge") as ei:

            @jit.plugin
            class Assign:
                last: Dict[int, int] = defaultdict(int)

                @jit.each_event
                def step(self, e):
                    self.last[e.pid] = e.dur  # MARK:assign

        _check(ei.value, "assign", "self.last")

    def test_scalar_assignment_has_no_inferable_merge(self):
        with pytest.raises(jit.JitError, match="n \\+= x") as ei:

            @jit.plugin
            class ScalarAssign:
                n: int = 0

                @jit.each_event
                def step(self, e):
                    self.n = e.dur  # MARK:scalar_assign

        _check(ei.value, "scalar_assign", "self.n")

    def test_other_augmented_operator(self):
        with pytest.raises(jit.JitError, match="operator other than") as ei:

            @jit.plugin
            class Minus:
                n: int = 0

                @jit.each_event
                def step(self, e):
                    self.n -= 1  # MARK:minus

        _check(ei.value, "minus", "self.n")

    def test_dict_method_call(self):
        with pytest.raises(jit.JitError, match="cannot infer how worker slices merge") as ei:

            @jit.plugin
            class Pop:
                d: Dict[int, int] = defaultdict(int)

                @jit.each_event
                def step(self, e):
                    self.d[e.pid] += 1
                    self.d.pop(e.pid)  # MARK:pop

        _check(ei.value, "pop", "self.d.pop")

    def test_set_method_other_than_add(self):
        with pytest.raises(jit.JitError, match="s.add\\(x\\)") as ei:

            @jit.plugin
            class Discard:
                s: Set[int] = set()

                @jit.each_event
                def step(self, e):
                    self.s.add(e.pid)
                    self.s.discard(e.tid)  # MARK:discard

        _check(ei.value, "discard", "self.s.discard")

    def test_list_method_other_than_append(self):
        with pytest.raises(jit.JitError, match="lst.append\\(x\\)") as ei:

            @jit.plugin
            class Extend:
                seq: List[int] = []

                @jit.each_event
                def step(self, e):
                    self.seq.extend(e.pid)  # MARK:extend

        _check(ei.value, "extend", "self.seq.extend")

    def test_state_cannot_be_read(self):
        with pytest.raises(jit.JitError, match="write-only in the body") as ei:

            @jit.plugin
            class ReadBack:
                n: int = 0
                hits: Dict[int, int] = defaultdict(int)

                @jit.each_event
                def step(self, e):
                    self.n += 1
                    self.hits[e.pid] += self.n  # MARK:read_back

        _check(ei.value, "read_back", "self.n")

    def test_one_field_cannot_have_two_merges(self):
        with pytest.raises(jit.JitError, match="a field has one merge") as ei:

            @jit.plugin
            class Two:
                d: Dict[int, int] = defaultdict(int)

                @jit.each_event
                def step(self, e):
                    self.d[e.pid] += 1
                    self.d[e.pid] = max(self.d[e.pid], e.dur)  # MARK:two

        _check(ei.value, "two", "self.d[e.pid] = max")

    def test_field_never_updated(self):
        with pytest.raises(jit.JitError, match="never updated") as ei:

            @jit.plugin
            class Unused:
                n: int = 0
                idle: int = 0  # MARK:unused

                @jit.each_event
                def step(self, e):
                    self.n += 1

        _check(ei.value, "unused", "idle")

    def test_nonempty_default(self):
        with pytest.raises(jit.JitError, match="starts empty|must start") as ei:

            @jit.plugin
            class Seeded:
                d: Dict[int, int] = {1: 2}  # MARK:seeded

                @jit.each_event
                def step(self, e):
                    self.d[e.pid] = self.d.get(e.pid, 0) + 1

        _check(ei.value, "seeded", "d: Dict")

    def test_nonzero_scalar_start(self):
        with pytest.raises(jit.JitError, match="must start at 0") as ei:

            @jit.plugin
            class Started:
                n: int = 5  # MARK:started

                @jit.each_event
                def step(self, e):
                    self.n += 1

        _check(ei.value, "started", "n: int")

    def test_defaultdict_factory_must_match_the_value_type(self):
        with pytest.raises(jit.JitError, match="defaultdict\\(float\\)") as ei:

            @jit.plugin
            class Factory:
                d: Dict[int, float] = defaultdict(int)  # MARK:factory

                @jit.each_event
                def step(self, e):
                    self.d[e.pid] += 1.5

        _check(ei.value, "factory", "d: Dict")

    def test_unsupported_key_type(self):
        with pytest.raises(jit.JitError, match="needs Dict\\[K, V\\]") as ei:

            @jit.plugin
            class FloatKey:
                d: Dict[float, int] = {}  # MARK:float_key

                @jit.each_event
                def step(self, e):
                    self.d[e.pid] = self.d.get(e.pid, 0) + 1

        _check(ei.value, "float_key", "d: Dict")

    def test_unsupported_container(self):
        with pytest.raises(jit.JitError, match="is not scan state") as ei:

            @jit.plugin
            class Frozen:
                f: FrozenSet[int] = frozenset()  # MARK:frozen

                @jit.each_event
                def step(self, e):
                    self.f.add(e.pid)

        _check(ei.value, "frozen", "f: FrozenSet")

    def test_key_family_mismatch(self):
        with pytest.raises(jit.JitError, match="is a number") as ei:

            @jit.plugin
            class WrongKey:
                d: Dict[int, int] = defaultdict(int)

                @jit.each_event
                def step(self, e):
                    self.d[e.name] += 1  # MARK:wrong_key

        _check(ei.value, "wrong_key", "e.name")

    def test_element_family_mismatch(self):
        with pytest.raises(jit.JitError, match="is a str") as ei:

            @jit.plugin
            class WrongElem:
                s: Set[str] = set()

                @jit.each_event
                def step(self, e):
                    self.s.add(e.pid)  # MARK:wrong_elem

        _check(ei.value, "wrong_elem", "e.pid")

    def test_key_arity(self):
        with pytest.raises(jit.JitError, match="takes 2 key components") as ei:

            @jit.plugin
            class Arity:
                d: Dict[Tuple[int, int], int] = defaultdict(int)

                @jit.each_event
                def step(self, e):
                    self.d[e.pid] += 1  # MARK:arity

        _check(ei.value, "arity", "self.d[e.pid]")

    def test_raw_body_cannot_use_plain_state(self):
        with pytest.raises(jit.JitError, match="AST-compiled") as ei:

            @jit.plugin
            class Raw:  # MARK:raw_plain
                n: int = 0

                @jit.each_event(raw=True)
                def step(self):
                    return "int x = 0; (void)x;"

        _check(ei.value, "raw_plain", "class Raw")

    def test_series_cannot_be_kept_on_self(self):
        with pytest.raises(jit.JitError, match="batch value is valid only inside one step") as ei:

            @jit.plugin
            class Escape:
                col: Series  # MARK:escape
                n: int = 0

                @jit.each_event
                def step(self, e):
                    self.n += 1

        _check(ei.value, "escape", "col:")


class TestStringAsNumber:
    def test_scalar_sum(self):
        with pytest.raises(jit.JitError, match="string field") as ei:

            @jit.plugin
            class ScalarStr:
                n: int = 0

                @jit.each_event
                def step(self, e):
                    self.n += e.name  # MARK:num_scalar

        _check(ei.value, "num_scalar", "e.name")

    def test_dict_sum(self):
        with pytest.raises(jit.JitError, match="string field") as ei:

            @jit.plugin
            class DictStr:
                d: Dict[int, int] = defaultdict(int)

                @jit.each_event
                def step(self, e):
                    self.d[e.pid] += e.cat  # MARK:num_dict

        _check(ei.value, "num_dict", "e.cat")

    def test_max_of_a_string(self):
        with pytest.raises(jit.JitError, match="string field") as ei:

            @jit.plugin
            class MaxStr:
                d: Dict[int, int] = defaultdict(int)

                @jit.each_event
                def step(self, e):
                    self.d[e.pid] = max(self.d[e.pid], e.name)  # MARK:num_max

        _check(ei.value, "num_max", "e.name)")

    def test_get_sum_operand(self):
        with pytest.raises(jit.JitError, match="string field") as ei:

            @jit.plugin
            class GetStr:
                d: Dict[int, int] = {}

                @jit.each_event
                def step(self, e):
                    self.d[e.pid] = self.d.get(e.pid, 0) + e.name  # MARK:num_get

        _check(ei.value, "num_get", "e.name")

    def test_descriptor_sum(self):
        with pytest.raises(jit.JitError, match="string field") as ei:

            @jit.plugin
            class MapStr:
                m = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    self.m[(e.pid,)] += e.name  # MARK:num_map

        _check(ei.value, "num_map", "e.name")

    def test_descriptor_max(self):
        with pytest.raises(jit.JitError, match="string field") as ei:

            @jit.plugin
            class MapMaxStr:
                m = jit.map(key=(jit.i64,), value=jit.max())

                @jit.each_event
                def step(self, e):
                    self.m[(e.pid,)].observe(e.name)  # MARK:num_mapmax

        _check(ei.value, "num_mapmax", "e.name")

    def test_store_put_value(self):
        with pytest.raises(jit.JitError, match="string field") as ei:

            @jit.plugin
            class PutStr:
                r: jit.Store[int, int]

                @jit.each_event
                def step(self, e):
                    self.r.put(e.pid, e.name)  # MARK:num_put

        _check(ei.value, "num_put", "e.name")

    def test_store_get_default(self):
        with pytest.raises(jit.JitError, match="string field") as ei:

            @jit.plugin
            class GetDefaultStr:
                r: jit.Store[int, int]
                n: int = 0

                @jit.each_event
                def step(self, e):
                    self.n += self.r.get(e.pid, e.name)  # MARK:num_getdef

        _check(ei.value, "num_getdef", "e.name")

    def test_constant_on_the_class_is_not_state(self):
        with pytest.raises(jit.JitError, match="use ClassVar\\[int\\] for a constant") as ei:

            @jit.plugin
            class Limit:
                LIMIT: int = 100  # MARK:limit
                n: int = 0

                @jit.each_event
                def step(self, e):
                    self.n += 1

        _check(ei.value, "limit", "LIMIT:")

    def test_reading_state_into_a_local(self):
        with pytest.raises(jit.JitError, match="write-only in the body") as ei:

            @jit.plugin
            class Local:
                d: Dict[int, int] = {}

                @jit.each_event
                def step(self, e):
                    x = self.d.get(e.pid, 0)  # MARK:local
                    self.d[e.pid] = self.d.get(e.pid, 0) + x

        _check(ei.value, "local", "self.d.get")


class TestStoreErrors:
    def test_store_has_no_default(self):
        with pytest.raises(jit.JitError, match="annotation only") as ei:

            @jit.plugin
            class Default:
                seen: jit.Store[int, int] = None  # type: ignore[assignment]  # MARK:default

                @jit.each_event
                def step(self, e):
                    self.seen.put(e.pid, 1)

        _check(ei.value, "default", "seen:")

    def test_store_value_type(self):
        with pytest.raises(jit.JitError, match="V an int or a float") as ei:

            @jit.plugin
            class StrValue:
                seen: jit.Store[int, str]  # MARK:str_value

                @jit.each_event
                def step(self, e):
                    self.seen.put(e.pid, e.name)

        _check(ei.value, "str_value", "seen:")

    def test_store_cannot_hold_a_series(self):
        with pytest.raises(jit.JitError, match="batch value is valid only inside one step") as ei:

            @jit.plugin
            class SeriesValue:
                seen: jit.Store[int, Series]  # MARK:series_value

                @jit.each_event
                def step(self, e):
                    self.seen.get(e.pid, 0)

        _check(ei.value, "series_value", "seen:")

    def test_store_name_is_a_namespace(self):
        with pytest.raises(jit.JitError, match="lowercase ASCII") as ei:

            @jit.plugin
            class Upper:
                Seen: jit.Store[int, int]  # MARK:upper

                @jit.each_event
                def step(self, e):
                    self.Seen.put(e.pid, 1)

        _check(ei.value, "upper", "Seen:")

    def test_store_get_is_a_value(self):
        with pytest.raises(jit.JitError, match="use put\\(key, value\\)") as ei:

            @jit.plugin
            class GetStatement:
                seen: jit.Store[int, int]

                @jit.each_event
                def step(self, e):
                    self.seen.get(e.pid, 0)  # MARK:get_statement

        _check(ei.value, "get_statement", "self.seen.get")

    def test_store_put_is_a_statement(self):
        with pytest.raises(jit.JitError, match="is a statement, not a value") as ei:

            @jit.plugin
            class PutValue:
                seen: jit.Store[int, int]
                total: int = 0

                @jit.each_event
                def step(self, e):
                    self.total += self.seen.put(e.pid, 1)  # MARK:put_value

        _check(ei.value, "put_value", "self.seen.put")

    def test_store_unknown_method(self):
        with pytest.raises(jit.JitError, match="store method .erase") as ei:

            @jit.plugin
            class Erase:
                seen: jit.Store[int, int]

                @jit.each_event
                def step(self, e):
                    self.seen.erase(e.pid)  # MARK:erase

        _check(ei.value, "erase", "self.seen.erase")

    def test_store_get_needs_a_default(self):
        with pytest.raises(jit.JitError, match="takes a key and a default") as ei:

            @jit.plugin
            class NoDefault:
                seen: jit.Store[int, int]
                total: int = 0

                @jit.each_event
                def step(self, e):
                    self.total += self.seen.get(e.pid)  # MARK:no_default

        _check(ei.value, "no_default", "self.seen.get")

    def test_store_key_arity(self):
        with pytest.raises(jit.JitError, match="takes 2 key components") as ei:

            @jit.plugin
            class StoreArity:
                seen: jit.Store[Tuple[int, str], int]

                @jit.each_event
                def step(self, e):
                    self.seen.put(e.pid, 1)  # MARK:store_arity

        _check(ei.value, "store_arity", "e.pid")

    def test_store_key_family(self):
        with pytest.raises(jit.JitError, match="is a str") as ei:

            @jit.plugin
            class StoreKey:
                seen: jit.Store[str, int]

                @jit.each_event
                def step(self, e):
                    self.seen.put(e.pid, 1)  # MARK:store_key

        _check(ei.value, "store_key", "e.pid")

    def test_store_float_into_int_value(self):
        with pytest.raises(jit.JitError, match="float value") as ei:

            @jit.plugin
            class FloatValue:
                seen: jit.Store[int, int]

                @jit.each_event
                def step(self, e):
                    self.seen.put(e.pid, e.dur * 0.5)  # MARK:float_value

        _check(ei.value, "float_value", "e.dur * 0.5")

    def test_store_cannot_be_read_bare(self):
        with pytest.raises(jit.JitError, match="use self.seen.get") as ei:

            @jit.plugin
            class Bare:
                seen: jit.Store[int, int]
                total: int = 0

                @jit.each_event
                def step(self, e):
                    self.total += self.seen  # MARK:bare

        _check(ei.value, "bare", "self.seen")


class TestConditions:
    def test_walrus(self):
        with pytest.raises(jit.JitError, match="assignment expression") as ei:

            @jit.plugin
            class Walrus:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if (d := e.dur) > 3:  # MARK:walrus
                        self.hits[(e.pid,)] += d

        _check(ei.value, "walrus", "d := e.dur")

    def test_function_call_in_a_condition(self):
        with pytest.raises(jit.JitError, match="function call") as ei:

            @jit.plugin
            class Call:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if len(e.name) > 3:  # MARK:call
                        self.hits[(e.pid,)] += 1

        _check(ei.value, "call", "len(e.name)")

    def test_string_compared_with_a_number(self):
        with pytest.raises(jit.JitError, match="string and a number") as ei:

            @jit.plugin
            class Mixed:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if e.cat == 3:  # MARK:mixed
                        self.hits[(e.pid,)] += 1

        _check(ei.value, "mixed", "e.cat")

    def test_number_compared_with_a_string_in_a_chain(self):
        with pytest.raises(jit.JitError, match="string") as ei:

            @jit.plugin
            class Chained:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if 1 < e.dur < "x":  # MARK:chain
                        self.hits[(e.pid,)] += 1

        _check(ei.value, "chain", "e.dur")

    def test_is_none_on_a_number(self):
        with pytest.raises(jit.JitError, match="is None") as ei:

            @jit.plugin
            class NumNone:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if e.dur is None:  # MARK:isnone
                        self.hits[(e.pid,)] += 1

        _check(ei.value, "isnone", "e.dur")

    def test_is_with_something_other_than_none(self):
        with pytest.raises(jit.JitError, match="is None") as ei:

            @jit.plugin
            class IsOther:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if e.fhash is e.name:  # MARK:isother
                        self.hits[(e.pid,)] += 1

        _check(ei.value, "isother", "e.fhash is e.name")

    def test_in_a_non_literal(self):
        with pytest.raises(jit.JitError, match="literal tuple") as ei:

            @jit.plugin
            class InName:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if e.pid in e.tid:  # MARK:innonlit
                        self.hits[(e.pid,)] += 1

        _check(ei.value, "innonlit", "e.tid")

    def test_in_with_a_computed_element(self):
        with pytest.raises(jit.JitError, match="takes only literals") as ei:

            @jit.plugin
            class InExpr:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if e.pid in (1, e.tid):  # MARK:inexpr
                        self.hits[(e.pid,)] += 1

        _check(ei.value, "inexpr", "(1, e.tid)")

    def test_ordered_string_comparison(self):
        with pytest.raises(jit.JitError, match="other than == / !="):

            @jit.plugin
            class Ordered:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if e.cat < "Z":
                        self.hits[(e.pid,)] += 1

    def test_string_as_a_truth_value(self):
        with pytest.raises(jit.JitError, match="not a truth value") as ei:

            @jit.plugin
            class Truth:
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if e.name:  # MARK:truth
                        self.hits[(e.pid,)] += 1

        _check(ei.value, "truth", "e.name")

    def test_is_none_on_a_required_config_field(self):
        @dataclass(frozen=True)
        class Cfg:
            cap: int = 3

        with pytest.raises(jit.JitError, match="not optional") as ei:

            @jit.plugin
            class Required:
                config: Cfg
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if self.config.cap is None:  # MARK:required
                        self.hits[(e.pid,)] += 1

        _check(ei.value, "required", "self.config.cap")


class TestBatchHook:
    def test_series_stored_on_self(self):
        with pytest.raises(jit.JitError, match="stored on self escapes") as ei:

            @jit.plugin
            class Stored:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.saved = df["dur"]  # MARK:b_stored
                    self.total += df["dur"].sum()

        _check(ei.value, "b_stored", "self.saved")

    def test_series_returned(self):
        with pytest.raises(jit.JitError, match="cannot be returned") as ei:

            @jit.plugin
            class Returned:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += df["dur"].sum()
                    return df["dur"]  # MARK:b_returned

        _check(ei.value, "b_returned", "return df")

    def test_series_folded_without_a_reduction(self):
        with pytest.raises(jit.JitError, match="reduce it first") as ei:

            @jit.plugin
            class Unreduced:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += df["dur"]  # MARK:b_unreduced

        _check(ei.value, "b_unreduced", 'df["dur"]')

    def test_method_outside_the_vocabulary(self):
        with pytest.raises(jit.JitError, match=r"\.std\(\) is outside") as ei:

            @jit.plugin
            class Std:
                total: float = 0.0

                @jit.each_batch
                def step(self, df):
                    self.total += df["dur"].std()  # MARK:b_std

        _check(ei.value, "b_std", 'df["dur"].std()')

    def test_function_outside_the_vocabulary(self):
        with pytest.raises(jit.JitError, match=r"call to len\(\)") as ei:

            @jit.plugin
            class Len:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += len(df["dur"])  # MARK:b_len

        _check(ei.value, "b_len", 'len(df["dur"])')

    def test_string_column(self):
        with pytest.raises(jit.JitError, match="is a string") as ei:

            @jit.plugin
            class Text:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += df["name"].count()  # MARK:b_text

        _check(ei.value, "b_text", 'df["name"]')

    def test_and_between_columns(self):
        with pytest.raises(jit.JitError, match="write .a. & .b."):

            @jit.plugin
            class AndOp:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += ((df["dur"] > 1) and (df["pid"] > 1)).sum()

    def test_not_on_a_column(self):
        with pytest.raises(jit.JitError, match="write ~"):

            @jit.plugin
            class NotOp:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += (not (df["dur"] > 1)).sum()

    def test_control_flow(self):
        with pytest.raises(jit.JitError, match="If in a batch hook") as ei:

            @jit.plugin
            class Branch:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    if True:  # MARK:b_branch
                        self.total += df["dur"].sum()

        _check(ei.value, "b_branch", "if True")

    def test_in_on_a_column(self):
        with pytest.raises(jit.JitError, match="`In` on columns") as ei:

            @jit.plugin
            class InCol:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += (df["pid"] in (1, 2)).sum()  # MARK:b_incol

        _check(ei.value, "b_incol", 'df["pid"] in')

    def test_mixed_row_sets(self):
        with pytest.raises(jit.JitError, match="different row sets") as ei:

            @jit.plugin
            class Rows:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    dur = df["dur"]
                    self.total += (dur[dur > 3] + dur).sum()  # MARK:b_rows

        _check(ei.value, "b_rows", "dur[dur > 3] + dur")

    def test_float_reduction_into_an_integer_field(self):
        with pytest.raises(jit.JitError, match="integer field and this reduction is a float") as ei:

            @jit.plugin
            class Narrow:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += (df["dur"] * 0.5).sum()  # MARK:b_narrow

        _check(ei.value, "b_narrow", "(df")

    def test_unknown_name(self):
        with pytest.raises(jit.JitError, match="unknown name nope") as ei:

            @jit.plugin
            class Nope:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += nope.sum()  # noqa: F821  # MARK:b_nope

        _check(ei.value, "b_nope", "nope.sum")

    def test_reading_state(self):
        with pytest.raises(jit.JitError, match="write-only") as ei:

            @jit.plugin
            class ReadState:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    keep = self.total + df["dur"]  # MARK:b_read
                    self.total += keep.sum()

        _check(ei.value, "b_read", "self.total")

    def test_assigning_a_reduction(self):
        with pytest.raises(jit.JitError, match="has no merge") as ei:

            @jit.plugin
            class Assign:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total = df["dur"].sum()  # MARK:b_assign

        _check(ei.value, "b_assign", "self.total")

    def test_keyed_state(self):
        with pytest.raises(jit.JitError, match="keyed update") as ei:

            @jit.plugin
            class Keyed:
                by_pid = jit.map(key=(jit.i64,), value=jit.sum())

                @jit.each_batch
                def step(self, df):
                    self.by_pid[(1,)] += df["dur"].sum()  # MARK:b_keyed

        _check(ei.value, "b_keyed", "self.by_pid")

    def test_container_state(self):
        with pytest.raises(jit.JitError, match="is a set") as ei:

            @jit.plugin
            class Container:
                seen: Set[int] = set()

                @jit.each_batch
                def step(self, df):
                    self.seen += df["pid"].max()  # MARK:b_container

        _check(ei.value, "b_container", "self.seen")

    def test_list_append_of_a_series(self):
        with pytest.raises(jit.JitError, match="stored on self escapes") as ei:

            @jit.plugin
            class Appended:
                total: int = 0
                keep: List[int] = []

                @jit.each_batch
                def step(self, df):
                    self.keep.append(df["dur"])  # MARK:b_append
                    self.total += 1

        _check(ei.value, "b_append", "self.keep.append")

    def test_bare_batch_name(self):
        with pytest.raises(jit.JitError, match="is the batch") as ei:

            @jit.plugin
            class Bare:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += df.sum()  # MARK:b_bare

        _check(ei.value, "b_bare", "df.sum")

    def test_column_name_must_be_literal(self):
        with pytest.raises(jit.JitError, match="takes a column name string") as ei:

            @jit.plugin
            class Dynamic:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    pick = 3
                    self.total += df[pick].sum()  # MARK:b_dynamic

        _check(ei.value, "b_dynamic", "pick")

    def test_each_event_and_each_batch_together(self):
        with pytest.raises(jit.JitError, match="not both"):

            @jit.plugin
            class Both:
                total: int = 0

                @jit.each_event
                def step(self, e):
                    self.total += 1

                @jit.each_batch
                def other(self, df):
                    self.total += df["dur"].sum()

    def test_a_field_the_batch_never_updates(self):
        with pytest.raises(jit.JitError, match="never updated in the each_batch body"):

            @jit.plugin
            class Idle:
                total: int = 0
                other: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += df["dur"].sum()

    def test_a_batch_body_that_folds_nothing(self):
        with pytest.raises(jit.JitError, match="folds nothing"):

            @jit.plugin
            class Nothing:
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    dur = df["dur"]  # noqa: F841

    def test_a_store_needs_each_event(self):
        with pytest.raises(jit.JitError, match="need @jit.each_event"):

            @jit.plugin
            class WithStore:
                seen: jit.Store[int, int]
                total: int = 0

                @jit.each_batch
                def step(self, df):
                    self.total += df["dur"].sum()
