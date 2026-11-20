# SPDX-License-Identifier: MIT
import sys

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins


@jit.plugin
class Producer:
    out: jit.Sender[int] = jit.sender(credits=512)

    @jit.each_event
    def step(self, e):
        if e.dur > 55:
            self.out.send(e.dur)


@jit.plugin
class Consumer:
    inp: jit.Receiver[int] = jit.receiver(Producer.out)
    events: int = 0

    @jit.each_event
    def step(self, e):
        self.events += 1

    @jit.finalize
    async def done(self) -> None:
        count = 0
        total = 0
        async for v in self.inp:
            count += 1
            total += v
        jit.emit("count", count)
        jit.emit("total", total)


run = Plugins([Consumer, Producer]).run(sys.argv[1])
print("count:", run.results["count"].decode())
print("total:", run.results["total"].decode())
