// SPDX-License-Identifier: MIT
// A plugin plan node is a Cursor the engine drives inside the same pull chain
// lower_cursor_chain builds for every built-in op, not a second protocol. These
// tests drive hand-written dftu_node_vt fixtures through the node adapter and
// dftu_lazyframe_op directly (no dlopen), the same style
// test_provider_schema_types.cpp uses for dftu_source_vt.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/abi_error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/node_adapter.h>
#include <dftracer/utils/plugins/host/provider_source.h>
#include <dftracer/utils/plugins/task_abi.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>

using namespace dftracer::utils;

namespace {

dftu_status register_node(const char* name, const dftu_node_vt* vt,
                          void* self) {
    auto registered = plugins::host::register_node_vtable(name, vt, self);
    return registered ? DFTU_OK
                      : dftracer::utils::common::to_status(
                            registered.error().condition());
}

dataframe::DataFrame run(coro::CoroTask<dstd::Result<dataframe::DataFrame>> t) {
    dataframe::DataFrame out = dftu_utils_test::ok(
        dftracer::utils::default_runtime().submit(std::move(t)).get());
    for (dataframe::Series& c : out.columns) c = c.materialize();
    return out;
}

// ---- A resident int64 source, one morsel per collect. ---------------------

struct IntSource {
    dstd::Vector<dstd::String> names;
    dstd::Vector<std::int64_t> col0;
};

dftu_status src_schema(dftu_ctx*, void* self, const char* const** out_names,
                       int32_t* out_count) {
    auto* s = static_cast<IntSource*>(self);
    static thread_local dstd::Vector<const char*> storage;
    storage.clear();
    for (const dstd::String& n : s->names) storage.push_back(n.c_str());
    *out_names = storage.data();
    *out_count = static_cast<int32_t>(storage.size());
    return DFTU_OK;
}

struct SrcCursorState {
    dftu_dataframe* frame;
    bool done = false;
};

dftu_status src_cursor_next(dftu_ctx*, void* self, std::int64_t,
                            dftu_dataframe** out, dftu_task** pending) {
    auto* cs = static_cast<SrcCursorState*>(self);
    *pending = nullptr;
    if (cs->done) {
        *out = nullptr;
        return DFTU_END;
    }
    *out = cs->frame;
    cs->frame = nullptr;
    cs->done = true;
    return DFTU_OK;
}

void src_cursor_destroy(void* self) {
    auto* cs = static_cast<SrcCursorState*>(self);
    if (cs->frame) dftu_dataframe_free(cs->frame);
    delete cs;
}

const dftu_cursor_vt SRC_CURSOR_VT = {sizeof(dftu_cursor_vt),
                                      src_cursor_next,
                                      src_cursor_destroy,
                                      nullptr,
                                      nullptr,
                                      nullptr};

dftu_status src_scan(dftu_ctx*, void* self, const dftu_scan_request*, int32_t*,
                     void** out_cursor_self, const dftu_cursor_vt** out_vt) {
    auto* s = static_cast<IntSource*>(self);
    dftu_series* c = dftu_series_new_flat(
        DFTU_TYPE_INT64, s->col0.data(),
        static_cast<std::int64_t>(s->col0.size()), nullptr);
    const char* names[1] = {s->names[0].c_str()};
    dftu_series* cols[1] = {c};
    auto* cs = new SrcCursorState{dftu_dataframe_new(names, cols, 1)};
    *out_cursor_self = cs;
    *out_vt = &SRC_CURSOR_VT;
    return DFTU_OK;
}

void src_destroy(void*) {}

// The Timestamp column carries a timezone (a parameterized type) so a node
// declaring it via dftu_schema_copy_field must preserve it, not just the id.
dftu_status src_schema_types(dftu_ctx*, void*, dftu_schema* out) {
    dftu_schema_add_field(out, "val", DFTU_TYPE_INT64, 0, DFTU_TIME_UNIT_MICRO,
                          nullptr, 0, 0, 0);
    dftu_schema_add_field(out, "ts", DFTU_TYPE_TIMESTAMP, 1,
                          DFTU_TIME_UNIT_MILLI, "UTC", 0, 0, 0);
    return DFTU_OK;
}

dstd::SharedPtr<dataframe::Source> make_int_source(IntSource& fx, bool typed) {
    dftu_source_vt vt{};
    vt.struct_size = sizeof(vt);
    vt.schema = src_schema;
    vt.scan = src_scan;
    vt.destroy = src_destroy;
    if (typed) vt.schema_types = src_schema_types;
    return plugins::host::make_provider_source(vt, &fx);
}

// ---- "test.double": a streaming node doubling column 0. --------------------

// The upstream's answer to one next(): its status and message, after awaiting
// the task it handed back, if any.
struct Pulled {
    dftu_status status = DFTU_OK;
    dstd::String message;
    dftu_dataframe* frame = nullptr;
};

coro::CoroTask<Pulled> pull(void* in_self, const dftu_cursor_vt* in_vt,
                            std::int64_t max_rows) {
    Pulled pulled;
    plugins::AbiContext ctx;
    dftu_task* t = nullptr;
    pulled.status =
        in_vt->next(ctx.get(), in_self, max_rows, &pulled.frame, &t);
    pulled.message = dstd::String(ctx.message());
    if (t) {
        plugins::AbiTask* at = plugins::abi_task(t);
        auto running = plugins::run_abi_task(at);
        pulled.status = co_await std::move(running);
        pulled.message = at->message;
        plugins::free_abi_task(t);
    }
    co_return std::move(pulled);
}

coro::CoroTask<void> pull_and_double(plugins::AbiTask* task, void* in_self,
                                     const dftu_cursor_vt* in_vt,
                                     std::int64_t max_rows,
                                     dftu_dataframe** out) {
    auto pulling = pull(in_self, in_vt, max_rows);
    Pulled in = co_await std::move(pulling);
    if (in.status == DFTU_END) {
        task->status = DFTU_END;
        co_return;
    }
    if (in.status < 0) {
        task->fail(in.status, in.message);
        co_return;
    }
    dftu_dataframe* frame = in.frame;
    dftu_series* c0 = dftu_dataframe_column(frame, "val");
    const std::int64_t n = dftu_series_length(c0);
    const auto* data = static_cast<const std::int64_t*>(dftu_series_data(c0));
    dstd::Vector<std::int64_t> doubled(data, data + n);
    for (std::int64_t& v : doubled) v *= 2;
    dftu_series_free(c0);
    dftu_series* new_col =
        dftu_series_new_flat(DFTU_TYPE_INT64, doubled.data(), n, nullptr);
    const char* names[1] = {"val"};
    dftu_series* cols[1] = {new_col};
    dftu_dataframe* result = dftu_dataframe_new(names, cols, 1);
    dftu_dataframe_free(frame);
    *out = result;
}

struct DoubleCursorState {
    void* in_self;
    const dftu_cursor_vt* in_vt;
};

dftu_status double_cursor_next(dftu_ctx*, void* self, std::int64_t max_rows,
                               dftu_dataframe** out, dftu_task** pending) {
    auto* st = static_cast<DoubleCursorState*>(self);
    *out = nullptr;
    *pending = plugins::make_abi_task([=](plugins::AbiTask* task) {
        return pull_and_double(task, st->in_self, st->in_vt, max_rows, out);
    });
    return DFTU_OK;
}

void double_cursor_destroy(void* self) {
    auto* st = static_cast<DoubleCursorState*>(self);
    if (st->in_vt->destroy) st->in_vt->destroy(st->in_self);
    delete st;
}

const dftu_cursor_vt DOUBLE_CURSOR_VT = {sizeof(dftu_cursor_vt),
                                         double_cursor_next,
                                         double_cursor_destroy,
                                         nullptr,
                                         nullptr,
                                         nullptr};

dftu_status double_node_open(dftu_ctx*, void*, void* in_self,
                             const dftu_cursor_vt* in_vt, const dftu_op_arg*,
                             std::uint64_t, void** out_self,
                             const dftu_cursor_vt** out_vt) {
    auto* st = new DoubleCursorState{in_self, in_vt};
    *out_self = st;
    *out_vt = &DOUBLE_CURSOR_VT;
    return DFTU_OK;
}

// Pass every input field through unchanged - doubling a value never changes
// its declared type, including a parameterized one (Timestamp + timezone).
dftu_status double_node_schema(dftu_ctx*, void*, const dftu_schema* in,
                               const dftu_op_arg*, dftu_schema* out) {
    const int32_t n = dftu_schema_field_count(in);
    for (int32_t i = 0; i < n; ++i) dftu_schema_copy_field(out, in, i);
    return DFTU_OK;
}

void double_node_destroy(void*) {}

dftu_node_vt make_double_vt() {
    dftu_node_vt vt{};
    vt.struct_size = sizeof(vt);
    vt.output_schema = double_node_schema;
    vt.open = double_node_open;
    vt.destroy = double_node_destroy;
    return vt;
}

// ---- "test.open_fails": open() always fails with a message. ----------

dftu_status failing_node_open(dftu_ctx* ctx, void*, void*,
                              const dftu_cursor_vt*, const dftu_op_arg*,
                              std::uint64_t, void**, const dftu_cursor_vt**) {
    return dftu_fail(ctx, DFTU_E_INVALID, "node refused to open");
}

dftu_status failing_node_schema(dftu_ctx*, void*, const dftu_schema*,
                                const dftu_op_arg*, dftu_schema*) {
    return DFTU_OK;
}

// ---- A node recording only whether it was ever opened. ---------------------

struct RecordingNode {
    bool opened = false;
    std::uint64_t budget = 0;
};

dftu_status recording_node_schema(dftu_ctx*, void*, const dftu_schema* in,
                                  const dftu_op_arg*, dftu_schema* out) {
    const int32_t n = dftu_schema_field_count(in);
    for (int32_t i = 0; i < n; ++i) dftu_schema_copy_field(out, in, i);
    return DFTU_OK;
}

// Reuses the doubler's cursor machinery (values end up doubled); the test
// below only checks that open() ran and where the node sits in explain(),
// not the values.
dftu_status recording_node_open(dftu_ctx*, void* self, void* in_self,
                                const dftu_cursor_vt* in_vt, const dftu_op_arg*,
                                std::uint64_t memory_budget, void** out_self,
                                const dftu_cursor_vt** out_vt) {
    static_cast<RecordingNode*>(self)->opened = true;
    static_cast<RecordingNode*>(self)->budget = memory_budget;
    auto* st = new DoubleCursorState{in_self, in_vt};
    *out_self = st;
    *out_vt = &DOUBLE_CURSOR_VT;
    return DFTU_OK;
}

// ---- "test.sync_probe.*": a transparent node that forwards in_vt->next
// unchanged (same *out, same returned task pointer), recording per-call
// whether the upstream answered with a NULL task. Exercises the
// HostCursorBridge::next_thunk fast path from the plugin-node side of the
// ABI, the same way a real node's own next() would observe it. ----------

struct ProbeCursorState {
    void* in_self;
    const dftu_cursor_vt* in_vt;
    dstd::Vector<bool>* sync_flags;
};

dftu_status probe_cursor_next(dftu_ctx* ctx, void* self, std::int64_t max_rows,
                              dftu_dataframe** out, dftu_task** pending) {
    auto* st = static_cast<ProbeCursorState*>(self);
    const dftu_status status =
        st->in_vt->next(ctx, st->in_self, max_rows, out, pending);
    st->sync_flags->push_back(*pending == nullptr);
    return status;
}

void probe_cursor_destroy(void* self) {
    auto* st = static_cast<ProbeCursorState*>(self);
    if (st->in_vt->destroy) st->in_vt->destroy(st->in_self);
    delete st;
}

const dftu_cursor_vt PROBE_CURSOR_VT = {sizeof(dftu_cursor_vt),
                                        probe_cursor_next,
                                        probe_cursor_destroy,
                                        nullptr,
                                        nullptr,
                                        nullptr};

dftu_status probe_node_schema(dftu_ctx*, void*, const dftu_schema* in,
                              const dftu_op_arg*, dftu_schema* out) {
    const int32_t n = dftu_schema_field_count(in);
    for (int32_t i = 0; i < n; ++i) dftu_schema_copy_field(out, in, i);
    return DFTU_OK;
}

dftu_status probe_node_open(dftu_ctx*, void* self, void* in_self,
                            const dftu_cursor_vt* in_vt, const dftu_op_arg*,
                            std::uint64_t, void** out_self,
                            const dftu_cursor_vt** out_vt) {
    auto* flags = static_cast<dstd::Vector<bool>*>(self);
    auto* st = new ProbeCursorState{in_self, in_vt, flags};
    *out_self = st;
    *out_vt = &PROBE_CURSOR_VT;
    return DFTU_OK;
}

dftu_node_vt make_probe_vt() {
    dftu_node_vt vt{};
    vt.struct_size = sizeof(vt);
    vt.output_schema = probe_node_schema;
    vt.open = probe_node_open;
    return vt;
}

// ---- A source whose cursor records every narrow() it is offered, as the
// mask the predicate selects over its own column. ---------------------------

struct NarrowLog {
    dstd::Vector<dstd::Vector<bool>> masks;
    // A declared resident size and the reclaims the source got for it.
    std::uint64_t bytes = 0;
    dstd::Vector<std::uint64_t> reclaims;
};

struct NarrowSource {
    IntSource src;
    NarrowLog* log;
};

struct NarrowCursorState {
    SrcCursorState base;
    NarrowSource* owner;
};

dftu_status narrow_cursor_next(dftu_ctx* ctx, void* self, std::int64_t max_rows,
                               dftu_dataframe** out, dftu_task** pending) {
    return src_cursor_next(ctx, &static_cast<NarrowCursorState*>(self)->base,
                           max_rows, out, pending);
}

void narrow_cursor_destroy(void* self) {
    auto* cs = static_cast<NarrowCursorState*>(self);
    if (cs->base.frame) dftu_dataframe_free(cs->base.frame);
    delete cs;
}

dftu_status narrow_cursor_narrow(dftu_ctx*, void* self,
                                 const dftu_expr* predicate,
                                 std::int32_t* out_applied,
                                 dftu_task** pending) {
    auto* cs = static_cast<NarrowCursorState*>(self);
    const dstd::Vector<std::int64_t>& v = cs->owner->src.col0;
    dataframe::Series column = dataframe::Series::flat_i64(
        v.data(), static_cast<std::int64_t>(v.size()));
    dataframe::Series mask = dftu_utils_test::ok(
        dataframe::eval(dataframe::expr_handle_unwrap(predicate), {&column}));
    dstd::Vector<bool> bits;
    for (std::int64_t i = 0; i < mask.length(); ++i)
        bits.push_back((mask.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1);
    cs->owner->log->masks.push_back(std::move(bits));
    *out_applied = 1;
    *pending = nullptr;
    return DFTU_OK;
}

std::uint64_t narrow_cursor_bytes(void* self) {
    return static_cast<NarrowCursorState*>(self)->owner->log->bytes;
}

std::uint64_t narrow_cursor_reclaim(void* self, std::uint64_t want) {
    NarrowLog* log = static_cast<NarrowCursorState*>(self)->owner->log;
    log->reclaims.push_back(want);
    const std::uint64_t freed = std::min(want, log->bytes);
    log->bytes -= freed;
    return freed;
}

const dftu_cursor_vt NARROW_CURSOR_VT = {
    sizeof(dftu_cursor_vt), narrow_cursor_next,  narrow_cursor_destroy,
    narrow_cursor_narrow,   narrow_cursor_bytes, narrow_cursor_reclaim};

dftu_status narrow_src_schema(dftu_ctx* ctx, void* self,
                              const char* const** out_names,
                              int32_t* out_count) {
    return src_schema(ctx, &static_cast<NarrowSource*>(self)->src, out_names,
                      out_count);
}

dftu_status narrow_src_scan(dftu_ctx*, void* self, const dftu_scan_request*,
                            int32_t*, void** out_cursor_self,
                            const dftu_cursor_vt** out_vt) {
    auto* s = static_cast<NarrowSource*>(self);
    dftu_series* c = dftu_series_new_flat(
        DFTU_TYPE_INT64, s->src.col0.data(),
        static_cast<std::int64_t>(s->src.col0.size()), nullptr);
    const char* names[1] = {s->src.names[0].c_str()};
    dftu_series* cols[1] = {c};
    auto* cs = new NarrowCursorState{{dftu_dataframe_new(names, cols, 1)}, s};
    *out_cursor_self = cs;
    *out_vt = &NARROW_CURSOR_VT;
    return DFTU_OK;
}

dstd::SharedPtr<dataframe::Source> make_narrow_source(NarrowSource& fx) {
    dftu_source_vt vt{};
    vt.struct_size = sizeof(vt);
    vt.schema = narrow_src_schema;
    vt.scan = narrow_src_scan;
    vt.destroy = src_destroy;
    return plugins::host::make_provider_source(vt, &fx);
}

// ---- "test.forward": a transparent node that forwards narrow() to its
// input, declares a (fake) resident size and records the reclaim it gets.

struct ForwardLog {
    bool reclaim_input = false;
    std::uint64_t bytes = 0;
    dstd::Vector<std::uint64_t> reclaims;
};

struct ForwardCursorState {
    void* in_self;
    const dftu_cursor_vt* in_vt;
    ForwardLog* log;
};

dftu_status forward_cursor_next(dftu_ctx* ctx, void* self,
                                std::int64_t max_rows, dftu_dataframe** out,
                                dftu_task** pending) {
    auto* st = static_cast<ForwardCursorState*>(self);
    return st->in_vt->next(ctx, st->in_self, max_rows, out, pending);
}

void forward_cursor_destroy(void* self) {
    auto* st = static_cast<ForwardCursorState*>(self);
    if (st->in_vt->destroy) st->in_vt->destroy(st->in_self);
    delete st;
}

dftu_status forward_cursor_narrow(dftu_ctx* ctx, void* self,
                                  const dftu_expr* predicate,
                                  std::int32_t* out_applied,
                                  dftu_task** pending) {
    auto* st = static_cast<ForwardCursorState*>(self);
    if (!st->in_vt->narrow) {
        *out_applied = 0;
        *pending = nullptr;
        return DFTU_OK;
    }
    return st->in_vt->narrow(ctx, st->in_self, predicate, out_applied, pending);
}

std::uint64_t forward_cursor_bytes(void* self) {
    return static_cast<ForwardCursorState*>(self)->log->bytes;
}

std::uint64_t forward_cursor_reclaim(void* self, std::uint64_t want) {
    auto* st = static_cast<ForwardCursorState*>(self);
    st->log->reclaims.push_back(want);
    const std::uint64_t freed = std::min(want, st->log->bytes);
    st->log->bytes -= freed;
    if (st->log->reclaim_input && st->in_vt->reclaim)
        st->in_vt->reclaim(st->in_self, want);
    return freed;
}

const dftu_cursor_vt FORWARD_CURSOR_VT = {
    sizeof(dftu_cursor_vt), forward_cursor_next,  forward_cursor_destroy,
    forward_cursor_narrow,  forward_cursor_bytes, forward_cursor_reclaim};

dftu_status forward_node_schema(dftu_ctx*, void*, const dftu_schema* in,
                                const dftu_op_arg*, dftu_schema* out) {
    const int32_t n = dftu_schema_field_count(in);
    for (int32_t i = 0; i < n; ++i) dftu_schema_copy_field(out, in, i);
    return DFTU_OK;
}

dftu_status forward_node_open(dftu_ctx*, void* self, void* in_self,
                              const dftu_cursor_vt* in_vt, const dftu_op_arg*,
                              std::uint64_t, void** out_self,
                              const dftu_cursor_vt** out_vt) {
    auto* st =
        new ForwardCursorState{in_self, in_vt, static_cast<ForwardLog*>(self)};
    *out_self = st;
    *out_vt = &FORWARD_CURSOR_VT;
    return DFTU_OK;
}

dftu_node_vt make_forward_vt() {
    dftu_node_vt vt{};
    vt.struct_size = sizeof(vt);
    vt.output_schema = forward_node_schema;
    vt.open = forward_node_open;
    return vt;
}

dataframe::DataFrame right_keys(dstd::Vector<std::int64_t> keys) {
    dataframe::DataFrame df;
    df.names = dstd::make_shared_strings({"key"});
    df.columns.push_back(dataframe::Series::flat_i64(
        keys.data(), static_cast<std::int64_t>(keys.size())));
    return df;
}

// A Source handing out a fresh Cursor from `factory` per scan(), so a test
// can drive the probe node over a hand-written host-native Cursor fixture
// (a resident cursor that answers via try_next, or one that genuinely
// suspends via coro::yield).
class CursorSource : public dataframe::Source {
   public:
    explicit CursorSource(
        dstd::Function<dstd::UniquePtr<dataframe::Cursor>()> factory)
        : factory_(std::move(factory)) {}
    dataframe::Schema schema() const override {
        return {{dataframe::Field{
            "val", dataframe::scalar(dataframe::TypeId::Int64), true}}};
    }
    dstd::Result<dataframe::ScanResult> scan(
        const dataframe::ScanRequest& req) const override {
        dataframe::ScanResult r;
        r.cursor = factory_();
        r.filters.assign(req.filters.size(), dataframe::Pushed::No);
        return r;
    }

   private:
    dstd::Function<dstd::UniquePtr<dataframe::Cursor>()> factory_;
};

// A cursor from outside the library whose advisory hooks throw. Its data is
// the six rows 1..6, in one morsel.
struct HookFaults {
    int narrows = 0;
    int reclaims = 0;
};

class ThrowingHooksCursor : public dataframe::Cursor {
   public:
    explicit ThrowingHooksCursor(HookFaults* faults) : faults_(faults) {}

    coro::CoroTask<dataframe::MorselResult> next(std::int64_t) override {
        if (done_) co_return dataframe::MorselResult(dstd::nullopt);
        done_ = true;
        dstd::Vector<std::int64_t> v{1, 2, 3, 4, 5, 6};
        dataframe::Morsel m;
        m.columns.push_back(
            dataframe::Series::flat_i64(v.data(), 6).materialize());
        m.rows = 6;
        co_return dataframe::MorselResult(std::move(m));
    }
    coro::CoroTask<bool> narrow(const dataframe::Expr&) override {
        ++faults_->narrows;
        throw std::runtime_error("narrow boom");
        co_return false;
    }
    std::uint64_t resident_bytes() const override { return 1; }
    coro::CoroTask<std::uint64_t> reclaim(std::uint64_t) override {
        ++faults_->reclaims;
        throw std::runtime_error("reclaim boom");
        co_return 0;
    }

   private:
    HookFaults* faults_;
    bool done_ = false;
};

dataframe::LazyFrame throwing_hooks_source(HookFaults& faults) {
    return dataframe::LazyFrame::scan(dstd::make_shared<CursorSource>(
        [&faults]() -> dstd::UniquePtr<dataframe::Cursor> {
            return dstd::make_unique<ThrowingHooksCursor>(&faults);
        }));
}

// Answers synchronously via try_next: one row, then end of stream (or, when
// `fail_on_call` matches, an error instead of a morsel).
class ThrowingSyncCursor : public dataframe::Cursor {
   public:
    explicit ThrowingSyncCursor(int fail_on_call = -1)
        : fail_on_call_(fail_on_call) {}

    coro::CoroTask<dataframe::MorselResult> next(
        std::int64_t max_rows) override {
        dataframe::MorselResult out(dstd::nullopt);
        fill(max_rows, out);
        co_return std::move(out);
    }
    bool try_next(std::int64_t max_rows,
                  dataframe::MorselResult& out) override {
        fill(max_rows, out);
        return true;
    }

   private:
    void fill(std::int64_t, dataframe::MorselResult& out) {
        ++calls_;
        if (calls_ == fail_on_call_) {
            out = dftracer::utils::common::make_error<"sync boom">(
                dftracer::utils::common::ErrorCode::Io);
            return;
        }
        if (calls_ > 1) {
            out = dataframe::MorselResult(dstd::nullopt);
            return;
        }
        dstd::Vector<std::int64_t> v{9};
        dataframe::Morsel m;
        m.columns.push_back(dataframe::Series::flat_i64(v.data(), 1));
        m.rows = 1;
        out = dataframe::MorselResult(std::move(m));
    }

    int fail_on_call_;
    int calls_ = 0;
};

// Genuinely suspends every call (co_await coro::yield() always reschedules
// on the executor), then answers exactly like ThrowingSyncCursor's data
// shape so the two are comparable value-for-value.
class ThrowingAsyncCursor : public dataframe::Cursor {
   public:
    explicit ThrowingAsyncCursor(int throw_on_call = -1)
        : throw_on_call_(throw_on_call) {}

    coro::CoroTask<dataframe::MorselResult> next(std::int64_t) override {
        co_await dftracer::utils::coro::yield();
        ++calls_;
        if (calls_ == throw_on_call_) throw std::runtime_error("async boom");
        if (calls_ > 1) co_return dataframe::MorselResult(dstd::nullopt);
        dstd::Vector<std::int64_t> v{9};
        dataframe::Morsel m;
        m.columns.push_back(dataframe::Series::flat_i64(v.data(), 1));
        m.rows = 1;
        co_return dataframe::MorselResult(std::move(m));
    }

   private:
    int throw_on_call_;
    int calls_ = 0;
};

// ---- Multi-morsel resident/streaming/hybrid sources for the try_next
// propagation tests: a chain of transforms (filter/select/with_column) over
// each of these must take the same fast/slow path its source does. ----------

dstd::Vector<std::int64_t> multi_morsel_data() {
    return {1, 2, 3, 4, 5, 6, 7, 8, 9};
}

// Answers every call via try_next: `chunk` rows at a time until exhausted.
class MultiSyncCursor : public dataframe::Cursor {
   public:
    MultiSyncCursor(dstd::Vector<std::int64_t> data, std::int64_t chunk)
        : data_(std::move(data)), chunk_(chunk) {}
    coro::CoroTask<dataframe::MorselResult> next(std::int64_t) override {
        dstd::Optional<dataframe::Morsel> out;
        fill(out);
        co_return dataframe::MorselResult(std::move(out));
    }
    bool try_next(std::int64_t, dataframe::MorselResult& out) override {
        dstd::Optional<dataframe::Morsel> morsel;
        fill(morsel);
        out = dataframe::MorselResult(std::move(morsel));
        return true;
    }

   private:
    void fill(dstd::Optional<dataframe::Morsel>& out) {
        const auto n64 = static_cast<std::int64_t>(data_.size());
        if (off_ >= n64) {
            out.reset();
            return;
        }
        const std::int64_t n = std::min(chunk_, n64 - off_);
        dataframe::Morsel m;
        m.columns.push_back(
            dataframe::Series::flat_i64(data_.data() + off_, n));
        m.rows = n;
        off_ += n;
        out = std::move(m);
    }
    dstd::Vector<std::int64_t> data_;
    std::int64_t chunk_;
    std::int64_t off_ = 0;
};

// Genuinely suspends on every call (try_next always false, the Cursor
// default): `chunk` rows at a time until exhausted.
class MultiAsyncCursor : public dataframe::Cursor {
   public:
    MultiAsyncCursor(dstd::Vector<std::int64_t> data, std::int64_t chunk)
        : data_(std::move(data)), chunk_(chunk) {}
    coro::CoroTask<dataframe::MorselResult> next(std::int64_t) override {
        co_await dftracer::utils::coro::yield();
        dstd::Optional<dataframe::Morsel> out;
        const auto n64 = static_cast<std::int64_t>(data_.size());
        if (off_ < n64) {
            const std::int64_t n = std::min(chunk_, n64 - off_);
            dataframe::Morsel m;
            m.columns.push_back(
                dataframe::Series::flat_i64(data_.data() + off_, n));
            m.rows = n;
            off_ += n;
            out = std::move(m);
        }
        co_return dataframe::MorselResult(std::move(out));
    }

   private:
    dstd::Vector<std::int64_t> data_;
    std::int64_t chunk_;
    std::int64_t off_ = 0;
};

// The SpoolReader shape: answers the first `sync_rows` rows via try_next
// (resident prefix), then genuinely suspends for the remainder (as if it had
// fallen through to a spilled run on disk).
class SyncThenAsyncCursor : public dataframe::Cursor {
   public:
    SyncThenAsyncCursor(dstd::Vector<std::int64_t> data, std::int64_t chunk,
                        std::int64_t sync_rows)
        : data_(std::move(data)), chunk_(chunk), sync_rows_(sync_rows) {}

    coro::CoroTask<dataframe::MorselResult> next(
        std::int64_t max_rows) override {
        dataframe::MorselResult sync(dstd::nullopt);
        if (try_next(max_rows, sync)) co_return std::move(sync);
        co_await dftracer::utils::coro::yield();
        dstd::Optional<dataframe::Morsel> out;
        fill(out);
        co_return dataframe::MorselResult(std::move(out));
    }

    bool try_next(std::int64_t, dataframe::MorselResult& out) override {
        if (off_ >= sync_rows_) return false;
        dstd::Optional<dataframe::Morsel> morsel;
        fill(morsel);
        out = dataframe::MorselResult(std::move(morsel));
        return true;
    }

   private:
    void fill(dstd::Optional<dataframe::Morsel>& out) {
        const auto n64 = static_cast<std::int64_t>(data_.size());
        if (off_ >= n64) {
            out.reset();
            return;
        }
        const std::int64_t n = std::min(chunk_, n64 - off_);
        dataframe::Morsel m;
        m.columns.push_back(
            dataframe::Series::flat_i64(data_.data() + off_, n));
        m.rows = n;
        off_ += n;
        out = std::move(m);
    }
    dstd::Vector<std::int64_t> data_;
    std::int64_t chunk_, sync_rows_;
    std::int64_t off_ = 0;
};

// The chain the fast-path propagation tests all drive: filter (keep
// everything) -> select "val" -> with_column "doubled" = val * 2 -> the sync
// probe node. Exercises FilterCursor, SelectCursor and WithColumnCursor's
// try_next in series over whatever source cursor is handed in.
dataframe::LazyFrame build_probe_chain(dstd::SharedPtr<dataframe::Source> src,
                                       const char* node_name) {
    return dataframe::LazyFrame::scan(src)
        .filter(dataframe::col(0) > std::int64_t{0})
        .select({"val"})
        .with_column("doubled",
                     dataframe::col(0) * dataframe::lit(std::int64_t{2}))
        .op(node_name, dataframe::OpArgs());
}

void check_multi_morsel_output(const dataframe::DataFrame& out) {
    REQUIRE(out.num_rows() == 9);
    REQUIRE(out.columns.size() == 2);
    for (std::int64_t i = 0; i < 9; ++i) {
        CHECK(out.columns[0].data<std::int64_t>()[i] == i + 1);
        CHECK(out.columns[1].data<std::int64_t>()[i] == (i + 1) * 2);
    }
}

}  // namespace

TEST_SUITE("lazyframe plugin node") {
    TEST_CASE("a streaming node doubles values through the pull chain") {
        REQUIRE(register_node("test.double", nullptr, nullptr) != 0);
        dftu_node_vt vt = make_double_vt();
        REQUIRE(register_node("test.double.a", &vt, nullptr) == 0);

        IntSource fx{{"val"}, {1, 2, 3, 4}};
        dataframe::LazyFrame lf =
            dataframe::LazyFrame::scan(make_int_source(fx, false))
                .op("test.double.a", dataframe::OpArgs());
        dataframe::DataFrame out = run(lf.collect());
        REQUIRE(out.columns.size() == 1);
        REQUIRE(out.num_rows() == 4);
        CHECK(out.columns[0].data<std::int64_t>()[0] == 2);
        CHECK(out.columns[0].data<std::int64_t>()[1] == 4);
        CHECK(out.columns[0].data<std::int64_t>()[2] == 6);
        CHECK(out.columns[0].data<std::int64_t>()[3] == 8);

        CHECK(dftu_node_unregister("test.double.a") == 0);
        CHECK(dftu_node_unregister("test.double.a") != 0);
    }

    TEST_CASE(
        "re-registering the same name is refused; unregister then "
        "re-register works") {
        dftu_node_vt vt = make_double_vt();
        REQUIRE(register_node("test.double.b", &vt, nullptr) == 0);
        CHECK(register_node("test.double.b", &vt, nullptr) != 0);
        REQUIRE(dftu_node_unregister("test.double.b") == 0);
        CHECK(register_node("test.double.b", &vt, nullptr) == 0);
        CHECK(dftu_node_unregister("test.double.b") == 0);
    }

    TEST_CASE(
        "output_schema over a plan reports the node's declared types, "
        "including a parameterized type") {
        dftu_node_vt vt = make_double_vt();
        REQUIRE(register_node("test.double.c", &vt, nullptr) == 0);

        // Schema-only: output_schema() never scans, so the source need not
        // actually carry a "ts" column, only declare it.
        IntSource fx{{"val", "ts"}, {1, 2, 3}};
        dataframe::LazyFrame lf =
            dataframe::LazyFrame::scan(make_int_source(fx, true))
                .op("test.double.c", dataframe::OpArgs());
        dataframe::Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[0].name == "val");
        CHECK(s.fields[0].type.id == dataframe::TypeId::Int64);
        CHECK(s.fields[1].name == "ts");
        CHECK(s.fields[1].type.id == dataframe::TypeId::Timestamp);
        CHECK(s.fields[1].type.time_unit() == dataframe::TimeUnit::Milli);
        CHECK(s.fields[1].type.timezone() == "UTC");

        CHECK(dftu_node_unregister("test.double.c") == 0);
    }

