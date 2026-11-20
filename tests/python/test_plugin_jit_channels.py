# SPDX-License-Identifier: MIT
"""Channels, latest values and phase results in a compiled plugin: a port is a
class attribute with its element type in the annotation (``jit.Sender[int]``,
``jit.Receiver[float]``, ``jit.Latest[int]``, ``jit.Phase[float]``) that the plugin
factory declares to the host (``dftu_svc_ports``). The host checks the plan; the
calls on a port lower to the ports service."""

import faulthandler
import gzip
import os
import shutil
import tempfile
import time

import pytest

from dftracer.utils import Runtime, jit
from dftracer.utils.plugins import Plugins

from .jit_common import assert_located, load_module, plugin_stats

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)

THREADS = [1, 4]
_N = 40


def _dur(i):
    return 10 + (i * 7) % 31


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("channel_trace")
    for f in range(2):
        with gzip.open(os.path.join(d, f"t{f}.pfw.gz"), "wt", encoding="utf-8") as fh:
            for i in range(f * _N, (f + 1) * _N):
                fh.write(
                    f'{{"name":"read","cat":"POSIX","pid":{1 + i % 3},"tid":1,"ts":{1000 + i},'
                    f'"dur":{_dur(i)},"ph":1,"args":{{"f":{i * 0.25}}}}}\n'
                )
    return str(d)


def _durs():
    return [_dur(i) for i in range(2 * _N)]


def _bytes(results):
    return {k: v for k, v in results.items() if isinstance(v, bytes)}


def _run(classes, trace, threads=1, stats_of=None):
    """The results of one run; with ``stats_of`` (a class of the set), also the
    change in the counters of its library over the run. The library stays loaded
    between runs of one process, so the counters are compared before and after,
    while the loaded set is alive."""
    with Runtime(threads=threads) as rt:
        plugins = Plugins(classes, runtime=rt)
        if stats_of is None:
            return plugins.run(trace).results
        path = jit.compile_class(stats_of)
        before = plugin_stats(path)
        results = plugins.run(trace).results
        after = plugin_stats(path)
        return results, {k: after[k] - before[k] for k in after}


# ---- two plugins wired producer to consumer -------------------------------


@pytest.fixture(autouse=True)
def _hang_guard():
    """A regression that waits for ever ends the process with the stack of every
    thread after two minutes, instead of hanging the suite."""
    faulthandler.dump_traceback_later(120, exit=True)
    yield
    faulthandler.cancel_dump_traceback_later()


@pytest.mark.parametrize("threads", THREADS)
def test_an_int_channel_carries_every_value_between_two_plugins(trace, threads):
    @jit.plugin
    class IntProd:
        out: jit.Sender[int] = jit.sender(credits=256)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            self.out.send(e.dur)

    @jit.plugin
    class IntCons:
        inp: jit.Receiver[int] = jit.receiver(IntProd.out)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        async def done(self) -> None:
            total = 0
            count = 0
            biggest = 0
            async for v in self.inp:
                total += v
                count += 1
                if v > biggest:
                    biggest = v
            jit.emit("total", total)
            jit.emit("count", count)
            jit.emit("biggest", biggest)

    results, stats = _run([IntProd, IntCons], trace, threads, stats_of=IntCons)
    got = _bytes(results)
    durs = _durs()
    assert got == {
        "total": str(sum(durs)).encode(),
        "count": str(len(durs)).encode(),
        "biggest": str(max(durs)).encode(),
    }
    assert stats["started"] == stats["released"] == stats["completed"] == 1


@pytest.mark.parametrize("threads", THREADS)
def test_a_float_channel_carries_floats_and_the_load_order_does_not_matter(trace, threads):
    @jit.plugin
    class FltProd:
        out: jit.Sender[float] = jit.sender(credits=256)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            self.out.send(e.arg_f64("f") * 2 + 0.5)

    @jit.plugin
    class FltCons:
        inp: jit.Receiver[float] = jit.receiver(FltProd.out)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        async def done(self) -> None:
            total = 0.0
            async for v in self.inp:
                total += v
            jit.emit("total", total)

    want = sum(i * 0.25 * 2 + 0.5 for i in range(2 * _N))
    for order in ([FltProd, FltCons], [FltCons, FltProd]):
        got = _bytes(_run(order, trace, threads))
        assert float(got["total"]) == pytest.approx(want)


