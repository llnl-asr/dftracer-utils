# SPDX-License-Identifier: MIT
import sys

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins


@jit.plugin
class SlowShare:
    slow: int = 0
    total: int = 0
    worst = jit.map(key=(), value=jit.max(of=jit.i64))

    @jit.each_batch
    def step(self, df: jit.Frame) -> None:
        dur = df["dur"]
        keep = dur > 40
        self.slow += keep.sum()
        self.total += dur.count()
        self.worst[()].observe(dur[keep].max())


run = Plugins([SlowShare]).run(sys.argv[1])
print("slow:", run.results["slow"].to_pandas()["value"][0])
print("total:", run.results["total"].to_pandas()["value"][0])
print("worst:", run.results["worst"].to_pandas()["value"][0])