    TEST_CASE("explain names the node") {
        dftu_node_vt vt = make_double_vt();
        REQUIRE(register_node("test.double.d", &vt, nullptr) == 0);

        IntSource fx{{"val"}, {1}};
        dataframe::LazyFrame lf =
            dataframe::LazyFrame::scan(make_int_source(fx, false))
                .op("test.double.d", dataframe::OpArgs());
        dstd::String explained = lf.explain();
        CHECK(explained.find("op test.double.d") != dstd::String::npos);

        CHECK(dftu_node_unregister("test.double.d") == 0);
    }

    TEST_CASE("an unknown node name is a clear error") {
        IntSource fx{{"val"}, {1}};
        dataframe::LazyFrame base =
            dataframe::LazyFrame::scan(make_int_source(fx, false));
        dftu_utils_test::expect_error(
            base.op("no.such.node", dataframe::OpArgs()).status(),
            dftracer::utils::common::ErrorCode::InvalidArgument);

        dftu_lazyframe* base_c =
            dftu_lazyframe_from_provider("no.such.provider");
        CHECK(base_c == nullptr);
    }

    TEST_CASE(
        "a node whose open() fails is a clear error at drive "
        "time") {
        dftu_node_vt vt{};
        vt.struct_size = sizeof(vt);
        vt.output_schema = failing_node_schema;
        vt.open = failing_node_open;
        REQUIRE(register_node("test.fails", &vt, nullptr) == 0);

        IntSource fx{{"val"}, {1, 2}};
        dataframe::LazyFrame lf =
            dataframe::LazyFrame::scan(make_int_source(fx, false))
                .op("test.fails", dataframe::OpArgs());
        const auto result =
            dftracer::utils::default_runtime().submit(lf.collect()).get();
        REQUIRE_FALSE(result.has_value());
        CHECK(
            dstd::String(result.error().c_str()).find("node refused to open") !=
            dstd::String::npos);

        CHECK(dftu_node_unregister("test.fails") == 0);
    }