# ---- Block: back-pressure, close, cancel ----------------------------------


def test_block_holds_the_producer_to_the_credits_of_the_channel(trace):
    @jit.plugin
    class Pipe:
        q: jit.Channel[int] = jit.channel(credits=2, policy="block")
        go: jit.Channel[int] = jit.channel()
        sent: jit.Latest[int] = jit.latest()
        gap: jit.Phase[int] = jit.phase(merge="max")
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def produce(self, count: int) -> int:
            await self.q.send(1)
            await self.q.send(2)
            self.sent.set(2)
            await self.q.send(3)
            self.sent.set(3)
            self.go.send(1)
            await self.q.send(4)
            self.sent.set(4)
            await self.q.send(5)
            self.sent.set(5)
            await self.q.send(6)
            self.sent.set(6)
            await self.q.send(7)
            self.sent.set(7)
            await self.q.send(8)
            self.sent.set(8)
            self.q.close()
            return count

        @jit.task
        async def consume(self, count: int) -> int:
            first = await self.q.recv()
            total = first
            seen = 1
            await self.go.recv()
            done = self.sent.get(0)
            self.gap.contribute(done - seen)
            async for v in self.q:
                seen += 1
                total += v
                done = self.sent.get(0)
                self.gap.contribute(done - seen)
            return total

        @jit.finalize
        async def done(self) -> None:
            a, b = await jit.all(self.produce(8), self.consume(8))
            self.gap.seal()
            worst = self.gap.result(0)
            jit.emit("produced", a)
            jit.emit("total", b)
            jit.emit("gap", worst)

    got = _bytes(_run([Pipe], trace))
    assert got["produced"] == b"8"
    assert got["total"] == str(sum(range(1, 9))).encode()
    # The producer is told when its third send is done, the consumer has taken one
    # value, and the 4th send then waits: with 2 credits it is 2 ahead and never
    # more. A channel without back-pressure would let it run to 7.
    assert int(got["gap"]) == 2


def test_close_ends_the_consumer_loop_and_recv_on_a_closed_channel_raises_closed(trace):
    @jit.plugin
    class Closing:
        q: jit.Channel[int] = jit.channel(credits=8)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        async def done(self) -> None:
            self.q.send(10)
            self.q.send(20)
            self.q.close()
            first = await self.q.recv()
            second = await self.q.recv()
            jit.emit("first", first)
            jit.emit("second", second)
            third = -1
            try:
                third = await self.q.recv()
            except jit.Closed:
                pass
            jit.emit("third", third)
            late = self.q.try_send(5)
            jit.emit("late", late)

    assert _bytes(_run([Closing], trace)) == {
        "first": b"10",
        "second": b"20",
        "third": b"-1",
        "late": b"0",
    }


def test_an_uncaught_closed_reaches_the_caller_with_its_location(trace, tmp_path):
    module = load_module(
        tmp_path,
        "chan_closed",
        """
        from dftracer.utils import jit

        @jit.plugin
        class Empty:
            q: jit.Channel[int] = jit.channel()
            n: int = 0

            @jit.each_event
            def step(self, e):
                self.n += 1

            @jit.finalize
            async def done(self) -> None:
                self.q.close()
                x = await self.q.recv()  # RECV_MARK
                jit.emit("x", x)
        """,
    )
    with pytest.raises(jit.Closed) as info:
        Plugins([module.Empty]).run(trace)
    err = info.value
    assert isinstance(err, jit.NotFound)
    assert err.file == str(tmp_path / "chan_closed.py")
    marked = next(
        i for i, ln in enumerate(open(err.file).read().splitlines(), 1) if "RECV_MARK" in ln
    )
    assert err.line == marked
    assert "closed and drained" in err.message


def test_a_receive_that_loses_a_race_is_cancelled_at_its_await_and_its_frame_is_released(trace):
    @jit.plugin
    class RecvRace:
        q: jit.Channel[int] = jit.channel()
        ready: jit.Channel[int] = jit.channel()
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def waits(self) -> int:
            self.ready.send(1)
            v = await self.q.recv()
            raise jit.Fatal("a receive on an empty channel returned")
            return v

        @jit.task
        async def quick(self, v: int) -> int:
            await self.ready.recv()
            return v

        @jit.finalize
        async def done(self) -> None:
            w = await jit.any(self.waits(), self.quick(9))
            jit.emit("winner", w)

    start = time.monotonic()
    results, stats = _run([RecvRace], trace, stats_of=RecvRace)
    assert _bytes(results) == {"winner": b"9"}
    assert time.monotonic() - start < 10
    assert stats["started"] == stats["released"] == 3
    assert stats["cancelled"] == 1
    assert stats["completed"] == 2


