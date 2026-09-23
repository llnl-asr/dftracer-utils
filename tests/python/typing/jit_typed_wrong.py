"""Deliberately-wrong typed @jit.plugin: ty must flag every mistake below.

Each marked line is a real authoring error the typed DSL now catches statically,
mirroring what the AST compiler would reject at decoration time.
"""

from dftracer.utils import jit
from dftracer.utils.jit import Event


@jit.plugin
class Wrong:
    hits = jit.map(key=(jit.i64,), value=jit.count())
    paths = jit.map(key=(jit.i64,), value=jit.list())
    per_tid = jit.map(key=(jit.i64, jit.i64), value=jit.count())
    hot = jit.map(key=(jit.i64,), value=jit.argmax(of=jit.str_))
    top_files = jit.map(key=(jit.i64,), value=jit.topk(3, of=jit.str_))

    @jit.each_event
    def step(self, e: Event) -> None:
        pid = e.pid
        self.hits[(pid,)] += "x"  # str into an int counter
        self.paths[(pid,)].append(e.name)  # missing required order_by
        self.per_tid[(pid, e.tid)] += "y"  # str into an int counter
        self.hot[(pid,)].observe(e.fhash)  # argmax observe missing required by=
        self.hot[(pid,)].observe(e.fhash, by=e.name)  # by= must be a float, not Str
        self.top_files[(pid,)].observe(e.fhash)  # topk observe missing required by=
        _ = e.piddd  # unknown event field
