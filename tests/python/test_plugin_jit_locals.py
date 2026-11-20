# SPDX-License-Identifier: MIT
"""Locals in a compiled body: ``int``, ``float`` and ``str`` bound by assignment,
updated with ``+=``, and kept across the statements of an ``@jit.each_event``
body, a ``@jit.finalize`` body and a task frame."""

import gzip
import os
import shutil
from dataclasses import dataclass

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

pa = pytest.importorskip("pyarrow")

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("local_trace")
    with gzip.open(os.path.join(d, "t.pfw.gz"), "wt", encoding="utf-8") as fh:
        for i in range(20):
            name = "read" if i % 2 else "write"
            fh.write(
                f'{{"name":"{name}","cat":"POSIX","pid":{1 + i % 3},"tid":1,"ts":{1000 + i},'
                f'"dur":{10 + i},"ph":1,"args":{{}}}}\n'
            )
    return str(d)


def _total(results, name):
    cols = pa.table(results[name]).to_pydict()
    return sum(cols["value"]) if cols["value"] else 0


def test_locals_in_each_event_match_plain_python(trace):
    @jit.plugin
    class Locals:
        big: int = 0
        half: float = 0.0
        reads: int = 0

        @jit.each_event
        def step(self, e):
            d = e.dur
            scaled = d * 0.5
            kind = e.name
            bump = 0
            bump += d - 10
            if bump > 5:
                self.big += bump
            self.half += scaled - 1.0
            if kind == "read":
                self.reads += -d + d + 1

    results = Plugins([Locals]).run(trace).results
    durs = [10 + i for i in range(20)]
    assert _total(results, "big") == sum(d - 10 for d in durs if d - 10 > 5)
    assert _total(results, "half") == pytest.approx(sum(d * 0.5 - 1.0 for d in durs))
    assert _total(results, "reads") == 10


def test_a_str_local_compares_and_is_a_key(trace):
    @jit.plugin
    class Keyed:
        by_name = jit.map(key=(jit.str_,), value=jit.count())

        @jit.each_event
        def step(self, e):
            n = e.name
            if n != "read":
                self.by_name[(n,)] += 1

    cols = pa.table(Plugins([Keyed]).run(trace).results["by_name"]).to_pydict()
    assert dict(zip(cols["k0"], cols["value"])) == {"write": 10}


def test_a_local_assigned_in_a_branch_is_read_after_it(trace):
    @jit.plugin
    class Branch:
        hi: int = 0
        lo: int = 0

        @jit.each_event
        def step(self, e):
            level = 1
            if e.dur > 20:
                level = 5
            if level > 1:
                self.hi += level
            else:
                self.lo += level

    results = Plugins([Branch]).run(trace).results
    over = sum(1 for i in range(20) if 10 + i > 20)
    assert _total(results, "hi") == 5 * over
    assert _total(results, "lo") == 20 - over


def test_task_locals_use_the_config(trace):
    @dataclass(frozen=True)
    class Cfg:
        label: str = "x"
        k: int = 3

    @jit.plugin
    class Cfged:
        config: Cfg
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def scale(self, v: int) -> int:
            await jit.sleep(0.001)
            tag = self.config.label
            if tag == "go":
                v += 100
            return v * self.config.k

        @jit.finalize
        async def done(self) -> None:
            r = await self.scale(2)
            jit.emit("r", r)

    plugins = Plugins([Cfged], config={"Cfged": {"label": "go", "k": 2}})
    assert plugins.run(trace).results["r"] == b"204"
    assert Plugins([Cfged]).run(trace).results["r"] == b"6"