# ---- try_send, DropOldest -------------------------------------------------


def test_try_send_on_a_full_channel_returns_false_and_keeps_what_it_holds(trace):
    @jit.plugin
    class Fulls:
        q: jit.Channel[int] = jit.channel(credits=3)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        async def done(self) -> None:
            a = self.q.try_send(1)
            b = self.q.try_send(2)
            c = self.q.try_send(3)
            d = self.q.try_send(4)
            self.q.close()
            jit.emit("a", a)
            jit.emit("d", d)
            e = 0
            async for v in self.q:
                e = e * 10 + v
            jit.emit("kept", e)
            jit.emit("bc", b + c)

    assert _bytes(_run([Fulls], trace)) == {"a": b"1", "d": b"0", "kept": b"123", "bc": b"2"}


def test_send_on_a_full_fail_channel_raises_an_error_with_the_channel_name(trace):
    @jit.plugin
    class FullErr:
        q: jit.Channel[int] = jit.channel(credits=1)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        def done(self) -> None:
            self.q.send(1)
            self.q.send(2)

    with pytest.raises(jit.Error, match="is full") as info:
        Plugins([FullErr]).run(trace)
    assert "plugin." in info.value.message and ".q" in info.value.message


def test_drop_oldest_keeps_the_newest_values(trace):
    @jit.plugin
    class Newest:
        q: jit.Channel[int] = jit.channel(credits=3, policy="drop_oldest")
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        async def done(self) -> None:
            self.q.send(1)
            self.q.send(2)
            self.q.send(3)
            self.q.send(4)
            self.q.send(5)
            self.q.close()
            kept = 0
            async for v in self.q:
                kept = kept * 10 + v
            jit.emit("kept", kept)

    got = _bytes(_run([Newest], trace))
    assert got == {"kept": b"345"}


@pytest.mark.parametrize("threads", THREADS)
def test_try_send_from_the_scan_counts_what_fits_and_the_consumer_gets_exactly_that(trace, threads):
    @jit.plugin
    class Cap:
        out: jit.Sender[int] = jit.sender(credits=25)
        stored: jit.Phase[int] = jit.phase(merge="sum")
        refused: jit.Phase[int] = jit.phase(merge="sum")

        @jit.each_event
        def step(self, e):
            ok = self.out.try_send(e.dur)
            if ok:
                self.stored.contribute(1)
            else:
                self.refused.contribute(1)

        @jit.finalize
        def done(self) -> None:
            self.stored.seal()
            self.refused.seal()
            kept = self.stored.result(0)
            dropped = self.refused.result(0)
            jit.emit("stored", kept)
            jit.emit("refused", dropped)

    @jit.plugin
    class CapCons:
        inp: jit.Receiver[int] = jit.receiver(Cap.out)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        async def done(self) -> None:
            count = 0
            async for v in self.inp:
                count += 1
            jit.emit("taken", count)

    got = _bytes(_run([Cap, CapCons], trace, threads))
    assert got == {"stored": b"25", "refused": str(2 * _N - 25).encode(), "taken": b"25"}


# ---- latest and phase ------------------------------------------------------


def test_latest_gives_the_last_writer_and_the_default_before_the_first(trace):
    @jit.plugin
    class Hint:
        cfg: jit.Latest[int] = jit.latest()
        ratio: jit.Latest[float] = jit.latest()
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        def done(self) -> None:
            before = self.cfg.get(-1)
            self.cfg.set(5)
            self.cfg.set(9)
            self.ratio.set(0.5)
            after = self.cfg.get(-1)
            ratio = self.ratio.get(0.0)
            jit.emit("before", before)
            jit.emit("after", after)
            jit.emit("ratio", ratio)

    assert _bytes(_run([Hint], trace)) == {"before": b"-1", "after": b"9", "ratio": b"0.5"}


