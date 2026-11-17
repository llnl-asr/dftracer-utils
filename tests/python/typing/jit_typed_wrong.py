# SPDX-License-Identifier: MIT
"""Deliberately-wrong typed @jit.plugin: ty must flag every mistake below.

Each marked line is a real authoring error the typed DSL now catches statically,
mirroring what the AST compiler would reject at decoration time.
"""

from dataclasses import dataclass

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


# ---- one wrong use per remaining construct ----------------------------------

jit.map(key=(1,), value=jit.count())  # a key component must be a jit type
jit.map(key=(jit.i64,), value=3)  # a value must be a reduction
jit.min(of=3)  # of= takes a jit type
jit.topk("3", of=jit.str_)  # k is an int
jit.publish(of="u64")  # of= takes a jit type
jit.consume(3)  # a port or a wire id string
jit.quantiles(0.5)  # a tuple of quantiles
jit.record(3)  # a class
jit.vfold(3)  # a class
jit.build("Wrong")  # a class
jit.JitPackage(1)  # a namespace string


@jit.each_event(raw=True)
def raw_returns_int(self: object) -> int:  # a raw body returns a C++ string
    return 1


@jit.series
def returns_int(dur: object) -> int:  # a series body returns an Expr
    return 1


_ = jit.JitPackage().identity(3)  # a class


@dataclass(frozen=True)
class WrongCfg:
    threshold: int = 0
    label: str = "x"


@jit.plugin
class WrongConfigured:
    config: WrongCfg
    hits = jit.map(key=(jit.i64,), value=jit.sum())

    @jit.each_event
    def step(self, e: Event) -> None:
        self.hits[(e.pid,)] += e.dur + self.config.label  # str into int arithmetic
        self.config.threshold = 5  # a frozen config field cannot be assigned
        _ = self.config.missing  # no such config field


@jit.plugin
class WrongStore:
    seen: jit.Store[int, int]

    @jit.each_event
    def step(self, e: Event) -> None:
        self.seen.put(e.pid, "x")  # a str value into a Store[int, int]
        self.seen.put("k", 1)  # a str key into a Store[int, ...]
        self.seen.get(e.pid)  # get needs a default
