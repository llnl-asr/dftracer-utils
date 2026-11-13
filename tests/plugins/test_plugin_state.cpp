// SPDX-License-Identifier: MIT
// Tier 2: a plugin registers its own mergeable state type and the host drives
// it like an accumulator without knowing its shape - one instance per worker
// slice, merged at the fan-in, spilled past the memory budget, finalized once.
// The shape here (a per-name map) is deliberately one the engine's agg table
// could do; what is under test is the machinery around it, not the shape.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/runtime/runtime.h>

// After fold_adapter.h so nanoarrow is set up before dataframe/c_api.h's
// arrow_abi.
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>
#include <plugins/test_abi_ctx.h>
#include <task_result.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <variant>

using namespace dftracer::utils;

namespace {

constexpr const char* STATE_NAME = "state_test.by_name";
constexpr int GROUPS = 1000;
constexpr int BATCH = 100;
// Small enough that a few dozen distinct names blow past it, so the spill
// engages many times over the batches below.
constexpr std::uint64_t TINY_BUDGET = 4096;
// The plugin's own estimate of one map entry; only its ratio to TINY_BUDGET
// matters.
constexpr std::uint64_t BYTES_PER_ENTRY = 64;

using Totals = dstd::Map<dstd::String, std::uint64_t>;

// "name\0dur\0..." - a self-contained encoding used both for a spill run and
// for the finalized result, so a spilled state and an in-memory one are read
// back the same way.
dstd::String encode(const Totals& t) {
    dstd::String out;
    for (const auto& [name, dur] : t) {
        out.append(name);
        out.push_back('\0');
        out.append(dstd::to_string(dur));
        out.push_back('\0');
    }
    return out;
}

Totals decode(const char* p, std::size_t n) {
    Totals out;
    std::size_t at = 0;
    while (at < n) {
        const dstd::String name(p + at);
        at += name.size() + 1;
        if (at > n) break;
        const dstd::String dur(p + at);
        at += dur.size() + 1;
        out[name] += std::strtoull(dur.c_str(), nullptr, 10);
    }
    return out;
}

/* ---- the plugin's state type, behind the C descriptor ------------------- */

dftu_status state_init(dftu_ctx*, void*, void** out) {
    *out = new Totals();
    return DFTU_OK;
}

dstd::StringView str_at(const dftu_series* col, std::int64_t r) {
    const std::int32_t* off = dftu_series_offsets(col);
    const char* base = static_cast<const char*>(dftu_series_data(col));
    if (!off || !base) return {};
    return {base + off[r], static_cast<std::size_t>(off[r + 1] - off[r])};
}

dftu_status state_update(dftu_ctx* ctx, void* state, const dftu_dataframe* df) {
    dftu_series* name = dftu_dataframe_column(df, "name");
    dftu_series* dur = dftu_dataframe_column(df, "dur");
    if (!name || !dur) {
        if (name) dftu_series_free(name);
        if (dur) dftu_series_free(dur);
        return dftu_fail(ctx, DFTU_E_NOT_FOUND, "batch has no name/dur column");
    }
    dftu_series* flat_name = dftu_series_materialize(name);
    dftu_series* flat_dur = dftu_series_materialize(dur);
    const auto* durs =
        static_cast<const std::uint64_t*>(dftu_series_data(flat_dur));
    auto& t = *static_cast<Totals*>(state);
    for (std::int64_t r = 0; r < dftu_series_length(flat_name); ++r)
        t[dstd::String(str_at(flat_name, r))] += durs[r];
    dftu_series_free(flat_name);
    dftu_series_free(flat_dur);
    dftu_series_free(name);
    dftu_series_free(dur);
    return DFTU_OK;
}

dftu_status state_merge(dftu_ctx*, void* into, void* other) {
    auto& a = *static_cast<Totals*>(into);
    for (const auto& [k, v] : *static_cast<Totals*>(other)) a[k] += v;
    return DFTU_OK;
}

std::uint64_t state_bytes(const void* state) {
    return static_cast<const Totals*>(state)->size() * BYTES_PER_ENTRY;
}

void free_buffer(void* data, void*) { delete[] static_cast<char*>(data); }

dftu_status state_serialize(dftu_ctx*, const void* state, dftu_bytes* out) {
    const dstd::String enc = encode(*static_cast<const Totals*>(state));
    char* buf = new char[enc.size() ? enc.size() : 1];
    std::memcpy(buf, enc.data(), enc.size());
    out->data = buf;
    out->len = enc.size();
    out->free_fn = free_buffer;
    out->ud = nullptr;
    return DFTU_OK;
}

dftu_status state_deserialize(dftu_ctx*, void*, dftu_bytes in, void** out) {
    *out = new Totals(decode(static_cast<const char*>(in.data),
                             static_cast<std::size_t>(in.len)));
    return DFTU_OK;
}

dftu_status state_finalize(dftu_ctx*, void* state, dftu_result_value* out) {
    // Held past the call: the host copies BYTES results, but not until it
    // returns from here.
    static thread_local dstd::String enc;
    enc = encode(*static_cast<Totals*>(state));
    out->kind = DFTU_RESULT_KIND_BYTES;
    out->u.bytes.data = enc.data();
    out->u.bytes.len = enc.size();
    return DFTU_OK;
}

void state_destroy(void* state) { delete static_cast<Totals*>(state); }

dftu_state_desc make_desc(bool spillable) {
    dftu_state_desc d{};
    d.struct_size = sizeof(d);
    d.name = STATE_NAME;
    d.init = state_init;
    d.update = state_update;
    d.merge = state_merge;
    d.bytes = state_bytes;
    if (spillable) {
        d.serialize = state_serialize;
        d.deserialize = state_deserialize;
    }
    d.finalize = state_finalize;
    d.destroy = state_destroy;
    return d;
}

/* ---- the host-side harness ---------------------------------------------- */

// A plugin that contributes nothing but the registered state, so anything the
// run produces came through the tier-2 path.
dftu_plugin state_only_plugin() {
    dftu_plugin p{};
    p.struct_size = sizeof(p);
    p.make_slice = [](dftu_ctx*, void*, void** out) -> dftu_status {
        static int sentinel;
        *out = &sentinel;
        return DFTU_OK;
    };
    p.merge = [](dftu_ctx*, void*, void*) -> dftu_status { return DFTU_OK; };
    p.on_finalize = [](dftu_ctx*, void*, const dftu_plugin_host*,
                       dftu_task**) -> dftu_status { return DFTU_OK; };
    p.destroy_slice = [](void*) {};
    p.destroy = [](void*) {};
    return p;
}

trace::views::detail::FoldEvent evt(dstd::StringIntern& intern,
                                    const dstd::String& name,
                                    std::uint64_t dur) {
    trace::views::detail::FoldEvent fe;
    fe.cat_id = intern.get_or_insert("POSIX");
    fe.name_id = intern.get_or_insert(name);
    fe.phase = trace::RecordPhase::Complete;
    fe.pid = 1;
    fe.tid = 1;
    fe.ts = dur;
    fe.dur = dur;
    fe.has_dur = true;
    return fe;
}

// GROUPS distinct names, each seen once per slice, so every total depends on
// the cross-slice merge as well as on any spill.
dstd::Vector<dstd::Vector<trace::views::detail::FoldEvent>> slices(
    dstd::StringIntern& intern, int n_slices) {
    dstd::Vector<dstd::Vector<trace::views::detail::FoldEvent>> out(
        static_cast<std::size_t>(n_slices));
    for (int s = 0; s < n_slices; ++s)
        for (int i = 0; i < GROUPS; ++i)
            out[static_cast<std::size_t>(s)].push_back(
                evt(intern, "op_" + dstd::to_string(i),
                    static_cast<std::uint64_t>((s + 1) * (i + 1))));
    return out;
}

void finalize_now(plugins::host::PluginFold& f) {
    Runtime rt(1);
    dftu_utils_test::ok(
        rt.submit(coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                      auto finalized = co_await f.finalize(
                          trace::views::detail::CoverageSet{});
                      CHECK(finalized.has_value());
                  }),
                  "fin")
            .wait());
    rt.shutdown();
}