def test_a_latest_value_set_in_the_scan_is_read_by_another_plugin(trace):
    @jit.plugin
    class Setter:
        last: jit.Latest[int] = jit.latest()
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            self.last.set(e.dur)

    @jit.plugin
    class Getter:
        last: jit.Latest[int] = jit.latest(Setter.last)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        def done(self) -> None:
            last = self.last.get(-1)
            jit.emit("last", last)

    got = int(_bytes(_run([Setter, Getter], trace))["last"])
    assert got in set(_durs())


@pytest.mark.parametrize("threads", THREADS)
def test_a_phase_merges_the_contributions_of_every_worker_and_is_read_in_finalize(trace, threads):
    @jit.plugin
    class Phases:
        total: jit.Phase[int] = jit.phase(merge="sum")
        lo: jit.Phase[int] = jit.phase(merge="min")
        hi: jit.Phase[float] = jit.phase(merge="max")
        mass: jit.Phase[float] = jit.phase()

        @jit.each_event
        def step(self, e):
            self.total.contribute(e.dur)
            self.lo.contribute(e.dur)
            self.hi.contribute(e.arg_f64("f"))
            self.mass.contribute(e.dur * 0.5)

        @jit.finalize
        def done(self) -> None:
            self.total.seal()
            self.lo.seal()
            self.hi.seal()
            self.mass.seal()
            total = self.total.result()
            lo = self.lo.result()
            hi = self.hi.result()
            mass = self.mass.result()
            jit.emit("total", total)
            jit.emit("lo", lo)
            jit.emit("hi", hi)
            jit.emit("mass", mass)

    got = _bytes(_run([Phases], trace, threads))
    durs = _durs()
    assert int(got["total"]) == sum(durs)
    assert int(got["lo"]) == min(durs)
    assert float(got["hi"]) == pytest.approx(max(i * 0.25 for i in range(2 * _N)))
    assert float(got["mass"]) == pytest.approx(sum(d * 0.5 for d in durs))


def test_a_phase_read_before_its_seal_and_a_contribution_after_it_are_errors(trace):
    @jit.plugin
    class Early:
        p: jit.Phase[int] = jit.phase()
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        def done(self) -> None:
            self.p.contribute(1)
            p = self.p.result(0)
            jit.emit("p", p)

    with pytest.raises(jit.Error, match="still open"):
        Plugins([Early]).run(trace)

    @jit.plugin
    class Late:
        p: jit.Phase[int] = jit.phase()
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        def done(self) -> None:
            self.p.seal()
            self.p.contribute(1)

    with pytest.raises(jit.Error, match="is sealed"):
        Plugins([Late]).run(trace)


def test_a_phase_with_no_contribution_reads_as_the_default(trace):
    @jit.plugin
    class Nothing:
        p: jit.Phase[int] = jit.phase(merge="min")
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        def done(self) -> None:
            self.p.seal()
            p = self.p.result(77)
            jit.emit("p", p)

    assert _bytes(_run([Nothing], trace)) == {"p": b"77"}


# ---- load order, string references, type agreement -------------------------


def _total_consumer(source, name):
    """A plugin that sums what it receives from ``source`` in its finalize."""

    @jit.plugin
    class Sum:
        inp: jit.Receiver[int] = (
            jit.receiver(source, credits=256) if isinstance(source, str) else jit.receiver(source)
        )
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        async def done(self) -> None:
            total = 0
            async for v in self.inp:
                total += v
            jit.emit("t", total)

    return Sum


def _producer():
    @jit.plugin
    class Gen:
        out: jit.Sender[int] = jit.sender(credits=256)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            self.out.send(e.dur)

    return Gen


@pytest.mark.parametrize("by_name", [False, True])
@pytest.mark.parametrize("producer_first", [False, True])
def test_a_consumer_loaded_before_its_sender_still_runs_after_it(trace, by_name, producer_first):
    gen = _producer()
    source = gen._jit_ports["out"].full if by_name else gen.out
    cons = _total_consumer(source, "t")
    order = [gen, cons] if producer_first else [cons, gen]
    assert int(_bytes(_run(order, trace))["t"]) == sum(_durs())


