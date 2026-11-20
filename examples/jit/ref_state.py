# SPDX-License-Identifier: MIT
import sys
import tempfile
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins
from dftracer.utils.store import Store


@jit.plugin
class Ops:
    by_name: Dict[Tuple[int, str], int] = defaultdict(int)
    bytes_by_pid: Dict[int, int] = {}
    names: Set[str] = set()
    order: List[int] = []
    total: int = 0
    secs: float = 0.0

    @jit.each_event
    def step(self, e):
        self.by_name[(e.pid, e.name)] += 1
        self.bytes_by_pid[e.pid] = self.bytes_by_pid.get(e.pid, 0) + e.arg_i64("ret")
        self.names.add(e.name)
        self.order.append(e.ts)
        self.total += 1
        self.secs += e.dur * 0.5


@jit.plugin
class Seen:
    runs: jit.Store[int, int]
    first_seen = jit.map(key=(jit.i64,), value=jit.min(of=jit.i64))

    @jit.each_event
    def step(self, e):
        self.first_seen[(e.pid,)].observe(self.runs.get(e.pid, 0))
        self.runs.put(e.pid, self.runs.get(e.pid, 0) + 1)


@jit.plugin
class Producer:
    seen = jit.map(key=(jit.i64,), value=jit.count())
    sig = jit.publish(of=jit.u64)

    @jit.each_event
    def step(self, e):
        self.seen[(e.pid,)] += 1
        self.sig += 1


@jit.plugin
class Consumer:
    got = jit.map(key=(jit.i64,), value=jit.count())
    sig = jit.consume(Producer.sig, of=jit.u64)

    @jit.each_event
    def step(self, e):
        if self.sig > 0:  # a producer published this batch
            self.got[(e.pid,)] += 1


@dataclass(frozen=True)
class Cfg:
    threshold: int = 0  # int, float, bool and str are native values
    label: str = "read"
    cap: Optional[int] = None  # Optional of one of them may be unset


@jit.plugin
class Slow:
    config: Cfg
    slow = jit.map(key=(jit.i64,), value=jit.count())

    @jit.each_event
    def step(self, e):
        if e.dur > self.config.threshold and e.name == self.config.label:
            self.slow[(e.pid,)] += 1


if __name__ == "__main__":
    trace = sys.argv[1]

    run = Plugins([Ops]).run(trace)
    assert int(run.results["total"].to_pandas()["value"][0]) == 300

    with tempfile.TemporaryDirectory() as index_dir:
        Plugins([Seen]).run(trace, index_dir=index_dir)
        store = Store.open(f"{index_dir}/.dftindex/store")  # the store of that index root
        name = Path(jit.compile_class(Seen)).stem
        ns = store.namespace(f"plugin.{name}.runs", 1, durable=True, shared=True)
        value = store.snapshot().get(ns, None, 1)  # what the scan stored for pid 1
        assert value is not None

    run = Plugins([Producer, Consumer]).run(trace)
    assert int(run.results["got"].to_pandas()["value"].sum()) == 300

    plugins = Plugins([Slow], config={"Slow": {"threshold": 45}})
    assert int(plugins.run(trace).results["slow"].to_pandas()["value"].sum()) == 84
