# SPDX-License-Identifier: MIT
import sys

import dftracer.utils as dft

with dft.udf.scope():

    @dft.udf
    def cost(dur: int, pid: int) -> float:
        if pid == 2:
            return dur * 2.0
        return dur * 1.0

    @dft.udf
    def over(x: float) -> bool:
        return x > 50.0

    view = dft.TraceViewer(sys.argv[1] + "/trace.pfw.gz")
    got = (
        view.duql("derive c = cost(dur, pid) | where over(c) | select pid, c").collect().to_pandas()
    )
print(got.groupby("pid")["c"].agg(["count", "max"]))
print("cost registered after scope:", "cost" in dft.udf)
