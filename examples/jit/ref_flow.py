# SPDX-License-Identifier: MIT
import os
import sys
import tempfile
from dataclasses import dataclass

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins


@dataclass(frozen=True)
class Cfg:
    path: str


@jit.plugin
class Totals:
    config: Cfg
    total: int = 0
    missing: int = 0
    bad: int = 0

    @jit.each_event
    def step(self, e):
        try:
            with jit.open(self.config.path) as f:
                for line in f:
                    d = jit.json(line)
                    self.total += d.find("n").as_int()
        except jit.NotFound:
            self.missing += 1
        except (jit.ParseError, jit.IoError):
            self.bad += 1


@jit.plugin
class Fan:
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1

    @jit.task
    async def fetch(self, a: int, secs: float) -> int:
        await jit.sleep(secs)
        return a * 10

    @jit.finalize
    async def done(self) -> None:
        one = await self.fetch(1, 0.1)
        x, y = await jit.all(self.fetch(2, 0.1), self.fetch(3, 0.1))
        first = await jit.any(self.fetch(4, 5.0), self.fetch(5, 0.01))
        jit.emit("sum", one + x + y + first)


@jit.plugin
class Source:
    out: jit.Sender[int] = jit.sender(credits=512)

    @jit.each_event
    def step(self, e):
        self.out.send(e.dur)  # stored now, or raises jit.Error


@jit.plugin
class Sink:
    inp: jit.Receiver[int] = jit.receiver(Source.out)
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1

    @jit.finalize
    async def done(self) -> None:
        total = 0
        async for v in self.inp:  # until the channel is closed
            total += v
        jit.emit("total", total)


@jit.plugin
class Tally:
    n: int = 0
    cfg: jit.Latest[int] = jit.latest()
    peak: jit.Phase[int] = jit.phase(merge="max")

    @jit.each_event
    def step(self, e):
        self.n += 1
        self.cfg.set(7)
        self.peak.contribute(e.dur)

    @jit.finalize
    def done(self) -> None:
        self.peak.seal()
        peak = self.peak.result(0)
        cfg = self.cfg.get(0)
        jit.emit("peak", peak)
        jit.emit("cfg", cfg)


if __name__ == "__main__":
    trace = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "n.jsonl")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write('{"n": 5}\n{"n": 6}\n')
        run = Plugins([Totals], config={"Totals": {"path": path}}).run(trace)
        assert run.results["total"].to_pandas()["value"][0] == 3300
        run = Plugins([Totals], config={"Totals": {"path": path + ".none"}}).run(trace)
        assert run.results["missing"].to_pandas()["value"][0] == 300

    assert Plugins([Fan]).run(trace).results["sum"] == b"110"
    # the three batches of this scan can send at most 300 values of 59 or less
    assert int(Plugins([Source, Sink]).run(trace).results["total"]) > 0
    run = Plugins([Tally]).run(trace)
    assert run.results["peak"] == b"59" and run.results["cfg"] == b"7"
