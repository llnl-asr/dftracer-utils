# SPDX-License-Identifier: MIT
"""Safety of the flow constructs of a compiled plugin: row buffers that a loop
fills, JSON documents that are parsed again, the store in a task, the tag of a
failure with a long path, and awaits in nested blocks of a task."""

import gzip
import os
import shutil
import time

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

from .jit_common import compile_error, load_module, plugin_stats

pa = pytest.importorskip("pyarrow")

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("safety_trace")
    with gzip.open(os.path.join(d, "t.pfw.gz"), "wt", encoding="utf-8") as fh:
        for i in range(3):
            fh.write(
                f'{{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":{1000 + i},'
                f'"dur":10,"ph":1,"args":{{}}}}\n'
            )
    return str(d)


def _total(results, name):
    return sum(pa.table(results[name]).to_pydict()["value"])


_CFG = """
from dataclasses import dataclass
from dftracer.utils import jit

@dataclass(frozen=True)
class Cfg:
    path: str = ""
"""

_SITES = 40
_ADDS = "\n".join("            self.total += 1" for _ in range(_SITES))


def _over_module(tmp_path, name, loop_first):
    loop = "            for line in f:\n                self.total += 1\n"
    plain = f"{_ADDS}\n"
    body = loop + plain if loop_first else plain + loop
    return load_module(
        tmp_path,
        name,
        _CFG
        + f"""
@jit.plugin
class Over:
    config: Cfg
    total: int = 0

    @jit.each_event
    def step(self, e):
        with jit.open(self.config.path) as f:
{body}""",
    )


@pytest.mark.parametrize("loop_first", [True, False])
@pytest.mark.parametrize("lines", [1, 2, 3, 41, 300])
def test_loop_and_plain_sites_of_one_map_fit_their_buffer(trace, tmp_path, loop_first, lines):
    module = _over_module(tmp_path, f"over_{int(loop_first)}_{lines}", loop_first)
    data = tmp_path / "f.txt"
    data.write_text("".join(f"l{i}\n" for i in range(lines)), encoding="utf-8")
    results = Plugins([module.Over], config={"Over": {"path": str(data)}}).run(trace).results
    assert _total(results, "total") == 3 * (lines + _SITES)


# ---- a document that is parsed again ---------------------------------------


def test_a_failed_reparse_keeps_the_old_document(trace, tmp_path):
    module = load_module(
        tmp_path,
        "reparse_failed",
        """
from dftracer.utils import jit

@jit.plugin
class Keep:
    total: int = 0

    @jit.each_event
    def step(self, e):
        x = jit.json('{"a": 11}')
        try:
            x = jit.json('{bad')
        except jit.ParseError:
            self.total += x.find("a").as_int()
""",
    )
    assert _total(Plugins([module.Keep]).run(trace).results, "total") == 33


def test_a_value_taken_before_a_reparse_cannot_be_used_after_it(tmp_path):
    with pytest.raises(jit.JitError, match="parsed again") as info:
        load_module(
            tmp_path,
            "reparse_stale",
            """
from dftracer.utils import jit

@jit.plugin
class Stale:
    total: int = 0

    @jit.each_event
    def step(self, e):
        x = jit.json('{"a": 11}')
        y = x.find("a")
        x = jit.json('{"a": 22}')
        self.total += y.as_int()
""",
        )
    from .jit_common import assert_located

    assert_located(info.value)
    assert info.value.source is not None and "y.as_int" in info.value.source


def test_a_value_used_in_a_loop_that_parses_its_source_again_is_an_error(tmp_path):
    with compile_error("parsed again"):
        load_module(
            tmp_path,
            "reparse_loop",
            """
from dftracer.utils import jit

@jit.plugin
class Loop:
    total: int = 0

    @jit.each_event
    def step(self, e):
        x = jit.json('{"a": [1, 2]}')
        y = x.find("a")
        for item in y:
            self.total += item.as_int()
            x = jit.json('{"a": 5}')
""",
        )


def test_numbers_read_before_a_reparse_stay_valid(trace, tmp_path):
    module = load_module(
        tmp_path,
        "reparse_ok",
        """
from dftracer.utils import jit

@jit.plugin
class Fine:
    total: int = 0

    @jit.each_event
    def step(self, e):
        x = jit.json('{"a": 11}')
        y = x.find("a").as_int()
        x = jit.json('{"a": 22}')
        self.total += y + x.find("a").as_int()
""",
    )
    assert _total(Plugins([module.Fine]).run(trace).results, "total") == 3 * 33


# ---- the store in a task ---------------------------------------------------

