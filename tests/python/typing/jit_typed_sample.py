# SPDX-License-Identifier: MIT
"""Typed @jit.plugin sample: ty and mypy --strict check this clean.

No map is annotated; the value/key types are inferred from jit.map's overloads,
proving IDE autocomplete + static checking work with zero annotations. The one
annotation, ``e: Event``, is what lets the body be checked (it is optional).
"""

from collections import defaultdict
from dataclasses import dataclass
from typing import Callable, DefaultDict, Dict, List, Optional, Set, Tuple, Union

from typing_extensions import assert_type

from dftracer.utils import DataFrame, jit
from dftracer.utils.columnar import Expr
from dftracer.utils.jit import (
    ApproxTopK,
    ArgMax,
    Counter,
    Event,
    Map,
    Sample,
    Str,
    Sum,
    TopK,
    Variance,
)
from dftracer.utils.plugins import JSONValue

total = jit.map(key=(jit.i64,), value=jit.sum())
assert_type(total, Map[tuple[int], Sum])

# A bare single-component key infers the same 1-tuple Map surface.
bare = jit.map(key=jit.i64, value=jit.sum())
assert_type(bare, Map[tuple[int], Sum])

hits = jit.map(key=(jit.i64,), value=jit.count())
assert_type(hits, Map[tuple[int], Counter])

dur_var = jit.map(key=(jit.i64,), value=jit.variance())
assert_type(dur_var, Map[tuple[int], Variance])

hot = jit.map(key=(jit.i64,), value=jit.argmax(of=jit.str_))
assert_type(hot, Map[tuple[int], ArgMax[Str]])

hot_k = jit.map(key=(jit.i64,), value=jit.topk(3, of=jit.str_))
assert_type(hot_k, Map[tuple[int], TopK[Str]])

freq_k = jit.map(key=(jit.i64,), value=jit.approx_topk(5, of=jit.str_))
assert_type(freq_k, Map[tuple[int], ApproxTopK[Str]])

samp_k = jit.map(key=(jit.i64,), value=jit.sample(10, of=jit.i64))
assert_type(samp_k, Map[tuple[int], Sample[int]])

two_key = jit.map(key=(jit.i64, jit.str_), value=jit.count())
assert_type(two_key, Map[tuple[int, Str], Counter])


@jit.plugin
class Sampled:
    hits = jit.map(key=(jit.i64,), value=jit.count())
    bare = jit.map(key=jit.i64, value=jit.count())
    bytes_ = jit.map(key=(jit.i64,), value=jit.sum())
    lo = jit.map(key=(jit.i64,), value=jit.min(of=jit.i64))
    names = jit.map(key=(jit.i64,), value=jit.set())
    paths = jit.map(key=(jit.i64,), value=jit.list())
    per_tid = jit.map(key=(jit.i64, jit.i64), value=jit.count())
    dur_var = jit.map(key=(jit.i64,), value=jit.variance())
    hot = jit.map(key=(jit.i64,), value=jit.argmax(of=jit.str_))
    top_files = jit.map(key=(jit.i64,), value=jit.topk(3, of=jit.str_))
    freq_files = jit.map(key=(jit.i64,), value=jit.approx_topk(5, of=jit.str_))
    ts_sample = jit.map(key=(jit.i64,), value=jit.sample(10, of=jit.i64))
    grand_total = jit.map(key=(), value=jit.sum())

    @jit.each_event
    def step(self, e: Event) -> None:
        pid = e.pid
        if e.cat == "POSIX":
            self.hits[(pid,)] += 1
            self.bare[pid] += 1
            self.bytes_[(pid,)] += e.arg_i64("ret")
            self.lo[(pid,)].observe(e.dur)
            self.names[(pid,)].observe(e.name)
            self.paths[(pid,)].append(e.name, order_by=e.ts)
            self.per_tid[(pid, e.tid)] += 1
            self.dur_var[(pid,)].observe(e.dur)
            self.hot[(pid,)].observe(e.fhash, by=e.dur)
            self.top_files[(pid,)].observe(e.fhash, by=e.dur)
            self.freq_files[(pid,)].observe(e.fhash)
            self.ts_sample[(pid,)].observe(e.ts)
            self.grand_total[()] += e.dur


