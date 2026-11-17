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


@jit.plugin
class WrongBatch:
    total: int = 0

    @jit.each_batch
    def step(self, df: int) -> None:  # a batch hook takes the batch frame
        self.total += df["dur"].sum()

    def helper(self, df: jit.Frame) -> None:
        _ = df["dur"] + "x"  # a column plus a str
        _ = df["dur"].where(1, 0)  # where takes a mask
        _ = df["dur"][df["pid"]]  # a column is indexed by a mask
        _ = df["dur"].clip("a", "b")  # clip takes numbers
        _ = df["dur"].std()  # outside the vocabulary
        _ = df[3]  # a column name is a string


jit.each_batch(3)  # a method


@jit.each_batch
def batch_returns_int(self: object, df: jit.Frame) -> int:  # a batch hook returns None
    return 1


# ---- exceptions, tasks, files and values ------------------------------------

jit.sleep("soon")  # the seconds are a number
jit.emit(1, 2)  # the name of a result is a string
jit.json(3)  # a text is a string
jit.open(4)  # a path is a string
jit.Fatal(3)  # a message is a string
jit.all(1, 2)  # an awaitable
_text: int = jit.json("{}").find("k").as_str()  # as_str gives a str
_missing = jit.json("{}").find(1)  # a key is a string
_default = jit.json("{}").as_int("x")  # a default is an int
jit.task(3)  # a function


@jit.task
def not_async(self: object) -> int:  # a task is an async def
    return 1
