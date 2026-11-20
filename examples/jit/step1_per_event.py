# SPDX-License-Identifier: MIT
import sys
from typing import Dict

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins


@jit.plugin
class SlowPerPid:
    slow: Dict[int, int] = {}

    @jit.each_event
    def step(self, e):
        if e.dur > 40:
            self.slow[e.pid] = self.slow.get(e.pid, 0) + 1


run = Plugins([SlowPerPid]).run(sys.argv[1])
table = run.results["slow"].to_pandas()
print(table.sort_values("k0").reset_index(drop=True))
print("lowering:", SlowPerPid._jit_plugin.lowering)