# ---- every remaining public name -------------------------------------------

# Widths and the NONE sentinel.
widths = (jit.i8, jit.i16, jit.i32, jit.i64, jit.u8, jit.u16, jit.u32, jit.u64)
floats = (jit.f32, jit.f64)
raw_bytes = jit.bytes
none_sentinel = jit.NONE

# Every monoid factory, as a typed map value.
mean_m = jit.map(key=(jit.i64,), value=jit.mean())
std_m = jit.map(key=(jit.i64,), value=jit.stddev())
var_m = jit.map(key=(jit.i64,), value=jit.var())
std_alias = jit.map(key=(jit.i64,), value=jit.std())
q_m = jit.map(key=(jit.i64,), value=jit.quantiles((0.5, 0.99)))
min_m = jit.map(key=(jit.i64,), value=jit.min(of=jit.i64))
max_m = jit.map(key=(jit.i64,), value=jit.max(of=jit.f64))
minf_m = jit.map(key=(jit.i64,), value=jit.minf())
maxf_m = jit.map(key=(jit.i64,), value=jit.maxf())
distinct_m = jit.map(key=(jit.i64,), value=jit.distinct())
argmin_m = jit.map(key=(jit.i64,), value=jit.argmin(of=jit.i64))
bottom_m = jit.map(key=(jit.i64,), value=jit.bottomk(2, of=jit.str_))
reducers = (
    jit.skew(),
    jit.kurt(),
    jit.sumsq(),
    jit.hist(),
    jit.busy(),
    jit.concurrency(),
    jit.utilization(),
    jit.active(),
    jit.first(),
    jit.last(),
    jit.count_valid(),
    jit.bitset(),
    jit.prod(),
    jit.corr(),
    jit.covar_pop(),
    jit.covar_samp(),
    jit.regr_slope(),
    jit.regr_intercept(),
    jit.regr_r2(),
)
sum_m = jit.sum()
assert_type(sum_m, Sum)

# Product values: a tuple, a dict, and a record.
tuple_product = jit.map(key=(jit.i64,), value=(jit.count(), jit.sum()))
dict_product = jit.map(key=(jit.i64,), value={"n": jit.count(), "bytes": jit.sum()})
assert_type(tuple_product, Map[Tuple[int], jit.Product])
assert_type(dict_product, Map[Tuple[int], jit.Product])


@jit.record
class Stats:
    count = jit.count()
    dur = jit.sum()


record_map = jit.map(key=(jit.i64, jit.str_), value=Stats)


def takes_port(port: jit.Port) -> None: ...


# Product, Port and the typed monoid classes by name.
product_names: Tuple[type, ...] = (
    jit.Product,
    jit.Mean,
    jit.Stddev,
    jit.Quantiles,
    jit.Distinct,
    jit.Min,
    jit.Max,
    jit.SetV,
    jit.ListV,
    jit.ArgMin,
    jit.BottomK,
)

# Numeric primitives lower inside @jit.each_event; each is a typed marker.
prim_ints = (
    jit.ilog2(8),
    jit.bit_width(8),
    jit.clz(8),
    jit.ctz(8),
    jit.popcount(8),
    jit.ceil_pow2(8),
    jit.floor_pow2(8),
    jit.rotl(8, 1),
    jit.rotr(8, 1),
    jit.abs(-8),
    jit.clamp(8, 0, 4),
    jit.div_ceil(8, 3),
    jit.div_round(8, 3),
    jit.align_up(8, 4),
    jit.align_down(8, 4),
    jit.isqrt(8),
    jit.gcd(8, 4),
    jit.mul_hi(8, 4),
    jit.mix64(8),
    jit.fastrange(8, 4),
)
prim_floats = (
    jit.fmin(1.0, 2.0),
    jit.fmax(1.0, 2.0),
    jit.clampf(1.0, 0.0, 2.0),
    jit.sqrt(4.0),
    jit.log2(4.0),
    jit.log(4.0),
    jit.exp(4.0),
    jit.fma(1.0, 2.0, 3.0),
    jit.lerp(1.0, 2.0, 0.5),
    jit.copysign(1.0, -1.0),
)