struct RunResult {
    Totals totals;
    std::size_t runs = 0;
    bool refused = false;
};

RunResult run_state(bool spillable, std::uint64_t budget, int n_slices) {
    plugins::host::StateRegistry registry{plugins::host::RegisteredState{
        STATE_NAME, make_desc(spillable), nullptr}};
    dftu_plugin p = state_only_plugin();
    dstd::StringIntern intern;
    plugins::NamedResultRegistry named;
    plugins::host::PluginFold master(&p, intern, nullptr, &named, "state_test",
                                     budget, &registry);
    for (const dstd::Vector<trace::views::detail::FoldEvent>& evs :
         slices(intern, n_slices)) {
        auto slice = master.slice();
        auto* pf = static_cast<plugins::host::PluginFold*>(slice.get());
        trace::views::detail::ScanUnit unit{};
        for (std::size_t at = 0; at < evs.size(); at += BATCH) {
            const std::size_t n = std::min<std::size_t>(BATCH, evs.size() - at);
            dftu_utils_test::ok(pf->step(trace::views::detail::FoldBatch{
                dstd::Span<const trace::views::detail::FoldEvent>(
                    evs.data() + at, n),
                unit,
                {}}));
        }
        dftu_utils_test::ok(master.merge(*pf));
    }

    RunResult out;
    out.runs = master.state_spill_runs(STATE_NAME);
    out.refused = master.state_spill_refused(STATE_NAME);
    finalize_now(master);

    auto it = named.results().find(STATE_NAME);
    REQUIRE(it != named.results().end());
    const auto& bytes = std::get<dstd::Vector<std::byte>>(it->second);
    out.totals =
        decode(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return out;
}

// A plugin whose own slice (not a registered state) grows a fixed amount per
// batch and reports it through dftu_plugin::bytes; with `reclaimable` it
// also gives the host a reclaim that shrinks it by what was asked.
struct GrowingSlice {
    std::uint64_t held = 0;
    std::uint64_t events = 0;
    dstd::Vector<std::uint64_t> reclaims;
};

struct SliceRun {
    std::uint64_t events = 0;
    std::uint64_t held = 0;
    std::size_t reclaims = 0;
    bool refused = false;
};

// Where the master slice reports itself from on_finalize.
SliceRun* g_slice_sink = nullptr;

dftu_plugin growing_plugin(bool reclaimable) {
    dftu_plugin p{};
    p.struct_size = sizeof(p);
    p.make_slice = [](dftu_ctx*, void*, void** out) -> dftu_status {
        *out = new GrowingSlice{};
        return DFTU_OK;
    };
    p.on_batch = [](dftu_ctx*, void* slice, const dftu_dataframe* df,
                    const dftu_plugin_host*) -> dftu_status {
        auto* s = static_cast<GrowingSlice*>(slice);
        s->events += static_cast<std::uint64_t>(dftu_dataframe_num_rows(df));
        s->held += BYTES_PER_ENTRY * static_cast<std::uint64_t>(BATCH);
        return DFTU_OK;
    };
    p.merge = [](dftu_ctx*, void* into, void* other) -> dftu_status {
        auto* a = static_cast<GrowingSlice*>(into);
        auto* b = static_cast<GrowingSlice*>(other);
        a->events += b->events;
        a->held += b->held;
        return DFTU_OK;
    };
    p.on_finalize = [](dftu_ctx*, void* slice, const dftu_plugin_host*,
                       dftu_task**) -> dftu_status {
        auto* s = static_cast<GrowingSlice*>(slice);
        if (g_slice_sink) {
            g_slice_sink->events = s->events;
            g_slice_sink->held = s->held;
        }
        return DFTU_OK;
    };
    p.destroy_slice = [](void* slice) {
        delete static_cast<GrowingSlice*>(slice);
    };
    p.destroy = [](void*) {};
    p.bytes = [](void* slice) -> std::uint64_t {
        return static_cast<GrowingSlice*>(slice)->held;
    };
    if (reclaimable)
        p.reclaim = [](void* slice, std::uint64_t want) -> std::uint64_t {
            auto* s = static_cast<GrowingSlice*>(slice);
            s->reclaims.push_back(want);
            const std::uint64_t freed = std::min(want, s->held);
            s->held -= freed;
            return freed;
        };
    return p;
}

SliceRun run_slice(bool reclaimable, std::uint64_t budget, int n_slices) {
    dftu_plugin p = growing_plugin(reclaimable);
    dstd::StringIntern intern;
    plugins::NamedResultRegistry named;
    plugins::host::PluginFold master(&p, intern, nullptr, &named, "slice_test",
                                     budget, nullptr);
    for (const dstd::Vector<trace::views::detail::FoldEvent>& evs :
         slices(intern, n_slices)) {
        auto slice = master.slice();
        auto* pf = static_cast<plugins::host::PluginFold*>(slice.get());
        trace::views::detail::ScanUnit unit{};
        for (std::size_t at = 0; at < evs.size(); at += BATCH) {
            const std::size_t n = std::min<std::size_t>(BATCH, evs.size() - at);
            dftu_utils_test::ok(pf->step(trace::views::detail::FoldBatch{
                dstd::Span<const trace::views::detail::FoldEvent>(
                    evs.data() + at, n),
                unit,
                {}}));
        }
        dftu_utils_test::ok(master.merge(*pf));
    }
    SliceRun out;
    out.reclaims = master.slice_reclaims();
    out.refused = master.slice_reclaim_refused();
    // The master's slice, every worker folded in, reports from on_finalize.
    g_slice_sink = &out;
    finalize_now(master);
    g_slice_sink = nullptr;
    return out;
}

Totals expected(int n_slices) {
    Totals want;
    for (int s = 0; s < n_slices; ++s)
        for (int i = 0; i < GROUPS; ++i)
            want["op_" + dstd::to_string(i)] +=
                static_cast<std::uint64_t>((s + 1) * (i + 1));
    return want;
}

}  // namespace

