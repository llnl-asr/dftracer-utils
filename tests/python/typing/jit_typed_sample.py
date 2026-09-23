"""Typed @jit.plugin sample: ty checks this clean with inference only.

No map is annotated; the value/key types are inferred from jit.map's overloads,
proving IDE autocomplete + static checking work with zero annotations. The one
annotation, ``e: Event``, is what lets the body be checked (it is optional).
"""

from typing_extensions import assert_type

from dftracer.utils import jit
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
class Sample:
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