    TEST_CASE("a filter placed after a node is not pushed through it") {
        dftu_node_vt vt = make_double_vt();
        REQUIRE(register_node("test.double.e", &vt, nullptr) == 0);

        IntSource fx{{"val"}, {1, 2, 3, 4, 5}};
        dataframe::LazyFrame lf =
            dataframe::LazyFrame::scan(make_int_source(fx, false))
                .op("test.double.e", dataframe::OpArgs())
                .filter(dataframe::col(0) > std::int64_t{5});
        dstd::String explained = lf.explain();
        // Structural: the node line still precedes the filter line, so
        // pushdown never hoisted the filter across the opaque node.
        std::size_t op_pos = explained.find("op test.double.e");
        std::size_t filter_pos = explained.find("filter");
        REQUIRE(op_pos != dstd::String::npos);
        REQUIRE(filter_pos != dstd::String::npos);
        CHECK(op_pos < filter_pos);

        // Functional: the filter must see doubled values (it runs on the
        // node's OUTPUT), not the source's original column.
        dataframe::DataFrame out = run(lf.collect());
        REQUIRE(out.num_rows() == 3);  // doubled: 2,4,6,8,10 -> >5: 6,8,10
        CHECK(out.columns[0].data<std::int64_t>()[0] == 6);
        CHECK(out.columns[0].data<std::int64_t>()[1] == 8);
        CHECK(out.columns[0].data<std::int64_t>()[2] == 10);

        CHECK(dftu_node_unregister("test.double.e") == 0);
    }