TEST_CASE("a registered state merges across worker slices") {
    const RunResult one =
        run_state(true, dftracer::utils::memory::NO_SPILL_BUDGET, 1);
    const RunResult many =
        run_state(true, dftracer::utils::memory::NO_SPILL_BUDGET, 4);

    CHECK(one.runs == 0);
    CHECK(many.runs == 0);
    CHECK(one.totals == expected(1));
    CHECK(many.totals == expected(4));
    // Four slices see four times the work; without the fan-in merge the
    // master would carry only its own (empty) share.
    CHECK(many.totals.size() == static_cast<std::size_t>(GROUPS));
}

TEST_CASE("a serializable state spills past the budget, same result") {
    const RunResult want =
        run_state(true, dftracer::utils::memory::NO_SPILL_BUDGET, 4);
    const RunResult got = run_state(true, TINY_BUDGET, 4);

    // The mechanism, not just the answer: a budget that was silently ignored
    // would still produce the right totals.
    CHECK(got.runs > 0);
    CHECK_FALSE(got.refused);
    CHECK(got.totals == want.totals);
}

TEST_CASE("a state with no serialize pair is refused a spill") {
    const RunResult got = run_state(false, TINY_BUDGET, 4);

    CHECK(got.refused);
    CHECK(got.runs == 0);
    // Refused, not dropped: the state stays whole in memory and still
    // finalizes, but the host says out loud that it could not honour the
    // budget.
    CHECK(got.totals == expected(4));
}

