# SPDX-License-Identifier: MIT
"""Per-event conditions in an @jit.each_event body: if / elif / else, and / or /
not, chained comparisons, ``in`` a literal, ``is None``, several guards and
nested ifs. Each test compiles a plugin, runs it over a multi-file trace and
compares the result with the same logic in plain Python."""

import gzip
import os
import re
import shutil
from dataclasses import dataclass
from typing import Optional

import pytest

from dftracer.utils import Runtime, jit
from dftracer.utils.plugins import Plugins

pa = pytest.importorskip("pyarrow")

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)

_FILES = 4
_PER_FILE = 60
_NAMES = ("read", "write", "open")
_CATS = ("POSIX", "STDIO")


def _events():
    """(pid, name, cat, ts, dur, v, f, fhash); fhash is None for every third."""
    out = []
    for n in range(_FILES * _PER_FILE):
        fhash = None if n % 3 == 0 else f"h{n % 5}"
        out.append(
            (
                1 + n % 4,
                _NAMES[n % 3],
                _CATS[n % 2],
                1000 + n,
                10 + (n * 7) % 53,
                (n * 5) % 17 - 8,
                (n % 11) * 0.5,
                fhash,
            )
        )
    return out


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("cond_trace")
    events = _events()
    for f in range(_FILES):
        with gzip.open(os.path.join(d, f"t{f}.pfw.gz"), "wt", encoding="utf-8") as fh:
            for pid, name, cat, ts, dur, v, fl, fhash in events[
                f * _PER_FILE : (f + 1) * _PER_FILE
            ]:
                fh_arg = "" if fhash is None else f',"fhash":"{fhash}"'
                fh.write(
                    f'{{"name":"{name}","cat":"{cat}","pid":{pid},"tid":1,"ts":{ts},'
                    f'"dur":{dur},"ph":"X","args":{{"v":{v},"f":{fl}{fh_arg}}}}}\n'
                )
    return str(d)


def _run(cls, trace, threads, config=None):
    with Runtime(threads=threads) as rt:
        plugins = (
            Plugins([cls], runtime=rt, config=config) if config else Plugins([cls], runtime=rt)
        )
        return plugins.run(trace).results


def _total(results, name):
    cols = pa.table(results[name]).to_pydict()
    return sum(cols["value"]) if cols["value"] else 0


def _counts(results, names):
    return {n: _total(results, n) for n in names}


THREADS = [1, 4]


@pytest.mark.parametrize("threads", THREADS)
def test_if_elif_else(trace, threads):
    @jit.plugin
    class Bands:
        small: int = 0
        mid: int = 0
        big: int = 0
        mid_dur: float = 0.0

        @jit.each_event
        def step(self, e):
            if e.dur < 20:
                self.small += 1
            elif e.dur < 40:
                self.mid += 1
                self.mid_dur += e.dur
            else:
                self.big += 1

    assert Bands._jit_plugin.lowering == "branch"
    want = {"small": 0, "mid": 0, "big": 0, "mid_dur": 0}
    for _, _, _, _, dur, *_ in _events():
        if dur < 20:
            want["small"] += 1
        elif dur < 40:
            want["mid"] += 1
            want["mid_dur"] += dur
        else:
            want["big"] += 1
    assert want["small"] and want["mid"] and want["big"]
    assert _counts(_run(Bands, trace, threads), want) == pytest.approx(want)


@pytest.mark.parametrize("threads", THREADS)
def test_else_without_elif_and_nested_ifs(trace, threads):
    @jit.plugin
    class Nested:
        posix_hi: int = 0
        posix_lo: int = 0
        other: int = 0

        @jit.each_event
        def step(self, e):
            if e.cat == "POSIX":
                if e.dur > 30:
                    self.posix_hi += 1
                else:
                    self.posix_lo += 1
            else:
                self.other += 1

    want = {"posix_hi": 0, "posix_lo": 0, "other": 0}
    for _, _, cat, _, dur, *_ in _events():
        if cat == "POSIX":
            if dur > 30:
                want["posix_hi"] += 1
            else:
                want["posix_lo"] += 1
        else:
            want["other"] += 1
    assert _counts(_run(Nested, trace, threads), want) == want