    TEST_CASE(
        "a select placed after a node is not pushed into the scan "
        "across it") {
        RecordingNode rec;
        dftu_node_vt vt{};
        vt.struct_size = sizeof(vt);
        vt.output_schema = recording_node_schema;
        vt.open = recording_node_open;
        REQUIRE(register_node("test.double.f", &vt, &rec) == 0);

        IntSource fx{{"val"}, {10, 20}};
        dataframe::LazyFrame lf =
            dataframe::LazyFrame::scan(make_int_source(fx, false))
                .op("test.double.f", dataframe::OpArgs())
                .select({"val"});
        dstd::String explained = lf.explain();
        std::size_t scan_pos = explained.find("scan");
        std::size_t op_pos = explained.find("op test.double.f");
        std::size_t select_pos = explained.find("select");
        REQUIRE(scan_pos != dstd::String::npos);
        REQUIRE(op_pos != dstd::String::npos);
        REQUIRE(select_pos != dstd::String::npos);
        // If pushdown had (incorrectly) hoisted the select across the node,
        // it would appear as a projection right after "scan", before "op".
        CHECK(scan_pos < op_pos);
        CHECK(op_pos < select_pos);

        dataframe::DataFrame out = run(lf.collect());
        REQUIRE(out.num_rows() == 2);
        CHECK(rec.opened);

        CHECK(dftu_node_unregister("test.double.f") == 0);
    }