@pytest.mark.parametrize("order", [(0, 1, 2), (2, 1, 0), (1, 2, 0), (1, 0, 2), (0, 2, 1)])
@pytest.mark.parametrize("by_name", [False, True])
def test_a_chain_of_three_plugins_runs_in_order_from_any_load_order(trace, order, by_name):
    @jit.plugin
    class A:
        out: jit.Sender[int] = jit.sender(credits=256)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            self.out.send(e.dur)

    a_out = A._jit_ports["out"].full if by_name else A.out

    @jit.plugin
    class B:
        inp: jit.Receiver[int] = (
            jit.receiver(a_out, credits=256) if by_name else jit.receiver(a_out)
        )
        out: jit.Sender[int] = jit.sender(credits=256)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        async def done(self) -> None:
            async for v in self.inp:
                self.out.send(v * 2)

    b_out = B._jit_ports["out"].full if by_name else B.out
    c = _total_consumer(b_out, "t")
    classes = [A, B, c]
    got = _bytes(_run([classes[i] for i in order], trace))
    assert int(got["t"]) == 2 * sum(_durs())


def test_a_cycle_through_string_references_is_a_plan_error_naming_the_ports(trace):
    # Two builds of one class name in one scope have one identity, so the second
    # Node can name a channel of Other, which names a channel of the first.
    @jit.plugin
    class Node:
        out: jit.Sender[int] = jit.sender()
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    node_full = Node._jit_ports["out"].full

    @jit.plugin
    class Other:
        inp: jit.Receiver[int] = jit.receiver(node_full, credits=16)
        out: jit.Sender[int] = jit.sender()
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    other_full = Other._jit_ports["out"].full

    @jit.plugin
    class Node:  # noqa: F811
        out: jit.Sender[int] = jit.sender()
        inp: jit.Receiver[int] = jit.receiver(other_full, credits=16)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    assert Node._jit_ports["out"].full == node_full
    with pytest.raises(jit.JitError, match="cycle") as info:
        Plugins([Node, Other])
    assert node_full in info.value.reason


def test_one_plugin_can_send_one_channel_and_receive_another(trace):
    gen = _producer()

    @jit.plugin
    class Relay:
        inp: jit.Receiver[int] = jit.receiver(gen.out)
        out: jit.Sender[int] = jit.sender(credits=256)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        async def done(self) -> None:
            async for v in self.inp:
                self.out.send(v + 1)

    cons = _total_consumer(Relay.out, "t")
    for order in ([gen, Relay, cons], [cons, Relay, gen]):
        assert int(_bytes(_run(order, trace))["t"]) == sum(d + 1 for d in _durs())


def test_a_plugin_that_receives_its_own_channel_by_name_is_a_located_error():
    def make(full):
        @jit.plugin
        class Selfish:
            out: jit.Sender[int] = jit.sender()
            inp: jit.Receiver[int] = jit.receiver(full, credits=16)
            n: int = 0

            @jit.each_event
            def step(self, e):
                self.n += 1

        return Selfish

    # The identity is the qualified name, so it is known from a first build.
    full = make("plugin.x.y")._jit_ports["out"].full
    with pytest.raises(jit.JitError, match="a channel of this plugin") as info:
        make(full)
    assert_located(info.value)


def test_a_string_receiver_of_another_type_than_its_jit_sender_is_a_located_error(trace):
    gen = _producer()
    full = gen._jit_ports["out"].full

    @jit.plugin
    class Wrong:
        inp: jit.Receiver[float] = jit.receiver(full, credits=256)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    with pytest.raises(jit.JitError, match="carries float but .* carries int") as info:
        Plugins([gen, Wrong])
    assert_located(info.value)
    assert "inp" in info.value.reason
    Plugins([gen, _total_consumer(full, "t")])


def test_a_receiver_with_no_sender_in_the_set_is_a_plan_error_not_a_hang(trace):
    gen = _producer()
    cons = _total_consumer(gen._jit_ports["out"].full, "t")
    with pytest.raises(jit.JitError, match=r"out"):
        Plugins([cons])


# ---- from an each_batch body ------------------------------------------------