@pytest.mark.parametrize("threads", THREADS)
def test_and_or_not(trace, threads):
    @jit.plugin
    class Logic:
        a: int = 0
        b: int = 0
        c: int = 0

        @jit.each_event
        def step(self, e):
            if e.dur > 20 and e.pid == 2:
                self.a += 1
            if e.dur < 15 or e.name == "open":
                self.b += 1
            if not (e.pid == 1 or e.cat == "STDIO") and e.dur >= 25:
                self.c += 1

    want = {"a": 0, "b": 0, "c": 0}
    for pid, name, cat, _, dur, *_ in _events():
        want["a"] += dur > 20 and pid == 2
        want["b"] += dur < 15 or name == "open"
        want["c"] += (not (pid == 1 or cat == "STDIO")) and dur >= 25
    assert all(want.values())
    assert Logic._jit_plugin.lowering == "branch"
    assert _counts(_run(Logic, trace, threads), want) == want


@pytest.mark.parametrize("threads", THREADS)
def test_chained_comparison(trace, threads):
    @jit.plugin
    class Chain:
        inside: int = 0
        half_open: int = 0
        fl: int = 0

        @jit.each_event
        def step(self, e):
            if 20 <= e.dur < 40:
                self.inside += 1
            if e.pid < e.tid + 2 <= e.pid + 1:
                self.half_open += 1
            if 1.0 < e.arg_f64("f") <= 3.0:
                self.fl += 1

    want = {"inside": 0, "half_open": 0, "fl": 0}
    for pid, _, _, _, dur, _, f, _ in _events():
        want["inside"] += 20 <= dur < 40
        want["half_open"] += pid < 1 + 2 <= pid + 1
        want["fl"] += 1.0 < f <= 3.0
    assert want["inside"] and want["fl"]
    assert _counts(_run(Chain, trace, threads), want) == want


@pytest.mark.parametrize("threads", THREADS)
def test_in_literal_tuple_and_set(trace, threads):
    @jit.plugin
    class Membership:
        pids: int = 0
        not_pids: int = 0
        names: int = 0
        cats: int = 0

        @jit.each_event
        def step(self, e):
            if e.pid in (1, 3):
                self.pids += 1
            if e.pid not in [1, 3]:
                self.not_pids += 1
            if e.name in {"read", "open"}:
                self.names += 1
            if e.cat in ("STDIO",):
                self.cats += 1

    want = {"pids": 0, "not_pids": 0, "names": 0, "cats": 0}
    for pid, name, cat, *_ in _events():
        want["pids"] += pid in (1, 3)
        want["not_pids"] += pid not in (1, 3)
        want["names"] += name in ("read", "open")
        want["cats"] += cat == "STDIO"
    assert _counts(_run(Membership, trace, threads), want) == want


@pytest.mark.parametrize("threads", THREADS)
def test_is_none_on_a_nullable_field(trace, threads):
    @jit.plugin
    class Nullable:
        missing: int = 0
        present: int = 0

        @jit.each_event
        def step(self, e):
            if e.fhash is None:
                self.missing += 1
            if e.fhash is not None:
                self.present += 1

    ev = _events()
    want = {
        "missing": sum(x[7] is None for x in ev),
        "present": sum(x[7] is not None for x in ev),
    }
    assert want["missing"] and want["present"]
    assert _counts(_run(Nullable, trace, threads), want) == want


@pytest.mark.parametrize("threads", THREADS)
def test_is_none_on_an_optional_config_field(trace, threads):
    @dataclass(frozen=True)
    class Cfg:
        cap: Optional[int] = None

    @jit.plugin
    class Capped:
        config: Cfg
        hits: int = 0

        @jit.each_event
        def step(self, e):
            if self.config.cap is None or e.dur < self.config.cap:
                self.hits += 1

    ev = _events()
    assert _total(_run(Capped, trace, threads), "hits") == len(ev)
    capped = _run(Capped, trace, threads, config={"Capped": {"cap": 30}})
    assert _total(capped, "hits") == sum(x[4] < 30 for x in ev)


@pytest.mark.parametrize("threads", THREADS)
def test_truthiness_of_a_number(trace, threads):
    @jit.plugin
    class Truthy:
        nonzero: int = 0
        zero: int = 0

        @jit.each_event
        def step(self, e):
            if e.arg_i64("v"):
                self.nonzero += 1
            if not e.arg_i64("v"):
                self.zero += 1

    ev = _events()
    want = {"nonzero": sum(x[5] != 0 for x in ev), "zero": sum(x[5] == 0 for x in ev)}
    assert want["zero"] and want["nonzero"]
    assert _counts(_run(Truthy, trace, threads), want) == want