    TEST_CASE("a node's open gets the plan's memory budget") {
        RecordingNode rec;
        dftu_node_vt vt{};
        vt.struct_size = sizeof(vt);
        vt.output_schema = recording_node_schema;
        vt.open = recording_node_open;
        REQUIRE(register_node("test.double.budget", &vt, &rec) == 0);

        IntSource fx{{"val"}, {10, 20}};
        dataframe::LazyFrame base =
            dataframe::LazyFrame::scan(make_int_source(fx, false))
                .op("test.double.budget", dataframe::OpArgs());

        constexpr std::uint64_t BUDGET = 3u << 20;
        REQUIRE(run(base.memory_budget(BUDGET).collect()).num_rows() == 2);
        CHECK(rec.budget == BUDGET);

        rec.budget = 0;
        REQUIRE(run(base.collect()).num_rows() == 2);
        CHECK(rec.budget > 0);

        CHECK(dftu_node_unregister("test.double.budget") == 0);
    }

    TEST_CASE("a breaker node drains its input and emits one frame") {
        struct CountState {
            void* in_self;
            const dftu_cursor_vt* in_vt;
            bool done = false;
        };

        struct Impl {
            static coro::CoroTask<void> drain_and_count(plugins::AbiTask* task,
                                                        CountState* st,
                                                        dftu_dataframe** out) {
                std::int64_t total = 0;
                for (;;) {
                    auto pulling = pull(st->in_self, st->in_vt, 1 << 20);
                    Pulled in = co_await std::move(pulling);
                    if (in.status == DFTU_END) break;
                    if (in.status < 0) {
                        task->fail(in.status, in.message);
                        co_return;
                    }
                    total += dftu_dataframe_num_rows(in.frame);
                    dftu_dataframe_free(in.frame);
                }
                dftu_series* c =
                    dftu_series_new_flat(DFTU_TYPE_INT64, &total, 1, nullptr);
                const char* names[1] = {"count"};
                dftu_series* cols[1] = {c};
                *out = dftu_dataframe_new(names, cols, 1);
            }

            static dftu_status next(dftu_ctx*, void* self, std::int64_t,
                                    dftu_dataframe** out, dftu_task** pending) {
                auto* st = static_cast<CountState*>(self);
                *out = nullptr;
                *pending = nullptr;
                if (st->done) return DFTU_END;
                st->done = true;
                *pending = plugins::make_abi_task([=](plugins::AbiTask* task) {
                    return drain_and_count(task, st, out);
                });
                return DFTU_OK;
            }
            static void destroy(void* self) {
                auto* st = static_cast<CountState*>(self);
                if (st->in_vt->destroy) st->in_vt->destroy(st->in_self);
                delete st;
            }
        };

        static const dftu_cursor_vt COUNT_VT = {sizeof(dftu_cursor_vt),
                                                Impl::next,
                                                Impl::destroy,
                                                nullptr,
                                                nullptr,
                                                nullptr};

        struct NodeImpl {
            static dftu_status schema(dftu_ctx*, void*, const dftu_schema*,
                                      const dftu_op_arg*, dftu_schema* out) {
                dftu_schema_add_field(out, "count", DFTU_TYPE_INT64, 0,
                                      DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
                return DFTU_OK;
            }
            static dftu_status open(dftu_ctx*, void*, void* in_self,
                                    const dftu_cursor_vt* in_vt,
                                    const dftu_op_arg*, std::uint64_t,
                                    void** out_self,
                                    const dftu_cursor_vt** out_vt) {
                auto* st = new CountState{in_self, in_vt};
                *out_self = st;
                *out_vt = &COUNT_VT;
                return DFTU_OK;
            }
        };

        dftu_node_vt vt{};
        vt.struct_size = sizeof(vt);
        vt.output_schema = NodeImpl::schema;
        vt.open = NodeImpl::open;
        REQUIRE(register_node("test.count_breaker", &vt, nullptr) == 0);

        IntSource fx{{"val"}, {1, 2, 3, 4, 5, 6, 7}};
        dataframe::LazyFrame lf =
            dataframe::LazyFrame::scan(make_int_source(fx, false))
                .op("test.count_breaker", dataframe::OpArgs());
        dataframe::DataFrame out = run(lf.collect());
        REQUIRE(out.num_rows() == 1);
        CHECK(out.columns[0].data<std::int64_t>()[0] == 7);

        CHECK(dftu_node_unregister("test.count_breaker") == 0);
    }

    TEST_CASE(
        "a node over a resident source observes next() returning NULL "
        "(sync path)") {
        dstd::Vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(register_node("test.sync_probe.resident", &vt, &sync_flags) ==
                0);

        dataframe::DataFrame df;
        df.names = dstd::make_shared_strings({"val"});
        dstd::Vector<std::int64_t> v{1, 2, 3};
        df.columns.push_back(dataframe::Series::flat_i64(v.data(), 3));

        dataframe::LazyFrame lf =
            df.lazy().op("test.sync_probe.resident", dataframe::OpArgs());
        dataframe::DataFrame out = run(lf.collect());

        REQUIRE(out.num_rows() == 3);
        CHECK(out.columns[0].data<std::int64_t>()[0] == 1);
        CHECK(out.columns[0].data<std::int64_t>()[1] == 2);
        CHECK(out.columns[0].data<std::int64_t>()[2] == 3);

        // The defect under test: an in-memory upstream can always answer
        // without suspending, so the node's own in_vt->next() call must get
        // a NULL task back every time, never a heap-allocated one it then
        // has to await for data it already had.
        REQUIRE(!sync_flags.empty());
        for (bool synced : sync_flags) CHECK(synced);

        CHECK(dftu_node_unregister("test.sync_probe.resident") == 0);
    }

    TEST_CASE(
        "a node over a genuinely suspending source still gets a task and "
        "the right values") {
        dstd::Vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(register_node("test.sync_probe.async", &vt, &sync_flags) == 0);

        auto src = dstd::make_shared<CursorSource>(
            [] { return dstd::make_unique<ThrowingAsyncCursor>(); });
        dataframe::LazyFrame lf = dataframe::LazyFrame::scan(src).op(
            "test.sync_probe.async", dataframe::OpArgs());
        dataframe::DataFrame out = run(lf.collect());

        REQUIRE(out.num_rows() == 1);
        CHECK(out.columns[0].data<std::int64_t>()[0] == 9);

        // Every call genuinely suspended (coro::yield always reschedules),
        // so the node's in_vt->next() must have gotten a real task back.
        REQUIRE(!sync_flags.empty());
        for (bool synced : sync_flags) CHECK_FALSE(synced);

        CHECK(dftu_node_unregister("test.sync_probe.async") == 0);
    }

    TEST_CASE(
        "an upstream error is reported identically on the sync and async "
        "paths") {
        // Unchecked here: the probe node requires a live self to record into,
        // not nullptr.
        dstd::Vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(register_node("test.sync_probe.err", &vt, &sync_flags) == 0);

        {
            auto src = dstd::make_shared<CursorSource>(
                [] { return dstd::make_unique<ThrowingSyncCursor>(2); });
            dataframe::LazyFrame lf = dataframe::LazyFrame::scan(src).op(
                "test.sync_probe.err", dataframe::OpArgs());
            const auto result =
                dftracer::utils::default_runtime().submit(lf.collect()).get();
            REQUIRE_FALSE(result.has_value());
            CHECK(dstd::String(result.error().c_str()).find("sync boom") !=
                  dstd::String::npos);
        }
        {
            auto src = dstd::make_shared<CursorSource>(
                [] { return dstd::make_unique<ThrowingAsyncCursor>(2); });
            dataframe::LazyFrame lf = dataframe::LazyFrame::scan(src).op(
                "test.sync_probe.err", dataframe::OpArgs());
            const auto result =
                dftracer::utils::default_runtime().submit(lf.collect()).get();
            REQUIRE_FALSE(result.has_value());
            CHECK(dstd::String(result.error().c_str()).find("async boom") !=
                  dstd::String::npos);
        }

        CHECK(dftu_node_unregister("test.sync_probe.err") == 0);
    }

    TEST_CASE(
        "a resident multi-stage chain (filter/select/with_column) takes the "
        "sync path for every call") {
        dstd::Vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(register_node("test.sync_probe.chain.resident", &vt,
                              &sync_flags) == 0);

        auto src = dstd::make_shared<CursorSource>([] {
            return dstd::make_unique<MultiSyncCursor>(multi_morsel_data(), 3);
        });
        dataframe::DataFrame out = run(
            build_probe_chain(src, "test.sync_probe.chain.resident").collect());
        check_multi_morsel_output(out);

        REQUIRE(!sync_flags.empty());
        for (bool synced : sync_flags) CHECK(synced);

        CHECK(dftu_node_unregister("test.sync_probe.chain.resident") == 0);
    }

    TEST_CASE(
        "a genuinely suspending multi-stage chain takes the async path "
        "throughout and produces identical values") {
        dstd::Vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(register_node("test.sync_probe.chain.async", &vt,
                              &sync_flags) == 0);

        auto src = dstd::make_shared<CursorSource>([] {
            return dstd::make_unique<MultiAsyncCursor>(multi_morsel_data(), 3);
        });
        dataframe::DataFrame out = run(
            build_probe_chain(src, "test.sync_probe.chain.async").collect());
        check_multi_morsel_output(out);

        REQUIRE(!sync_flags.empty());
        for (bool synced : sync_flags) CHECK_FALSE(synced);

        CHECK(dftu_node_unregister("test.sync_probe.chain.async") == 0);
    }

    TEST_CASE(
        "a chain that starts resident and becomes async partway (the "
        "SpoolReader shape) produces correct values across the transition") {
        dstd::Vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(register_node("test.sync_probe.chain.hybrid", &vt,
                              &sync_flags) == 0);

        auto src = dstd::make_shared<CursorSource>([] {
            return dstd::make_unique<SyncThenAsyncCursor>(multi_morsel_data(),
                                                          3, 3);
        });
        dataframe::DataFrame out = run(
            build_probe_chain(src, "test.sync_probe.chain.hybrid").collect());
        check_multi_morsel_output(out);

        // The first morsel (3 rows) is answered by the resident prefix; every
        // call after the transition must genuinely suspend. try_next never
        // silently claims a synchronous answer for data it had to await.
        REQUIRE(sync_flags.size() >= 2);
        CHECK(sync_flags.front());
        for (std::size_t i = 1; i < sync_flags.size(); ++i)
            CHECK_FALSE(sync_flags[i]);

        CHECK(dftu_node_unregister("test.sync_probe.chain.hybrid") == 0);
    }

    TEST_CASE(
        "an upstream error through the filter/select/with_column chain is "
        "reported identically on the sync and async paths") {
        dstd::Vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(register_node("test.sync_probe.chain.err", &vt, &sync_flags) ==
                0);

        {
            auto src = dstd::make_shared<CursorSource>(
                [] { return dstd::make_unique<ThrowingSyncCursor>(2); });
            dataframe::LazyFrame lf =
                build_probe_chain(src, "test.sync_probe.chain.err");
            const auto result =
                dftracer::utils::default_runtime().submit(lf.collect()).get();
            REQUIRE_FALSE(result.has_value());
            CHECK(dstd::String(result.error().c_str()).find("sync boom") !=
                  dstd::String::npos);
        }
        {
            auto src = dstd::make_shared<CursorSource>(
                [] { return dstd::make_unique<ThrowingAsyncCursor>(2); });
            dataframe::LazyFrame lf =
                build_probe_chain(src, "test.sync_probe.chain.err");
            const auto result =
                dftracer::utils::default_runtime().submit(lf.collect()).get();
            REQUIRE_FALSE(result.has_value());
            CHECK(dstd::String(result.error().c_str()).find("async boom") !=
                  dstd::String::npos);
        }

        CHECK(dftu_node_unregister("test.sync_probe.chain.err") == 0);
    }

    TEST_CASE(
        "a join narrows the source cursor to its build keys through the "
        "streaming ops between them") {
        NarrowLog log;
        NarrowSource fx{{{"val"}, {1, 2, 3, 4, 5, 6}}, &log};
        dataframe::LazyFrame right = right_keys({2, 4, 4}).lazy();

        SUBCASE("inner join: the set of build keys, positional to the source") {
            dataframe::LazyFrame lf =
                dataframe::LazyFrame::scan(make_narrow_source(fx))
                    .with_column("twice", dataframe::col(0) *
                                              dataframe::lit(std::int64_t{2}))
                    .filter(dataframe::col(0) > std::int64_t{1})
                    .with_row_index("row")
                    .join(right, {"val"}, {"key"}, dataframe::JoinHow::Inner);
            dataframe::DataFrame out = run(lf.collect());
            REQUIRE(out.num_rows() == 3);  // 2, 4, 4

            REQUIRE(log.masks.size() == 1);
            const dstd::Vector<bool> want{false, true,  false,
                                          true,  false, false};
            CHECK(log.masks[0] == want);
        }

        SUBCASE("a left join keeps every left row, so nothing is offered") {
            dataframe::LazyFrame lf =
                dataframe::LazyFrame::scan(make_narrow_source(fx))
                    .join(right, {"val"}, {"key"}, dataframe::JoinHow::Left);
            dataframe::DataFrame out = run(lf.collect());
            REQUIRE(out.num_rows() == 7);
            CHECK(log.masks.empty());
        }

        SUBCASE("a join that matches null keys offers no narrowing") {
            // The build keys leave out nulls, so a left row with a null key
            // would be pruned although it matches a null build key.
            dataframe::LazyFrame lf =
                dataframe::LazyFrame::scan(make_narrow_source(fx))
                    .join(right, {"val"}, {"key"}, dataframe::JoinHow::Inner,
                          "_right", true);
            dataframe::DataFrame out = run(lf.collect());
            REQUIRE(out.num_rows() == 3);  // 2, 4, 4
            CHECK(log.masks.empty());
        }

        SUBCASE("a predicate on the produced column stops at with_column") {
            // The join key is the derived column, which the source does not
            // carry; the offer must not be forwarded as if it were col 0.
            dataframe::LazyFrame lf =
                dataframe::LazyFrame::scan(make_narrow_source(fx))
                    .with_column("twice", dataframe::col(0) *
                                              dataframe::lit(std::int64_t{2}))
                    .join(right, {"twice"}, {"key"}, dataframe::JoinHow::Inner);
            dataframe::DataFrame out = run(lf.collect());
            REQUIRE(out.num_rows() == 3);  // twice == 2 (1), twice == 4 (2, 2)
            CHECK(log.masks.empty());
        }
    }

    TEST_CASE(
        "a plugin node forwards narrow() to its input and is reclaimed "
        "against the plan's budget") {
        ForwardLog flog;
        flog.bytes = std::uint64_t{1} << 40;
        dftu_node_vt vt = make_forward_vt();
        REQUIRE(register_node("test.forward", &vt, &flog) == 0);

        NarrowLog log;
        NarrowSource fx{{{"val"}, {1, 2, 3, 4, 5, 6}}, &log};
        dataframe::LazyFrame right = right_keys({3, 5}).lazy();
        dataframe::LazyFrame lf =
            dataframe::LazyFrame::scan(make_narrow_source(fx))
                .op("test.forward", dataframe::OpArgs())
                .join(right, {"val"}, {"key"}, dataframe::JoinHow::Inner)
                .memory_budget(64 * 1024 * 1024);
        dataframe::DataFrame out = run(lf.collect());
        REQUIRE(out.num_rows() == 2);

        // The offer went through the node's own narrow slot to the source.
        REQUIRE(log.masks.size() == 1);
        const dstd::Vector<bool> want{false, false, true, false, true, false};
        CHECK(log.masks[0] == want);

        // The node's declared size cannot be reserved under the budget, so
        // its governor asked it for all of it at the next morsel.
        REQUIRE(!flog.reclaims.empty());
        CHECK(flog.reclaims[0] == std::uint64_t{1} << 40);
        CHECK(flog.bytes == 0);

        CHECK(dftu_node_unregister("test.forward") == 0);
    }

    TEST_CASE(
        "a provider under a plugin node is still measured and reclaimed") {
        // The node owns its input cursor once open() succeeds. The provider
        // has its own place in the governor, so it is measured and asked
        // wherever it ends up.
        constexpr std::uint64_t BUDGET = 64 * 1024 * 1024;
        ForwardLog flog;  // the node itself holds nothing
        dftu_node_vt vt = make_forward_vt();
        REQUIRE(register_node("test.forward.under", &vt, &flog) == 0);

        NarrowLog log;
        log.bytes = std::uint64_t{1} << 40;
        NarrowSource fx{{{"val"}, {1, 2, 3, 4, 5, 6}}, &log};
        dataframe::LazyFrame lf =
            dataframe::LazyFrame::scan(make_narrow_source(fx))
                .op("test.forward.under", dataframe::OpArgs())
                .op("test.forward.under",
                    dataframe::OpArgs())  // two nodes deep
                .with_column("twice", dataframe::col(0) *
                                          dataframe::lit(std::int64_t{2}))
                .memory_budget(BUDGET);
        dataframe::DataFrame out = run(lf.collect());
        REQUIRE(out.num_rows() == 6);

        CHECK(flog.reclaims.empty());
        REQUIRE(!log.reclaims.empty());
        CHECK(log.reclaims[0] == std::uint64_t{1} << 40);
        CHECK(log.bytes == 0);

        CHECK(dftu_node_unregister("test.forward.under") == 0);
    }

    TEST_CASE("a narrow that throws is logged and the join still answers") {
        HookFaults faults;
        dataframe::LazyFrame lf = throwing_hooks_source(faults).join(
            right_keys({2, 4, 4}).lazy(), {"val"}, {"key"},
            dataframe::JoinHow::Inner);
        dataframe::DataFrame out = run(lf.collect());
        CHECK(faults.narrows == 1);
        CHECK(out.num_rows() == 3);
    }

    TEST_CASE("a reclaim that throws is logged and counts as nothing freed") {
        HookFaults faults;
        ForwardLog flog;
        flog.reclaim_input = true;
        flog.bytes = std::uint64_t{1} << 40;
        dftu_node_vt vt = make_forward_vt();
        REQUIRE(register_node("test.forward.throwing", &vt, &flog) == 0);

        dataframe::LazyFrame lf =
            throwing_hooks_source(faults)
                .op("test.forward.throwing", dataframe::OpArgs())
                .memory_budget(64 * 1024 * 1024);
        dataframe::DataFrame out = run(lf.collect());
        CHECK(out.num_rows() == 6);
        CHECK(!flog.reclaims.empty());
        CHECK(faults.reclaims >= 1);

        CHECK(dftu_node_unregister("test.forward.throwing") == 0);
    }
}