@pytest.mark.parametrize("threads", THREADS)
def test_an_each_batch_body_sends_sets_and_contributes_a_reduction_per_batch(trace, threads):
    @jit.plugin
    class BatchProd:
        out: jit.Sender[int] = jit.sender(credits=4096)
        tot: jit.Phase[int] = jit.phase()
        top: jit.Latest[int] = jit.latest()
        seen: int = 0

        @jit.each_batch
        def fold(self, df: jit.Frame) -> None:
            self.seen += df["dur"].count()
            self.out.send(df["dur"].sum())
            self.tot.contribute(df["dur"].sum())
            self.top.set(df["dur"].max())

    @jit.plugin
    class BatchCons:
        inp: jit.Receiver[int] = jit.receiver(BatchProd.out)
        tot: jit.Phase[int] = jit.phase(BatchProd.tot)
        top: jit.Latest[int] = jit.latest(BatchProd.top)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        async def done(self) -> None:
            queued = 0
            async for v in self.inp:
                queued += v
            self.tot.seal()
            merged = self.tot.result(-1)
            top = self.top.get(-1)
            jit.emit("queued", queued)
            jit.emit("merged", merged)
            jit.emit("top", top)

    got = _bytes(_run([BatchProd, BatchCons], trace, threads))
    durs = _durs()
    assert int(got["queued"]) == sum(durs)
    assert int(got["merged"]) == sum(durs)
    assert int(got["top"]) <= max(durs) and int(got["top"]) >= min(durs)


# ---- plan errors from the host name the port --------------------------------


def test_a_receiver_without_its_sender_in_the_set_is_a_jit_error_naming_the_port(trace):
    @jit.plugin
    class Lonely:
        out: jit.Sender[int] = jit.sender()
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    @jit.plugin
    class Waiting:
        inp: jit.Receiver[int] = jit.receiver(Lonely.out)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    with pytest.raises(jit.JitError, match=r"plugin\..*lonely\.out"):
        Plugins([Waiting])


def test_a_latest_or_phase_reference_to_a_plugin_that_is_not_loaded_is_a_located_error(trace):
    @jit.plugin
    class Owns:
        last: jit.Latest[int] = jit.latest()
        total: jit.Phase[int] = jit.phase()
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    @jit.plugin
    class ReadsLatest:
        last: jit.Latest[int] = jit.latest(Owns.last)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    @jit.plugin
    class SumsPhase:
        total: jit.Phase[int] = jit.phase(Owns.total)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    for cls, attr in ((ReadsLatest, "last"), (SumsPhase, "total")):
        with pytest.raises(jit.JitError, match="no plugin of this set declares") as info:
            Plugins([cls])
        assert_located(info.value)
        assert attr in info.value.reason and f"owns.{attr}" in info.value.reason
        Plugins([Owns, cls])


def test_a_channel_string_reference_with_other_options_fails_the_plan(trace):
    @jit.plugin
    class Owner:
        out: jit.Sender[int] = jit.sender(credits=4)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    @jit.plugin
    class Other:
        inp: jit.Receiver[int] = jit.receiver(Owner._jit_ports["out"].full, credits=8)
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    with pytest.raises(jit.JitError, match=r"declares 8 credits.*declares 4"):
        Plugins([Owner, Other])


def test_a_channel_string_reference_to_no_plugin_fails_the_plan(trace):
    @jit.plugin
    class Dangling:
        inp: jit.Receiver[int] = jit.receiver("plugin.nobody.q")
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    with pytest.raises(jit.JitError, match=r"plugin\.nobody\.q"):
        Plugins([Dangling])


def test_two_plugins_of_one_name_that_both_declare_ports_fail_the_plan(trace):
    def make():
        @jit.plugin
        class Twin:
            q: jit.Channel[int] = jit.channel()
            n: int = 0

            @jit.each_event
            def step(self, e):
                self.n += 1

        return Twin

    with pytest.raises(jit.JitError, match="both provide"):
        Plugins([make(), make()])


# ---- compile errors with the location --------------------------------------


def _err(source):
    with tempfile.TemporaryDirectory() as tmp:
        with pytest.raises(jit.JitError) as info:
            load_module(tmp, "bad_port", source)
    assert_located(info.value)
    return info.value


_HEAD = """
from dftracer.utils import jit

@jit.plugin
class Bad:
    n: int = 0
"""

_EVENT = """
    @jit.each_event
    def step(self, e):
        self.n += 1
"""


def test_a_port_needs_an_annotation_with_its_element_type():
    err = _err(
        """
from dftracer.utils import jit

@jit.plugin
class Bad:
    out = jit.sender()
    n: int = 0
"""
        + _EVENT
    )
    assert "needs an annotation" in err.reason and "out" in err.reason