_STORE_TASK = """
from dftracer.utils import jit

@jit.plugin
class T:
    runs: jit.Store[int, int]
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1

    @jit.task
    async def work(self) -> int:
        {stmt}
        return 1

    @jit.finalize
    async def done(self):
        x = await self.work()
        jit.emit("x", x)
"""


@pytest.mark.parametrize(
    "stmt",
    ["v = self.runs.get(1, 0)", "self.runs.put(1, 2)", "v = await self.runs.get(1, 0)"],
)
def test_a_task_cannot_touch_the_store(tmp_path, stmt):
    with compile_error("store"):
        load_module(tmp_path, "store_task", _STORE_TASK.format(stmt=stmt))


# ---- the tag of a failure with a long path ---------------------------------


def test_a_long_path_does_not_cut_the_tag(trace, tmp_path):
    long_path = "/nonexistent/" + "d" * 900 + "/f.txt"
    module = load_module(
        tmp_path,
        "long_path",
        _CFG
        + """
@jit.plugin
class Long:
    config: Cfg
    n: int = 0

    @jit.each_event
    def step(self, e):
        with jit.open(self.config.path) as f:
            for line in f:
                self.n += 1
""",
    )
    with pytest.raises(jit.NotFound) as info:
        Plugins([module.Long], config={"Long": {"path": long_path}}).run(trace)
    assert info.value.line is not None and info.value.file is not None
    assert long_path in info.value.message


# ---- awaits in nested blocks and cancels -----------------------------------


def test_awaits_inside_if_elif_else_and_try_in_a_task(trace, tmp_path):
    module = load_module(
        tmp_path,
        "nested_await",
        """
from dftracer.utils import jit

@jit.plugin
class Nest:
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1

    @jit.task
    async def pick(self, mode: int) -> int:
        total = 0
        if mode == 0:
            await jit.sleep(0.001)
            total += 1
        elif mode == 1:
            await jit.sleep(0.001)
            total += 10
            await jit.sleep(0.001)
            total += 10
        else:
            try:
                await jit.sleep(0.001)
                total += 100
            except jit.Error:
                total += 7
        try:
            x = await self.leaf(mode)
            total += x
        except jit.Error:
            total += 1000
        return total

    @jit.task
    async def leaf(self, v: int) -> int:
        await jit.sleep(0.001)
        return v

    @jit.finalize
    async def done(self) -> None:
        a = await self.pick(0)
        b = await self.pick(1)
        c = await self.pick(2)
        if a == 1:
            await jit.sleep(0.001)
            jit.emit("a", a)
        else:
            jit.emit("a_else", -1)
        jit.emit("b", b)
        jit.emit("c", c)
""",
    )
    got = {
        k: v for k, v in Plugins([module.Nest]).run(trace).results.items() if isinstance(v, bytes)
    }
    assert got == {"a": b"1", "b": b"21", "c": b"102"}


def test_every_loser_of_any_is_cancelled_and_released_once(trace):
    @jit.plugin
    class Losers:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def slow(self, secs: float) -> int:
            await jit.sleep(secs)
            return 1

        @jit.task
        async def fast(self) -> int:
            await jit.sleep(0.01)
            return 7

        @jit.finalize
        async def done(self) -> None:
            w = await jit.any(self.slow(30.0), self.slow(40.0), self.fast(), self.slow(50.0))
            jit.emit("w", w)

    plugins = Plugins([Losers])
    start = time.monotonic()
    results = plugins.run(trace).results
    assert time.monotonic() - start < 10
    assert results["w"] == b"7"
    stats = plugin_stats(jit.compile_class(Losers))
    # finalize and four tasks started, all released once, the three losers never completed
    assert stats["started"] == 5
    assert stats["released"] == 5
    assert stats["completed"] == 2
    assert stats["cancelled"] == 3


def test_a_failure_in_all_cancels_each_other_task_exactly_once(trace):
    @jit.plugin
    class Siblings:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def forever(self, secs: float) -> int:
            await jit.sleep(secs)
            return 1

        @jit.task
        async def fails(self) -> int:
            await jit.sleep(0.01)
            raise jit.Fatal("one failed")

        @jit.finalize
        async def done(self) -> None:
            a, b, c, d = await jit.all(
                self.forever(30.0), self.forever(40.0), self.fails(), self.forever(50.0)
            )
            jit.emit("a", a + b + c + d)

    plugins = Plugins([Siblings])
    with pytest.raises(jit.Fatal, match="one failed"):
        plugins.run(trace)
    stats = plugin_stats(jit.compile_class(Siblings))
    assert stats["started"] == 5
    assert stats["released"] == 5
    assert stats["completed"] == 0
    assert stats["cancelled"] == 3
