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
class Budget:
    config: Cfg
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1

    @jit.finalize
    def done(self) -> None:
        limit = 0
        with jit.open(self.config.path) as f:
            for line in f:
                d = jit.json(line)
                limit += d.find("max_events").as_int()
        jit.emit("limit", limit)


with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "budget.jsonl")
    with open(path, "w", encoding="utf-8") as f:
        f.write('{"max_events": 100}\n{"max_events": 250}\n')
    run = Plugins([Budget], config={"Budget": {"path": path}}).run(sys.argv[1])
print("limit:", run.results["limit"].decode())
print("events:", run.results["n"].to_pandas()["value"][0])