def test_a_guarded_operand_is_not_hoisted_above_its_guard():
    # The same division sits in two guards. It is only valid once the left side
    # of the `and` held, so the compiler must not compute it up front.
    @jit.plugin
    class Guarded:
        up: int = 0
        down: int = 0

        @jit.each_event
        def step(self, e):
            if e.arg_i64("v") > 0 and e.dur / e.arg_i64("v") > 3:
                self.up += 1
            if e.arg_i64("v") > 0 and e.dur / e.arg_i64("v") > 5:
                self.down += 1

    source = Guarded._jit_plugin.source
    assert "/" in source
    assert not re.search(r"_cse\d+ = .*/", source), "a guarded division was hoisted"


@pytest.mark.parametrize("threads", THREADS)
def test_guarded_division_matches_python(trace, threads):
    @jit.plugin
    class Guarded:
        up: int = 0
        down: int = 0

        @jit.each_event
        def step(self, e):
            if e.arg_i64("v") > 0 and e.dur / e.arg_i64("v") > 3:
                self.up += 1
            if e.arg_i64("v") > 0 and e.dur / e.arg_i64("v") > 5:
                self.down += 1

    ev = _events()
    want = {
        "up": sum(1 for x in ev if x[5] > 0 and x[4] / x[5] > 3),
        "down": sum(1 for x in ev if x[5] > 0 and x[4] / x[5] > 5),
    }
    assert want["up"] > want["down"] > 0
    assert _counts(_run(Guarded, trace, threads), want) == want


@pytest.mark.parametrize("threads", THREADS)
def test_integer_division_is_true_division(trace, threads):
    @jit.plugin
    class Ratio:
        ratio: float = 0.0
        half: float = 0.0

        @jit.each_event
        def step(self, e):
            if e.arg_i64("v") > 0:
                self.ratio += e.dur / e.arg_i64("v")
            self.half += e.pid / e.tid / 2

    ev = _events()
    want = {
        "ratio": sum(x[4] / x[5] for x in ev if x[5] > 0),
        "half": sum(x[0] / 1 / 2 for x in ev),
    }
    assert _counts(_run(Ratio, trace, threads), want) == pytest.approx(want)


def test_floor_division_is_a_compile_error():
    with pytest.raises(jit.JitError, match="arithmetic operator"):

        @jit.plugin
        class Floor:
            total: float = 0.0

            @jit.each_event
            def step(self, e):
                self.total += e.dur // 2


@pytest.mark.parametrize("threads", THREADS)
def test_a_single_guard_lowers_to_a_mask(trace, threads):
    @jit.plugin
    class Masked:
        hits = jit.map(key=(jit.i64,), value=jit.count())
        secs = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            if e.dur > 30 and e.cat == "POSIX":
                self.hits[(e.pid,)] += 1
                self.secs[(e.pid,)] += e.dur * 0.5

    assert Masked._jit_plugin.lowering == "mask"
    results = _run(Masked, trace, threads)
    cols = pa.table(results["hits"]).to_pydict()
    got = dict(zip(cols["k0"], cols["value"]))
    want = {}
    for pid, _, cat, _, dur, *_ in _events():
        if dur > 30 and cat == "POSIX":
            want[pid] = want.get(pid, 0) + 1
    assert got == want
    scols = pa.table(results["secs"]).to_pydict()
    assert dict(zip(scols["k0"], scols["value"])) == pytest.approx(
        {
            pid: sum(
                d * 0.5 for p, _, c, _, d, *_ in _events() if p == pid and d > 30 and c == "POSIX"
            )
            for pid in want
        }
    )


@pytest.mark.parametrize("threads", THREADS)
def test_several_top_level_guards_branch_per_event(trace, threads):
    @jit.plugin
    class TwoGuards:
        a: int = 0
        b: int = 0

        @jit.each_event
        def step(self, e):
            if e.dur > 30:
                self.a += 1
            if e.pid == 1:
                self.b += 1

    assert TwoGuards._jit_plugin.lowering == "branch"
    ev = _events()
    want = {"a": sum(x[4] > 30 for x in ev), "b": sum(x[0] == 1 for x in ev)}
    assert _counts(_run(TwoGuards, trace, threads), want) == want


def test_an_unguarded_body_branches_per_event():
    @jit.plugin
    class Plain:
        hits: int = 0

        @jit.each_event
        def step(self, e):
            self.hits += 1

    assert Plain._jit_plugin.lowering == "branch"