TEST_CASE("a plugin's own slice is asked to reclaim past the budget") {
    const SliceRun free =
        run_slice(true, dftracer::utils::memory::NO_SPILL_BUDGET, 4);
    const SliceRun held = run_slice(true, TINY_BUDGET, 4);

    CHECK(free.reclaims == 0);
    CHECK(free.held == BYTES_PER_ENTRY * GROUPS * 4);
    // Every step and merge that left the slice over budget asked it to
    // shrink, and what it holds at the end is within the budget.
    CHECK(held.reclaims > 0);
    CHECK_FALSE(held.refused);
    CHECK(held.held <= TINY_BUDGET);
    // Reclaiming touched the plugin's memory, not its answer.
    CHECK(held.events == free.events);
    CHECK(held.events == static_cast<std::uint64_t>(GROUPS) * 4);
}

TEST_CASE("a slice that measures itself but cannot reclaim is reported") {
    const SliceRun got = run_slice(false, TINY_BUDGET, 4);

    CHECK(got.refused);
    CHECK(got.reclaims > 0);
    CHECK(got.held == BYTES_PER_ENTRY * GROUPS * 4);
    CHECK(got.events == static_cast<std::uint64_t>(GROUPS) * 4);
}

namespace {
// The C++ facade: a Slice with bytes() and reclaim(want) gets both slots.
struct FacadeSlice {
    explicit FacadeSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftu_dataframe*, dftracer::utils::plugins::Host) {}
    void merge(FacadeSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
    std::uint64_t bytes() const { return 12; }
    std::uint64_t reclaim(std::uint64_t want) { return want > 12 ? 12 : want; }
};
struct PlainSlice {
    explicit PlainSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftu_dataframe*, dftracer::utils::plugins::Host) {}
    void merge(PlainSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};
}  // namespace

TEST_CASE("make_plugin wires bytes and reclaim when the Slice has them") {
    dftu_plugin* with =
        dftracer::utils::plugins::make_plugin<FacadeSlice>(nullptr);
    REQUIRE(with->bytes);
    REQUIRE(with->reclaim);
    test_plugin_common::Ctx c;
    void* s = nullptr;
    REQUIRE(with->make_slice(c.get(), with->self, &s) == DFTU_OK);
    CHECK(with->bytes(s) == 12);
    CHECK(with->reclaim(s, 100) == 12);
    CHECK(with->reclaim(s, 5) == 5);
    with->destroy_slice(s);
    with->destroy(with->self);

    dftu_plugin* without =
        dftracer::utils::plugins::make_plugin<PlainSlice>(nullptr);
    CHECK(without->bytes == nullptr);
    CHECK(without->reclaim == nullptr);
    without->destroy(without->self);
}
