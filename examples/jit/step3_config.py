# SPDX-License-Identifier: MIT
import sys
from dataclasses import dataclass

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins


@dataclass(frozen=True)
class Cfg:
    threshold: int = 0
    label: str = "read"


@jit.plugin
class Slow:
    config: Cfg
    slow = jit.map(key=(jit.i64,), value=jit.count())

    @jit.each_event
    def step(self, e):
        if e.dur > self.config.threshold and e.name == self.config.label:
            self.slow[(e.pid,)] += 1


for threshold in (40, 50):
    run = Plugins([Slow], config={"Slow": {"threshold": threshold}}).run(sys.argv[1])
    table = run.results["slow"].to_pandas().sort_values("k0")
    print(threshold, dict(zip(table["k0"], table["value"])))

try:
    Plugins([Slow], config={"Slow": {"threshold": "high"}})
except jit.JitError as error:
    print("rejected:", error.reason)