@jit.plugin
class Everything:
    total = jit.publish(of=jit.f64)
    mean_dur = jit.map(key=(jit.i64,), value=jit.mean())
    stats = jit.map(key=(jit.i64,), value={"n": jit.count(), "bytes": jit.sum()})

    @jit.each_event
    def step(self, e: Event) -> None:
        self.total += e.dur
        self.mean_dur[(e.pid,)].observe(e.dur)
        self.stats[(e.pid,)].n += 1
        if e.fhash != jit.NONE:
            self.mean_dur[(jit.ilog2(e.dur),)].observe(e.arg_f64("x"))


@jit.plugin
class Consumer:
    seen = jit.consume(Everything.total, of=jit.f64)
    hits = jit.map(key=(jit.i64,), value=jit.count())

    @jit.each_event
    def step(self, e: Event) -> None:
        self.hits[(e.pid,)] += 1


@jit.plugin()
class Called:
    hits = jit.map(key=(jit.i64,), value=jit.count())

    @jit.each_event(raw=False)
    def step(self, e: Event) -> None:
        self.hits[(e.pid,)] += 1


@jit.plugin
class RawBody:
    hits = jit.map(key=(jit.i64,), value=jit.count())

    @jit.each_event(raw=True)
    def step(self) -> str:
        return "int x = 0; (void)x;"


@jit.vfold
class BatchFold:
    total = jit.sum()

    @jit.each_batch
    def step(self, df: DataFrame) -> None:
        self.total += df["dur"].sum()


# Packages: identity, build settings, plugin and series decorators.
pkg = jit.JitPackage("acme", cache_dir=None, cflags=["-O2"], out=None)


@pkg.plugin
class Packaged:
    hits = jit.map(key=(jit.i64,), value=jit.count())

    @jit.each_event
    def step(self, e: Event) -> None:
        self.hits[(e.pid,)] += 1


identity: str = pkg.identity(Packaged, "hits")
shippable: bool = pkg.shippable(Packaged)
so_path: str = pkg.build(Packaged)
so_path2: str = jit.build(Packaged, out=None)
class_path: str = jit.compile_class(Packaged)
provided: dict[str, str] = jit.provided_names(Packaged)
renamed: dict[str, str] = jit.plugin_result_names(Packaged)
is_plugin: bool = jit.is_jit_plugin(Packaged)

# jit.series: an Expr over its column arguments, called and registered.


@jit.series
def doubled(dur: Expr) -> Expr:
    return dur * 2


@jit.series(module="stats")
def shifted(dur: Expr) -> Expr:
    return dur + 1


@pkg.series
def scaled(dur: Expr) -> Expr:
    return dur * 3


series_ops: Tuple[Callable[..., Union[int, float, object]], ...] = (doubled, shifted, scaled)
op_names: list[str] = jit.ops.list()

# jit.op: a one-argument transform, piped, compiled and run.


@jit.op
def plus_one(x: int) -> int:
    return x + 1


piped = plus_one | plus_one
assert_type(piped, jit.Op)
compiled: str = jit.compile_op(piped)
ran = jit.run_op(piped, 3)

# A plugin's runtime config is a frozen dataclass named by ``config: Cfg``; the
# body reads each field as ``self.config.<field>`` with the dataclass type.


@dataclass(frozen=True)
class Cfg:
    threshold: int = 0
    scale: float = 1.0
    enabled: bool = True
    label: str = "read"
    cap: Optional[int] = None


@jit.plugin
class Configured:
    config: Cfg
    hits = jit.map(key=(jit.i64,), value=jit.count())
    scaled = jit.map(key=(jit.i64,), value=jit.sum())

    @jit.each_event
    def step(self, e: Event) -> None:
        if e.dur > self.config.threshold:
            self.hits[(e.pid,)] += 1
            self.scaled[(e.pid,)] += e.dur * self.config.scale


