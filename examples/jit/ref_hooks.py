# SPDX-License-Identifier: MIT
import sys

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins


@jit.plugin
class SlowReads:
    secs: float = 0.0
    kept: int = 0
    worst = jit.map(key=(), value=jit.max(of=jit.i64))

    @jit.each_batch
    def step(self, df: jit.Frame) -> None:
        dur = df["dur"]
        keep = (dur > 30) & (df["pid"] != 3)
        slow = dur[keep]
        self.secs += (slow * 0.5).sum()
        self.kept += keep.sum()
        self.worst[()].observe(slow.max())


if __name__ == "__main__":
    run = Plugins([SlowReads]).run(sys.argv[1])
    assert int(run.results["kept"].to_pandas()["value"][0]) == 174
    assert int(run.results["worst"].to_pandas()["value"][0]) == 59