@pytest.mark.parametrize(
    "decl, expect",
    [
        ("out: jit.Sender[str] = jit.sender()", "carries str"),
        ("out: jit.Sender[bytes] = jit.sender()", "carries bytes"),
        ("out: jit.Receiver[int] = jit.sender()", "annotated jit.Receiver"),
        ("out: int = jit.sender()", "annotate it"),
        ("out: jit.Latest[int] = jit.sender()", "annotated jit.Latest"),
        ("out: jit.Phase[str] = jit.phase()", "carries str"),
    ],
)
def test_a_mistyped_port_declaration_is_a_located_error(decl, expect):
    err = _err(_HEAD + f"    {decl}\n" + _EVENT)
    assert expect in err.reason
    assert "out" in err.reason


def test_a_receiver_of_another_element_type_than_its_sender_is_a_located_error():
    err = _err(
        """
from dftracer.utils import jit

@jit.plugin
class Prod:
    out: jit.Sender[int] = jit.sender()
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1

@jit.plugin
class Cons:
    inp: jit.Receiver[float] = jit.receiver(Prod.out)
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1
"""
    )
    assert "inp" in err.reason and "carries float" in err.reason and "int" in err.reason


def test_a_block_channel_stays_in_the_plugin_that_owns_it():
    for call in (
        "jit.sender(policy='block')",
        "jit.receiver('plugin.a.b', policy='block')",
    ):
        err = _err(_HEAD + f"    out: jit.Sender[int] = {call}\n" + _EVENT)
        assert "finalize bodies of two plugins run one after the other" in err.reason
    err = _err(
        """
from dftracer.utils import jit

@jit.plugin
class Owner:
    q: jit.Channel[int] = jit.channel(policy='block')
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1

@jit.plugin
class Other:
    inp: jit.Receiver[int] = jit.receiver(Owner.q)
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1
"""
    )
    assert "stays in the plugin that owns it" in err.reason and "inp" in err.reason


def test_bad_options_are_located_errors_at_the_declaration():
    for call, expect in (
        ("jit.sender(credits=0)", "credits is a count of at least 1"),
        ("jit.sender(policy='lazy')", "policy is 'fail', 'block' or 'drop_oldest'"),
        ("jit.phase(merge='count')", "merge is 'sum', 'min' or 'max'"),
    ):
        err = _err(_HEAD + f"    out: jit.Sender[int] = {call}\n" + _EVENT)
        assert expect in err.reason


_BODY_ERRORS = [
    (
        "send on a receiver",
        "inp: jit.Receiver[int] = jit.receiver('plugin.a.b')",
        "    @jit.each_event\n    def step(self, e):\n        self.inp.send(1)\n",
        "is a receiver",
    ),
    (
        "recv on a sender",
        "out: jit.Sender[int] = jit.sender()",
        "    @jit.each_event\n    def step(self, e):\n        self.n += 1\n"
        "    @jit.finalize\n    async def done(self):\n        x = await self.out.recv()\n",
        "is a sender",
    ),
    (
        "close in the scan",
        "q: jit.Channel[int] = jit.channel()",
        "    @jit.each_event\n    def step(self, e):\n        self.q.close()\n",
        "ends the sends of every worker",
    ),
    (
        "block in the scan",
        "q: jit.Channel[int] = jit.channel(policy='block')",
        "    @jit.each_event\n    def step(self, e):\n        ok = self.q.try_send(1)\n",
        "never from the per-event body",
    ),
    (
        "send of a block channel is not awaited",
        "q: jit.Channel[int] = jit.channel(policy='block')",
        "    @jit.each_event\n    def step(self, e):\n        self.n += 1\n"
        "    @jit.finalize\n    def done(self):\n        self.q.send(1)\n",
        "waits for a credit",
    ),
    (
        "a float on an int channel",
        "q: jit.Channel[int] = jit.channel()",
        "    @jit.each_event\n    def step(self, e):\n        self.q.send(e.dur / 2)\n",
        "carries an int",
    ),
    (
        "a try_send is awaited",
        "q: jit.Channel[int] = jit.channel()",
        "    @jit.each_event\n    def step(self, e):\n        self.n += 1\n"
        "    @jit.finalize\n    async def done(self):\n        await self.q.try_send(1)\n",
        "never waits",
    ),
    (
        "a recv that is not awaited",
        "q: jit.Channel[int] = jit.channel()",
        "    @jit.each_event\n    def step(self, e):\n        self.n += 1\n"
        "    @jit.finalize\n    async def done(self):\n        x = self.q.recv()\n",
        "await self.q.recv()",
    ),
    (
        "try_send in a condition",
        "q: jit.Channel[int] = jit.channel()",
        "    @jit.each_event\n    def step(self, e):\n        if self.q.try_send(1):\n"
        "            self.n += 1\n",
        "inside an expression or a condition",
    ),
    (
        "seal in the scan",
        "p: jit.Phase[int] = jit.phase()",
        "    @jit.each_event\n    def step(self, e):\n        self.p.seal()\n",
        "open to contributions while the scan runs",
    ),
    (
        "result in the scan",
        "p: jit.Phase[int] = jit.phase()",
        "    @jit.each_event\n    def step(self, e):\n        x = self.p.result(0)\n",
        "open to contributions while the scan runs",
    ),
    (
        "a method a latest does not have",
        "c: jit.Latest[int] = jit.latest()",
        "    @jit.each_event\n    def step(self, e):\n        self.c.contribute(1)\n",
        "a latest port has set, get",
    ),
    (
        "async for over a sender",
        "out: jit.Sender[int] = jit.sender()",
        "    @jit.each_event\n    def step(self, e):\n        self.n += 1\n"
        "    @jit.finalize\n    async def done(self):\n        async for v in self.out:\n"
        "            self.n += v\n",
        "takes a receiver or channel",
    ),
    (
        "the value of get is unused",
        "c: jit.Latest[int] = jit.latest()",
        "    @jit.each_event\n    def step(self, e):\n        self.c.get(0)\n",
        "is unused",
    ),
    (
        "except jit.Closed in the scan",
        "q: jit.Channel[int] = jit.channel()",
        "    @jit.each_event\n    def step(self, e):\n        try:\n            self.n += 1\n"
        "        except jit.Closed:\n            self.n += 2\n",
        "jit.Closed comes from",
    ),
]