checked_config: Optional[Dict[str, JSONValue]] = jit.plugin_config(Configured, {"threshold": 3})

jit_error: type[Exception] = jit.JitError


# Plain-container scan state: the merge across worker slices is inferred from
# the update code; a jit.Store[K, V] is a persistent key-value field.


@jit.plugin
class PlainState:
    by_pid_name: Dict[Tuple[int, str], int] = defaultdict(int)
    total: int = 0
    secs: float = 0.0
    names: Set[str] = set()
    order: List[int] = []
    hi: DefaultDict[int, int] = defaultdict(int)
    lo: dict[int, int] = {}
    runs: jit.Store[int, int]
    weights: jit.Store[Tuple[int, str], float]

    @jit.each_event
    def step(self, e: Event) -> None:
        self.by_pid_name[(e.pid, e.name)] += 1
        self.total += 1
        self.secs += e.dur * 0.5
        self.names.add(e.name)
        self.order.append(e.ts)
        self.hi[e.pid] = max(self.hi[e.pid], e.dur)
        self.lo[e.pid] = min(self.lo.get(e.pid, e.dur), e.dur)
        self.runs.put(e.pid, self.runs.get(e.pid, 0) + 1)
        self.weights.put((e.pid, e.name), self.weights.get((e.pid, e.name), 0.0) + 1.0)


def use_store(handle: jit.Store[int, int]) -> int:
    handle.put(1, 2)
    return handle.get(1, 0)


# Per-event conditions: if / elif / else, and / or / not, chained comparisons,
# ``in`` a literal and ``is None``.


@dataclass(frozen=True)
class CondCfg:
    cap: Optional[int] = None
    label: str = "read"


@jit.plugin
class Conditions:
    config: CondCfg
    small: int = 0
    mid: int = 0
    big: int = 0
    picked: int = 0
    missing: int = 0

    @jit.each_event
    def step(self, e: Event) -> None:
        if e.dur < 20:
            self.small += 1
        elif 20 <= e.dur < 40 and not e.pid == 3:
            self.mid += 1
        else:
            self.big += 1
        if e.pid in (1, 3) or e.name in {"read", "open"}:
            self.picked += 1
        if e.fhash is None and (self.config.cap is None or e.dur < self.config.cap):
            self.missing += 1


# A batch hook: columns in, reductions into scan state out.


@jit.plugin
class BatchHook:
    total: float = 0.0
    kept: int = 0
    hi = jit.map(key=(), value=jit.max(of=jit.i64))
    avg = jit.map(key=(), value=jit.mean())

    @jit.each_batch
    def step(self, df: jit.Frame) -> None:
        dur = df["dur"]
        keep = (dur > 30) & (df["pid"] != 3)
        picked = dur[keep]
        self.total += (picked.clip(0, 100) * 0.5 + abs(picked) + picked.abs()).sum()
        self.total += (2 * picked + 1 - picked + 0.5 * picked + 1.0 / picked + (3 - picked)).sum()
        self.kept += keep.sum()
        self.hi[()].observe(picked.where(picked > 40, 0).max())
        spare: jit.Column[float] = df["args.f"].fill_null(0.0)
        flags: jit.Mask = spare.isnull() | ~(spare >= 1.5)
        self.avg[()].observe(spare[flags].mean())


# Exceptions, tasks, files and JSON / YAML values. A raise lowers to a status,
# a try over a call that can fail lowers to a status check, an ``async def`` task
# is a state machine started from the finalize body, ``with jit.open`` is a file
# that closes on every way out and ``jit.json`` is a typed value.


@dataclass(frozen=True)
class FlowCfg:
    path: str = ""


