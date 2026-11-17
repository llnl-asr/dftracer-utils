# SPDX-License-Identifier: MIT
"""Typed @jit.plugin sample: ty and mypy --strict check this clean.

No map is annotated; the value/key types are inferred from jit.map's overloads,
proving IDE autocomplete + static checking work with zero annotations. The one
annotation, ``e: Event``, is what lets the body be checked (it is optional).
"""

from dataclasses import dataclass
from typing import Callable, Dict, Optional, Tuple, Union

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