@pytest.mark.parametrize(
    "decl, body, expect", [c[1:] for c in _BODY_ERRORS], ids=[c[0] for c in _BODY_ERRORS]
)
def test_a_misused_port_is_a_located_compile_error(decl, body, expect):
    err = _err(_HEAD + f"    {decl}\n" + body)
    assert expect in err.reason


@pytest.mark.parametrize(
    "stmt, expect",
    [
        ("self.q.recv()", "recv() in an each_batch body"),
        ("self.p.seal()", "seal() in an each_batch body"),
        ("self.q.send(df['dur'])", "not a column or a bare number"),
        ("self.q.send(df['dur'].sum() / 2)", "a reduction inside a column expression"),
        ("self.q.send(1)", "not a column or a bare number"),
    ],
)
def test_a_batch_port_call_that_is_not_a_reduction_is_a_located_error(stmt, expect):
    q = "q: jit.Sender[int] = jit.sender()"
    p = "p: jit.Phase[int] = jit.phase()"
    err = _err(
        "from dftracer.utils import jit\n\n@jit.plugin\nclass Bad:\n"
        f"    {q}\n    {p}\n    n: int = 0\n\n"
        "    @jit.each_batch\n    def fold(self, df):\n"
        f"        self.n += df['dur'].count()\n        {stmt}\n"
    )
    assert expect in err.reason


def test_a_block_channel_is_not_used_from_a_batch_body():
    err = _err(
        "from dftracer.utils import jit\n\n@jit.plugin\nclass Bad:\n"
        "    q: jit.Channel[int] = jit.channel(policy='block')\n    n: int = 0\n\n"
        "    @jit.each_batch\n    def fold(self, df):\n"
        "        self.n += df['dur'].count()\n        self.q.send(df['dur'].sum())\n"
    )
    assert "Block channel" in err.reason


def test_a_port_attribute_is_not_scan_state():
    @jit.plugin
    class Quiet:
        q: jit.Channel[int] = jit.channel()
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    assert "q" not in getattr(Quiet, "_jit_config", {})
    assert Quiet._jit_ports["q"].full.endswith(".quiet.q")


def test_a_port_is_not_callable_in_plain_python():
    @jit.plugin
    class Plain:
        q: jit.Channel[int] = jit.channel()
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    with pytest.raises(jit.JitError, match="plugin port"):
        Plain.q.send(1)