@jit.plugin
class Flow:
    config: FlowCfg
    names: Set[str] = set()
    total: int = 0

    @jit.each_event
    def step(self, e: Event) -> None:
        if e.dur > 1_000_000:
            raise jit.Fatal("too long")
        if e.dur < 0:
            raise jit.SkipRest()
        if e.pid < 0:
            raise jit.Cancelled()
        try:
            with jit.open(self.config.path) as f:
                for line in f:
                    doc = jit.json(line)
                    self.total += doc.find("n").as_int()
                    self.names.add(doc.find("s").as_str("x"))
        except jit.NotFound:
            self.total += 1
        except (jit.ParseError, jit.IoError):
            self.total += 2
        except jit.Error:
            self.total += 3

    @jit.task
    async def work(self, a: int, secs: float) -> int:
        await jit.sleep(secs)
        return a

    @jit.finalize
    async def done(self) -> None:
        x = await self.work(1, 0.1)
        a, b = await jit.all(self.work(1, 0.1), self.work(2, 0.1))
        w = await jit.any(self.work(1, 0.1), self.work(2, 0.2))
        jit.emit("x", x + a + b + w)
        yml = jit.yaml("a: 1")
        for key, value in yml.items():
            jit.emit("key", key)
            jit.emit("is_null", value.is_null())
        for item in yml.find("list"):
            jit.emit("float", item.as_float() + len(yml))
            jit.emit("bool", item.as_bool())


def typed_handles(file: jit.File, value: jit.Value) -> Tuple[str, str, int]:
    return file.read(4), value.find("k").as_str(), len(value)


errors: Tuple[type, ...] = (jit.Error, jit.NotFound, jit.IoError, jit.ParseError)
fatal: Exception = jit.Fatal("stop")
skip: Exception = jit.SkipRest()
cancelled: BaseException = jit.Cancelled()
located: Optional[str] = jit.Fatal("stop", file="f.py", line=1, col=1).file


# Channels, latest values and phase results. A port is a class attribute: the
# annotation says the element type (``int`` or ``float``), the factory the
# options, and a receiver, a latest or a phase takes another plugin's attribute.


@jit.plugin
class PortsOut:
    out: jit.Sender[int] = jit.sender(credits=64, policy="drop_oldest")
    ratio: jit.Sender[float] = jit.sender()
    pipe: jit.Channel[int] = jit.channel(credits=2, policy="block")
    cfg: jit.Latest[int] = jit.latest()
    mass: jit.Phase[float] = jit.phase(merge="max")
    total: jit.Phase[int] = jit.phase()
    seen: int = 0

    @jit.each_event
    def step(self, e: Event) -> None:
        self.seen += 1
        sent: bool = self.out.try_send(e.dur)
        self.ratio.send(e.dur / 2)
        self.cfg.set(e.pid)
        self.mass.contribute(e.dur * 0.5)
        self.total.contribute(e.dur)
        if sent:
            self.cfg.set(0)

    @jit.task
    async def produce(self, n: int) -> int:
        await self.pipe.send(n)
        self.pipe.close()
        return n

    @jit.task
    async def consume(self) -> int:
        got = 0
        async for v in self.pipe:
            got += v
        return got

    @jit.finalize
    async def done(self) -> None:
        a, b = await jit.all(self.produce(3), self.consume())
        self.total.seal()
        self.mass.seal()
        whole: int = self.total.result(0)
        peak: float = self.mass.result()
        latest: int = self.cfg.get(-1)
        jit.emit("whole", whole + a + b + latest)
        jit.emit("peak", peak)


@jit.plugin
class PortsIn:
    inp: jit.Receiver[int] = jit.receiver(PortsOut.out)
    flt: jit.Receiver[float] = jit.receiver(PortsOut.ratio)
    named: jit.Receiver[int] = jit.receiver("plugin.other.q", credits=8, policy="drop_oldest")
    cfg: jit.Latest[int] = jit.latest(PortsOut.cfg)
    total: jit.Phase[int] = jit.phase(PortsOut.total)
    seen: int = 0

    @jit.each_event
    def step(self, e: Event) -> None:
        self.seen += 1

    @jit.finalize
    async def done(self) -> None:
        first: int = await self.inp.recv()
        try:
            second: float = await self.flt.recv()
            jit.emit("second", second)
        except jit.Closed:
            jit.emit("closed", first)
        async for v in self.inp:
            jit.emit("v", v)


closed: Exception = jit.Closed("channel closed")
