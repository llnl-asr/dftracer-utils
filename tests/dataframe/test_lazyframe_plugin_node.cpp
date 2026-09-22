// dftu_node_register/dftu_lazyframe_op: a plugin plan node is a Cursor the
// engine drives inside the same pull chain lower_cursor_chain builds for
// every built-in op, not a second protocol. These tests drive hand-written
// dftu_node_vt fixtures through the public C ABI directly (no dlopen), the
// same style test_provider_schema_types.cpp uses for dftu_source_vt.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/task_abi.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>
#include <dftracer/utils/dataframe/internal/provider_source.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using dftracer::utils::coro::CoroTask;
using dftracer::utils::dataframe::col;
using dftracer::utils::dataframe::Cursor;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::JoinHow;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::make_provider_source;
using dftracer::utils::dataframe::Morsel;
using dftracer::utils::dataframe::OpArgs;
using dftracer::utils::dataframe::Schema;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::Source;
using dftracer::utils::dataframe::TimeUnit;
using dftracer::utils::dataframe::TypeId;

namespace {

DataFrame run(CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

// ---- A resident int64 source, one morsel per collect. ---------------------

struct IntSource {
    std::vector<std::string> names;
    std::vector<std::int64_t> col0;
};

int32_t src_schema(void* self, const char* const** out_names) {
    auto* s = static_cast<IntSource*>(self);
    static thread_local std::vector<const char*> storage;
    storage.clear();
    for (const std::string& n : s->names) storage.push_back(n.c_str());
    *out_names = storage.data();
    return static_cast<int32_t>(storage.size());
}

struct SrcCursorState {
    dftu_dataframe* frame;
    bool done = false;
};

dftu_task* src_cursor_next(void* self, std::int64_t, dftu_result_frame* out) {
    auto* cs = static_cast<SrcCursorState*>(self);
    out->ok = 1;
    if (cs->done) {
        out->u.value = nullptr;
    } else {
        out->u.value = cs->frame;
        cs->frame = nullptr;
        cs->done = true;
    }
    return nullptr;
}

void src_cursor_destroy(void* self) {
    auto* cs = static_cast<SrcCursorState*>(self);
    if (cs->frame) dftu_dataframe_free(cs->frame);
    delete cs;
}

const dftu_cursor_vt SRC_CURSOR_VT = {src_cursor_next, src_cursor_destroy,
                                      nullptr, nullptr, nullptr};

void* src_scan(void* self, const dftu_scan_request*, int32_t*,
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
    return cs;
}

void src_destroy(void*) {}

// The Timestamp column carries a timezone (a parameterized type) so a node
// declaring it via dftu_schema_copy_field must preserve it, not just the id.
void src_schema_types(void* self, dftu_schema* out) {
    auto* s = static_cast<IntSource*>(self);
    (void)s;
    dftu_schema_add_field(out, "val", DFTU_TYPE_INT64, 0, DFTU_TIME_UNIT_MICRO,
                          nullptr, 0, 0, 0);
    dftu_schema_add_field(out, "ts", DFTU_TYPE_TIMESTAMP, 1,
                          DFTU_TIME_UNIT_MILLI, "UTC", 0, 0, 0);
}

std::shared_ptr<Source> make_int_source(IntSource& fx, bool typed) {
    dftu_source_vt vt{};
    vt.schema = src_schema;
    vt.scan = src_scan;
    vt.destroy = src_destroy;
    if (typed) vt.schema_types = src_schema_types;
    return make_provider_source(vt, &fx);
}

// ---- "test.double": a streaming node doubling column 0. --------------------

CoroTask<void> pull_and_double(void* in_self, const dftu_cursor_vt* in_vt,
                               std::int64_t max_rows, dftu_result_frame* out) {
    dftu_result_frame in_out{};
    if (dftu_task* t = in_vt->next(in_self, max_rows, &in_out)) {
        auto* task = reinterpret_cast<CoroTask<void>*>(t);
        co_await *task;
        delete task;
    }
    if (!DFTU_RESULT_OK(in_out)) {
        *out = in_out;
        co_return;
    }
    dftu_dataframe* frame = DFTU_RESULT_VALUE(in_out);
    if (!frame) {
        out->ok = 1;
        out->u.value = nullptr;
        co_return;
    }
    dftu_series* c0 = dftu_dataframe_column(frame, "val");
    const std::int64_t n = dftu_series_length(c0);
    const auto* data = static_cast<const std::int64_t*>(dftu_series_data(c0));
    std::vector<std::int64_t> doubled(data, data + n);
    for (std::int64_t& v : doubled) v *= 2;
    dftu_series_free(c0);
    dftu_series* new_col =
        dftu_series_new_flat(DFTU_TYPE_INT64, doubled.data(), n, nullptr);
    const char* names[1] = {"val"};
    dftu_series* cols[1] = {new_col};
    dftu_dataframe* result = dftu_dataframe_new(names, cols, 1);
    dftu_dataframe_free(frame);
    out->ok = 1;
    out->u.value = result;
}

struct DoubleCursorState {
    void* in_self;
    const dftu_cursor_vt* in_vt;
};

dftu_task* double_cursor_next(void* self, std::int64_t max_rows,
                              dftu_result_frame* out) {
    auto* st = static_cast<DoubleCursorState*>(self);
    return dftracer::utils::task_to_abi(
        pull_and_double(st->in_self, st->in_vt, max_rows, out));
}

void double_cursor_destroy(void* self) {
    auto* st = static_cast<DoubleCursorState*>(self);
    if (st->in_vt->destroy) st->in_vt->destroy(st->in_self);
    delete st;
}

const dftu_cursor_vt DOUBLE_CURSOR_VT = {
    double_cursor_next, double_cursor_destroy, nullptr, nullptr, nullptr};

void* double_node_open(void*, void* in_self, const dftu_cursor_vt* in_vt,
                       const dftu_op_arg*, void** out_self,
                       const dftu_cursor_vt** out_vt) {
    auto* st = new DoubleCursorState{in_self, in_vt};
    *out_self = st;
    *out_vt = &DOUBLE_CURSOR_VT;
    return st;
}

// Pass every input field through unchanged - doubling a value never changes
// its declared type, including a parameterized one (Timestamp + timezone).
void double_node_schema(void*, const dftu_schema* in, const dftu_op_arg*,
                        dftu_schema* out) {
    const int32_t n = dftu_schema_field_count(in);
    for (int32_t i = 0; i < n; ++i) dftu_schema_copy_field(out, in, i);
}

void double_node_destroy(void*) {}

dftu_node_vt make_double_vt() {
    dftu_node_vt vt{};
    vt.output_schema = double_node_schema;
    vt.open = double_node_open;
    vt.destroy = double_node_destroy;
    return vt;
}

// ---- "test.open_fails": open() always returns the NULL sentinel. ----------

void*  // NOLINT(readability-non-const-parameter)
failing_node_open(void*, void*, const dftu_cursor_vt*, const dftu_op_arg*,
                  void**, const dftu_cursor_vt**) {
    return nullptr;
}

void failing_node_schema(void*, const dftu_schema*, const dftu_op_arg*,
                         dftu_schema*) {}

// ---- A node recording only whether it was ever opened. ---------------------

struct RecordingNode {
    bool opened = false;
};

void recording_node_schema(void*, const dftu_schema* in, const dftu_op_arg*,
                           dftu_schema* out) {
    const int32_t n = dftu_schema_field_count(in);
    for (int32_t i = 0; i < n; ++i) dftu_schema_copy_field(out, in, i);
}

// Reuses the doubler's cursor machinery (values end up doubled); the test
// below only checks that open() ran and where the node sits in explain(),
// not the values.
void* recording_node_open(void* self, void* in_self,
                          const dftu_cursor_vt* in_vt, const dftu_op_arg*,
                          void** out_self, const dftu_cursor_vt** out_vt) {
    static_cast<RecordingNode*>(self)->opened = true;
    auto* st = new DoubleCursorState{in_self, in_vt};
    *out_self = st;
    *out_vt = &DOUBLE_CURSOR_VT;
    return st;
}

// ---- "test.sync_probe.*": a transparent node that forwards in_vt->next
// unchanged (same *out, same returned task pointer), recording per-call
// whether the upstream answered with a NULL task. Exercises the
// HostCursorBridge::next_thunk fast path from the plugin-node side of the
// ABI, the same way a real node's own next() would observe it. ----------

struct ProbeCursorState {
    void* in_self;
    const dftu_cursor_vt* in_vt;
    std::vector<bool>* sync_flags;
};

dftu_task* probe_cursor_next(void* self, std::int64_t max_rows,
                             dftu_result_frame* out) {
    auto* st = static_cast<ProbeCursorState*>(self);
    dftu_task* t = st->in_vt->next(st->in_self, max_rows, out);
    st->sync_flags->push_back(t == nullptr);
    return t;
}

void probe_cursor_destroy(void* self) {
    auto* st = static_cast<ProbeCursorState*>(self);
    if (st->in_vt->destroy) st->in_vt->destroy(st->in_self);
    delete st;
}

const dftu_cursor_vt PROBE_CURSOR_VT = {probe_cursor_next, probe_cursor_destroy,
                                        nullptr, nullptr, nullptr};

void probe_node_schema(void*, const dftu_schema* in, const dftu_op_arg*,
                       dftu_schema* out) {
    const int32_t n = dftu_schema_field_count(in);
    for (int32_t i = 0; i < n; ++i) dftu_schema_copy_field(out, in, i);
}

void* probe_node_open(void* self, void* in_self, const dftu_cursor_vt* in_vt,
                      const dftu_op_arg*, void** out_self,
                      const dftu_cursor_vt** out_vt) {
    auto* flags = static_cast<std::vector<bool>*>(self);
    auto* st = new ProbeCursorState{in_self, in_vt, flags};
    *out_self = st;
    *out_vt = &PROBE_CURSOR_VT;
    return st;
}

dftu_node_vt make_probe_vt() {
    dftu_node_vt vt{};
    vt.output_schema = probe_node_schema;
    vt.open = probe_node_open;
    return vt;
}

// ---- A source whose cursor records every narrow() it is offered, as the
// mask the predicate selects over its own column. ---------------------------

struct NarrowLog {
    std::vector<std::vector<bool>> masks;
    // A declared resident size and the reclaims the source got for it.
    std::uint64_t bytes = 0;
    std::vector<std::uint64_t> reclaims;
};

struct NarrowSource {
    IntSource src;
    NarrowLog* log;
};

struct NarrowCursorState {
    SrcCursorState base;
    NarrowSource* owner;
};

dftu_task* narrow_cursor_next(void* self, std::int64_t max_rows,
                              dftu_result_frame* out) {
    return src_cursor_next(&static_cast<NarrowCursorState*>(self)->base,
                           max_rows, out);
}

void narrow_cursor_destroy(void* self) {
    auto* cs = static_cast<NarrowCursorState*>(self);
    if (cs->base.frame) dftu_dataframe_free(cs->base.frame);
    delete cs;
}

dftu_task* narrow_cursor_narrow(void* self, const dftu_expr* predicate,
                                std::int32_t* out_applied) {
    auto* cs = static_cast<NarrowCursorState*>(self);
    const std::vector<std::int64_t>& v = cs->owner->src.col0;
    Series column =
        Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
    Series mask = dftracer::utils::dataframe::eval(
        dftracer::utils::dataframe::expr_handle_unwrap(predicate), {&column});
    std::vector<bool> bits;
    for (std::int64_t i = 0; i < mask.length(); ++i)
        bits.push_back((mask.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1);
    cs->owner->log->masks.push_back(std::move(bits));
    *out_applied = 1;
    return nullptr;
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
    narrow_cursor_next, narrow_cursor_destroy, narrow_cursor_narrow,
    narrow_cursor_bytes, narrow_cursor_reclaim};

int32_t narrow_src_schema(void* self, const char* const** out_names) {
    return src_schema(&static_cast<NarrowSource*>(self)->src, out_names);
}

void* narrow_src_scan(void* self, const dftu_scan_request*, int32_t*,
                      void** out_cursor_self, const dftu_cursor_vt** out_vt) {
    auto* s = static_cast<NarrowSource*>(self);
    dftu_series* c = dftu_series_new_flat(
        DFTU_TYPE_INT64, s->src.col0.data(),
        static_cast<std::int64_t>(s->src.col0.size()), nullptr);
    const char* names[1] = {s->src.names[0].c_str()};
    dftu_series* cols[1] = {c};
    auto* cs = new NarrowCursorState{{dftu_dataframe_new(names, cols, 1)}, s};
    *out_cursor_self = cs;
    *out_vt = &NARROW_CURSOR_VT;
    return cs;
}

std::shared_ptr<Source> make_narrow_source(NarrowSource& fx) {
    dftu_source_vt vt{};
    vt.schema = narrow_src_schema;
    vt.scan = narrow_src_scan;
    vt.destroy = src_destroy;
    return make_provider_source(vt, &fx);
}

// ---- "test.forward": a transparent node that forwards narrow() to its
// input, declares a (fake) resident size and records the reclaim it gets.

struct ForwardLog {
    std::uint64_t bytes = 0;
    std::vector<std::uint64_t> reclaims;
};

struct ForwardCursorState {
    void* in_self;
    const dftu_cursor_vt* in_vt;
    ForwardLog* log;
};

dftu_task* forward_cursor_next(void* self, std::int64_t max_rows,
                               dftu_result_frame* out) {
    auto* st = static_cast<ForwardCursorState*>(self);
    return st->in_vt->next(st->in_self, max_rows, out);
}

void forward_cursor_destroy(void* self) {
    auto* st = static_cast<ForwardCursorState*>(self);
    if (st->in_vt->destroy) st->in_vt->destroy(st->in_self);
    delete st;
}

dftu_task* forward_cursor_narrow(void* self, const dftu_expr* predicate,
                                 std::int32_t* out_applied) {
    auto* st = static_cast<ForwardCursorState*>(self);
    if (!st->in_vt->narrow) {
        *out_applied = 0;
        return nullptr;
    }
    return st->in_vt->narrow(st->in_self, predicate, out_applied);
}

std::uint64_t forward_cursor_bytes(void* self) {
    return static_cast<ForwardCursorState*>(self)->log->bytes;
}

std::uint64_t forward_cursor_reclaim(void* self, std::uint64_t want) {
    auto* st = static_cast<ForwardCursorState*>(self);
    st->log->reclaims.push_back(want);
    const std::uint64_t freed = std::min(want, st->log->bytes);
    st->log->bytes -= freed;
    return freed;
}

const dftu_cursor_vt FORWARD_CURSOR_VT = {
    forward_cursor_next, forward_cursor_destroy, forward_cursor_narrow,
    forward_cursor_bytes, forward_cursor_reclaim};

void forward_node_schema(void*, const dftu_schema* in, const dftu_op_arg*,
                         dftu_schema* out) {
    const int32_t n = dftu_schema_field_count(in);
    for (int32_t i = 0; i < n; ++i) dftu_schema_copy_field(out, in, i);
}

void* forward_node_open(void* self, void* in_self, const dftu_cursor_vt* in_vt,
                        const dftu_op_arg*, void** out_self,
                        const dftu_cursor_vt** out_vt) {
    auto* st =
        new ForwardCursorState{in_self, in_vt, static_cast<ForwardLog*>(self)};
    *out_self = st;
    *out_vt = &FORWARD_CURSOR_VT;
    return st;
}

dftu_node_vt make_forward_vt() {
    dftu_node_vt vt{};
    vt.output_schema = forward_node_schema;
    vt.open = forward_node_open;
    return vt;
}

DataFrame right_keys(std::vector<std::int64_t> keys) {
    DataFrame df;
    df.names = {"key"};
    df.columns.push_back(
        Series::flat_i64(keys.data(), static_cast<std::int64_t>(keys.size())));
    return df;
}

// A Source handing out a fresh Cursor from `factory` per scan(), so a test
// can drive the probe node over a hand-written host-native Cursor fixture
// (a resident cursor that answers via try_next, or one that genuinely
// suspends via coro::yield).
class CursorSource : public Source {
   public:
    explicit CursorSource(std::function<std::unique_ptr<Cursor>()> factory)
        : factory_(std::move(factory)) {}
    Schema schema() const override {
        return {{dftracer::utils::dataframe::Field{
            "val", dftracer::utils::dataframe::scalar(TypeId::Int64), true}}};
    }
    dftracer::utils::dataframe::ScanResult scan(
        const dftracer::utils::dataframe::ScanRequest& req) const override {
        dftracer::utils::dataframe::ScanResult r;
        r.cursor = factory_();
        r.filters.assign(req.filters.size(),
                         dftracer::utils::dataframe::Pushed::No);
        return r;
    }

   private:
    std::function<std::unique_ptr<Cursor>()> factory_;
};

// Answers synchronously via try_next: one row, then end of stream (or, when
// `throw_on_call` matches, a synchronous exception instead of a morsel).
class ThrowingSyncCursor : public Cursor {
   public:
    explicit ThrowingSyncCursor(int throw_on_call = -1)
        : throw_on_call_(throw_on_call) {}

    CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        std::optional<Morsel> out;
        fill(max_rows, out);
        co_return out;
    }
    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        fill(max_rows, out);
        return true;
    }

   private:
    void fill(std::int64_t, std::optional<Morsel>& out) {
        ++calls_;
        if (calls_ == throw_on_call_) throw std::runtime_error("sync boom");
        if (calls_ > 1) {
            out.reset();
            return;
        }
        std::vector<std::int64_t> v{9};
        Morsel m;
        m.columns.push_back(Series::flat_i64(v.data(), 1));
        m.rows = 1;
        out = std::move(m);
    }

    int throw_on_call_;
    int calls_ = 0;
};

// Genuinely suspends every call (co_await coro::yield() always reschedules
// on the executor), then answers exactly like ThrowingSyncCursor's data
// shape so the two are comparable value-for-value.
class ThrowingAsyncCursor : public Cursor {
   public:
    explicit ThrowingAsyncCursor(int throw_on_call = -1)
        : throw_on_call_(throw_on_call) {}

    CoroTask<std::optional<Morsel>> next(std::int64_t) override {
        co_await dftracer::utils::coro::yield();
        ++calls_;
        if (calls_ == throw_on_call_) throw std::runtime_error("async boom");
        if (calls_ > 1) co_return std::nullopt;
        std::vector<std::int64_t> v{9};
        Morsel m;
        m.columns.push_back(Series::flat_i64(v.data(), 1));
        m.rows = 1;
        co_return m;
    }

   private:
    int throw_on_call_;
    int calls_ = 0;
};

// ---- Multi-morsel resident/streaming/hybrid sources for the try_next
// propagation tests: a chain of transforms (filter/select/with_column) over
// each of these must take the same fast/slow path its source does. ----------

std::vector<std::int64_t> multi_morsel_data() {
    return {1, 2, 3, 4, 5, 6, 7, 8, 9};
}

// Answers every call via try_next: `chunk` rows at a time until exhausted.
class MultiSyncCursor : public Cursor {
   public:
    MultiSyncCursor(std::vector<std::int64_t> data, std::int64_t chunk)
        : data_(std::move(data)), chunk_(chunk) {}
    CoroTask<std::optional<Morsel>> next(std::int64_t) override {
        std::optional<Morsel> out;
        fill(out);
        co_return out;
    }
    bool try_next(std::int64_t, std::optional<Morsel>& out) override {
        fill(out);
        return true;
    }

   private:
    void fill(std::optional<Morsel>& out) {
        const auto n64 = static_cast<std::int64_t>(data_.size());
        if (off_ >= n64) {
            out.reset();
            return;
        }
        const std::int64_t n = std::min(chunk_, n64 - off_);
        Morsel m;
        m.columns.push_back(Series::flat_i64(data_.data() + off_, n));
        m.rows = n;
        off_ += n;
        out = std::move(m);
    }
    std::vector<std::int64_t> data_;
    std::int64_t chunk_;
    std::int64_t off_ = 0;
};

// Genuinely suspends on every call (try_next always false, the Cursor
// default): `chunk` rows at a time until exhausted.
class MultiAsyncCursor : public Cursor {
   public:
    MultiAsyncCursor(std::vector<std::int64_t> data, std::int64_t chunk)
        : data_(std::move(data)), chunk_(chunk) {}
    CoroTask<std::optional<Morsel>> next(std::int64_t) override {
        co_await dftracer::utils::coro::yield();
        std::optional<Morsel> out;
        const auto n64 = static_cast<std::int64_t>(data_.size());
        if (off_ < n64) {
            const std::int64_t n = std::min(chunk_, n64 - off_);
            Morsel m;
            m.columns.push_back(Series::flat_i64(data_.data() + off_, n));
            m.rows = n;
            off_ += n;
            out = std::move(m);
        }
        co_return out;
    }

   private:
    std::vector<std::int64_t> data_;
    std::int64_t chunk_;
    std::int64_t off_ = 0;
};

// The SpoolReader shape: answers the first `sync_rows` rows via try_next
// (resident prefix), then genuinely suspends for the remainder (as if it had
// fallen through to a spilled run on disk).
class SyncThenAsyncCursor : public Cursor {
   public:
    SyncThenAsyncCursor(std::vector<std::int64_t> data, std::int64_t chunk,
                        std::int64_t sync_rows)
        : data_(std::move(data)), chunk_(chunk), sync_rows_(sync_rows) {}

    CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        std::optional<Morsel> out;
        if (try_next(max_rows, out)) co_return out;
        co_await dftracer::utils::coro::yield();
        fill(out);
        co_return out;
    }

    bool try_next(std::int64_t, std::optional<Morsel>& out) override {
        if (off_ >= sync_rows_) return false;
        fill(out);
        return true;
    }

   private:
    void fill(std::optional<Morsel>& out) {
        const auto n64 = static_cast<std::int64_t>(data_.size());
        if (off_ >= n64) {
            out.reset();
            return;
        }
        const std::int64_t n = std::min(chunk_, n64 - off_);
        Morsel m;
        m.columns.push_back(Series::flat_i64(data_.data() + off_, n));
        m.rows = n;
        off_ += n;
        out = std::move(m);
    }
    std::vector<std::int64_t> data_;
    std::int64_t chunk_, sync_rows_;
    std::int64_t off_ = 0;
};

// The chain the fast-path propagation tests all drive: filter (keep
// everything) -> select "val" -> with_column "doubled" = val * 2 -> the sync
// probe node. Exercises FilterCursor, SelectCursor and WithColumnCursor's
// try_next in series over whatever source cursor is handed in.
LazyFrame build_probe_chain(std::shared_ptr<Source> src,
                            const char* node_name) {
    return LazyFrame::scan(src)
        .filter(col(0) > std::int64_t{0})
        .select({"val"})
        .with_column("doubled",
                     col(0) * dftracer::utils::dataframe::lit(std::int64_t{2}))
        .op(node_name, OpArgs());
}

void check_multi_morsel_output(const DataFrame& out) {
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
        REQUIRE(dftu_node_register("test.double", nullptr, nullptr) != 0);
        dftu_node_vt vt = make_double_vt();
        REQUIRE(dftu_node_register("test.double.a", &vt, nullptr) == 0);

        IntSource fx{{"val"}, {1, 2, 3, 4}};
        LazyFrame lf = LazyFrame::scan(make_int_source(fx, false))
                           .op("test.double.a", OpArgs());
        DataFrame out = run(lf.collect());
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
        REQUIRE(dftu_node_register("test.double.b", &vt, nullptr) == 0);
        CHECK(dftu_node_register("test.double.b", &vt, nullptr) != 0);
        REQUIRE(dftu_node_unregister("test.double.b") == 0);
        CHECK(dftu_node_register("test.double.b", &vt, nullptr) == 0);
        CHECK(dftu_node_unregister("test.double.b") == 0);
    }

    TEST_CASE(
        "output_schema over a plan reports the node's declared types, "
        "including a parameterized type") {
        dftu_node_vt vt = make_double_vt();
        REQUIRE(dftu_node_register("test.double.c", &vt, nullptr) == 0);

        // Schema-only: output_schema() never scans, so the source need not
        // actually carry a "ts" column, only declare it.
        IntSource fx{{"val", "ts"}, {1, 2, 3}};
        LazyFrame lf = LazyFrame::scan(make_int_source(fx, true))
                           .op("test.double.c", OpArgs());
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[0].name == "val");
        CHECK(s.fields[0].type.id == TypeId::Int64);
        CHECK(s.fields[1].name == "ts");
        CHECK(s.fields[1].type.id == TypeId::Timestamp);
        CHECK(s.fields[1].type.time_unit == TimeUnit::Milli);
        CHECK(s.fields[1].type.timezone == "UTC");

        CHECK(dftu_node_unregister("test.double.c") == 0);
    }

    TEST_CASE("explain names the node") {
        dftu_node_vt vt = make_double_vt();
        REQUIRE(dftu_node_register("test.double.d", &vt, nullptr) == 0);

        IntSource fx{{"val"}, {1}};
        LazyFrame lf = LazyFrame::scan(make_int_source(fx, false))
                           .op("test.double.d", OpArgs());
        std::string explained = lf.explain();
        CHECK(explained.find("op test.double.d") != std::string::npos);

        CHECK(dftu_node_unregister("test.double.d") == 0);
    }

    TEST_CASE("an unknown node name is a clear error") {
        IntSource fx{{"val"}, {1}};
        LazyFrame base = LazyFrame::scan(make_int_source(fx, false));
        CHECK_THROWS_AS(base.op("no.such.node", OpArgs()),
                        std::invalid_argument);

        dftu_lazyframe* base_c =
            dftu_lazyframe_from_provider("no.such.provider");
        CHECK(base_c == nullptr);
    }

    TEST_CASE(
        "a node whose open() returns NULL is a clear error at drive "
        "time") {
        dftu_node_vt vt{};
        vt.output_schema = failing_node_schema;
        vt.open = failing_node_open;
        REQUIRE(dftu_node_register("test.fails", &vt, nullptr) == 0);

        IntSource fx{{"val"}, {1, 2}};
        LazyFrame lf = LazyFrame::scan(make_int_source(fx, false))
                           .op("test.fails", OpArgs());
        CHECK_THROWS_AS(run(lf.collect()), std::runtime_error);

        CHECK(dftu_node_unregister("test.fails") == 0);
    }

    TEST_CASE("a filter placed after a node is not pushed through it") {
        dftu_node_vt vt = make_double_vt();
        REQUIRE(dftu_node_register("test.double.e", &vt, nullptr) == 0);

        IntSource fx{{"val"}, {1, 2, 3, 4, 5}};
        LazyFrame lf = LazyFrame::scan(make_int_source(fx, false))
                           .op("test.double.e", OpArgs())
                           .filter(col(0) > std::int64_t{5});
        std::string explained = lf.explain();
        // Structural: the node line still precedes the filter line, so
        // pushdown never hoisted the filter across the opaque node.
        std::size_t op_pos = explained.find("op test.double.e");
        std::size_t filter_pos = explained.find("filter");
        REQUIRE(op_pos != std::string::npos);
        REQUIRE(filter_pos != std::string::npos);
        CHECK(op_pos < filter_pos);

        // Functional: the filter must see doubled values (it runs on the
        // node's OUTPUT), not the source's original column.
        DataFrame out = run(lf.collect());
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
        vt.output_schema = recording_node_schema;
        vt.open = recording_node_open;
        REQUIRE(dftu_node_register("test.double.f", &vt, &rec) == 0);

        IntSource fx{{"val"}, {10, 20}};
        LazyFrame lf = LazyFrame::scan(make_int_source(fx, false))
                           .op("test.double.f", OpArgs())
                           .select({"val"});
        std::string explained = lf.explain();
        std::size_t scan_pos = explained.find("scan");
        std::size_t op_pos = explained.find("op test.double.f");
        std::size_t select_pos = explained.find("select");
        REQUIRE(scan_pos != std::string::npos);
        REQUIRE(op_pos != std::string::npos);
        REQUIRE(select_pos != std::string::npos);
        // If pushdown had (incorrectly) hoisted the select across the node,
        // it would appear as a projection right after "scan", before "op".
        CHECK(scan_pos < op_pos);
        CHECK(op_pos < select_pos);

        DataFrame out = run(lf.collect());
        REQUIRE(out.num_rows() == 2);
        CHECK(rec.opened);

        CHECK(dftu_node_unregister("test.double.f") == 0);
    }

    TEST_CASE("a breaker node drains its input and emits one frame") {
        struct CountState {
            void* in_self;
            const dftu_cursor_vt* in_vt;
            bool done = false;
        };

        static const dftu_cursor_vt* count_vt_holder = nullptr;
        (void)count_vt_holder;

        struct Impl {
            static CoroTask<void> drain_and_count(CountState* st,
                                                  dftu_result_frame* out) {
                std::int64_t total = 0;
                for (;;) {
                    dftu_result_frame in_out{};
                    if (dftu_task* t =
                            st->in_vt->next(st->in_self, 1 << 20, &in_out)) {
                        auto* task = reinterpret_cast<CoroTask<void>*>(t);
                        co_await *task;
                        delete task;
                    }
                    if (!DFTU_RESULT_OK(in_out)) {
                        *out = in_out;
                        co_return;
                    }
                    dftu_dataframe* frame = DFTU_RESULT_VALUE(in_out);
                    if (!frame) break;
                    total += dftu_dataframe_num_rows(frame);
                    dftu_dataframe_free(frame);
                }
                dftu_series* c =
                    dftu_series_new_flat(DFTU_TYPE_INT64, &total, 1, nullptr);
                const char* names[1] = {"count"};
                dftu_series* cols[1] = {c};
                out->ok = 1;
                out->u.value = dftu_dataframe_new(names, cols, 1);
            }

            static dftu_task* next(void* self, std::int64_t,
                                   dftu_result_frame* out) {
                auto* st = static_cast<CountState*>(self);
                if (st->done) {
                    out->ok = 1;
                    out->u.value = nullptr;
                    return nullptr;
                }
                st->done = true;
                return dftracer::utils::task_to_abi(drain_and_count(st, out));
            }
            static void destroy(void* self) {
                auto* st = static_cast<CountState*>(self);
                if (st->in_vt->destroy) st->in_vt->destroy(st->in_self);
                delete st;
            }
        };

        static const dftu_cursor_vt COUNT_VT = {Impl::next, Impl::destroy,
                                                nullptr, nullptr, nullptr};

        struct NodeImpl {
            static void schema(void*, const dftu_schema*, const dftu_op_arg*,
                               dftu_schema* out) {
                dftu_schema_add_field(out, "count", DFTU_TYPE_INT64, 0,
                                      DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
            }
            static void* open(void*, void* in_self, const dftu_cursor_vt* in_vt,
                              const dftu_op_arg*, void** out_self,
                              const dftu_cursor_vt** out_vt) {
                auto* st = new CountState{in_self, in_vt};
                *out_self = st;
                *out_vt = &COUNT_VT;
                return st;
            }
        };

        dftu_node_vt vt{};
        vt.output_schema = NodeImpl::schema;
        vt.open = NodeImpl::open;
        REQUIRE(dftu_node_register("test.count_breaker", &vt, nullptr) == 0);

        IntSource fx{{"val"}, {1, 2, 3, 4, 5, 6, 7}};
        LazyFrame lf = LazyFrame::scan(make_int_source(fx, false))
                           .op("test.count_breaker", OpArgs());
        DataFrame out = run(lf.collect());
        REQUIRE(out.num_rows() == 1);
        CHECK(out.columns[0].data<std::int64_t>()[0] == 7);

        CHECK(dftu_node_unregister("test.count_breaker") == 0);
    }

    TEST_CASE(
        "a node over a resident source observes next() returning NULL "
        "(sync path)") {
        std::vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(dftu_node_register("test.sync_probe.resident", &vt,
                                   &sync_flags) == 0);

        DataFrame df;
        df.names = {"val"};
        std::vector<std::int64_t> v{1, 2, 3};
        df.columns.push_back(Series::flat_i64(v.data(), 3));

        LazyFrame lf = df.lazy().op("test.sync_probe.resident", OpArgs());
        DataFrame out = run(lf.collect());

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
        std::vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(dftu_node_register("test.sync_probe.async", &vt, &sync_flags) ==
                0);

        auto src = std::make_shared<CursorSource>(
            [] { return std::make_unique<ThrowingAsyncCursor>(); });
        LazyFrame lf =
            LazyFrame::scan(src).op("test.sync_probe.async", OpArgs());
        DataFrame out = run(lf.collect());

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
        std::vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(dftu_node_register("test.sync_probe.err", &vt, &sync_flags) ==
                0);

        {
            auto src = std::make_shared<CursorSource>(
                [] { return std::make_unique<ThrowingSyncCursor>(2); });
            LazyFrame lf =
                LazyFrame::scan(src).op("test.sync_probe.err", OpArgs());
            bool threw = false;
            try {
                run(lf.collect());
            } catch (const std::runtime_error& e) {
                threw = true;
                CHECK(std::string(e.what()).find("sync boom") !=
                      std::string::npos);
            }
            CHECK(threw);
        }
        {
            auto src = std::make_shared<CursorSource>(
                [] { return std::make_unique<ThrowingAsyncCursor>(2); });
            LazyFrame lf =
                LazyFrame::scan(src).op("test.sync_probe.err", OpArgs());
            bool threw = false;
            try {
                run(lf.collect());
            } catch (const std::runtime_error& e) {
                threw = true;
                CHECK(std::string(e.what()).find("async boom") !=
                      std::string::npos);
            }
            CHECK(threw);
        }

        CHECK(dftu_node_unregister("test.sync_probe.err") == 0);
    }

    TEST_CASE(
        "a resident multi-stage chain (filter/select/with_column) takes the "
        "sync path for every call") {
        std::vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(dftu_node_register("test.sync_probe.chain.resident", &vt,
                                   &sync_flags) == 0);

        auto src = std::make_shared<CursorSource>([] {
            return std::make_unique<MultiSyncCursor>(multi_morsel_data(), 3);
        });
        DataFrame out = run(
            build_probe_chain(src, "test.sync_probe.chain.resident").collect());
        check_multi_morsel_output(out);

        REQUIRE(!sync_flags.empty());
        for (bool synced : sync_flags) CHECK(synced);

        CHECK(dftu_node_unregister("test.sync_probe.chain.resident") == 0);
    }

    TEST_CASE(
        "a genuinely suspending multi-stage chain takes the async path "
        "throughout and produces identical values") {
        std::vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(dftu_node_register("test.sync_probe.chain.async", &vt,
                                   &sync_flags) == 0);

        auto src = std::make_shared<CursorSource>([] {
            return std::make_unique<MultiAsyncCursor>(multi_morsel_data(), 3);
        });
        DataFrame out = run(
            build_probe_chain(src, "test.sync_probe.chain.async").collect());
        check_multi_morsel_output(out);

        REQUIRE(!sync_flags.empty());
        for (bool synced : sync_flags) CHECK_FALSE(synced);

        CHECK(dftu_node_unregister("test.sync_probe.chain.async") == 0);
    }

    TEST_CASE(
        "a chain that starts resident and becomes async partway (the "
        "SpoolReader shape) produces correct values across the transition") {
        std::vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(dftu_node_register("test.sync_probe.chain.hybrid", &vt,
                                   &sync_flags) == 0);

        auto src = std::make_shared<CursorSource>([] {
            return std::make_unique<SyncThenAsyncCursor>(multi_morsel_data(), 3,
                                                         3);
        });
        DataFrame out = run(
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
        std::vector<bool> sync_flags;
        dftu_node_vt vt = make_probe_vt();
        REQUIRE(dftu_node_register("test.sync_probe.chain.err", &vt,
                                   &sync_flags) == 0);

        {
            auto src = std::make_shared<CursorSource>(
                [] { return std::make_unique<ThrowingSyncCursor>(2); });
            LazyFrame lf = build_probe_chain(src, "test.sync_probe.chain.err");
            bool threw = false;
            try {
                run(lf.collect());
            } catch (const std::runtime_error& e) {
                threw = true;
                CHECK(std::string(e.what()).find("sync boom") !=
                      std::string::npos);
            }
            CHECK(threw);
        }
        {
            auto src = std::make_shared<CursorSource>(
                [] { return std::make_unique<ThrowingAsyncCursor>(2); });
            LazyFrame lf = build_probe_chain(src, "test.sync_probe.chain.err");
            bool threw = false;
            try {
                run(lf.collect());
            } catch (const std::runtime_error& e) {
                threw = true;
                CHECK(std::string(e.what()).find("async boom") !=
                      std::string::npos);
            }
            CHECK(threw);
        }

        CHECK(dftu_node_unregister("test.sync_probe.chain.err") == 0);
    }

    TEST_CASE(
        "a join narrows the source cursor to its build keys through the "
        "streaming ops between them") {
        NarrowLog log;
        NarrowSource fx{{{"val"}, {1, 2, 3, 4, 5, 6}}, &log};
        LazyFrame right = right_keys({2, 4, 4}).lazy();

        SUBCASE("inner join: the set of build keys, positional to the source") {
            LazyFrame lf =
                LazyFrame::scan(make_narrow_source(fx))
                    .with_column("twice",
                                 col(0) * dftracer::utils::dataframe::lit(
                                              std::int64_t{2}))
                    .filter(col(0) > std::int64_t{1})
                    .with_row_index("row")
                    .join(right, {"val"}, {"key"}, JoinHow::Inner);
            DataFrame out = run(lf.collect());
            REQUIRE(out.num_rows() == 3);  // 2, 4, 4

            REQUIRE(log.masks.size() == 1);
            const std::vector<bool> want{false, true,  false,
                                         true,  false, false};
            CHECK(log.masks[0] == want);
        }

        SUBCASE("a left join keeps every left row, so nothing is offered") {
            LazyFrame lf = LazyFrame::scan(make_narrow_source(fx))
                               .join(right, {"val"}, {"key"}, JoinHow::Left);
            DataFrame out = run(lf.collect());
            REQUIRE(out.num_rows() == 7);
            CHECK(log.masks.empty());
        }

        SUBCASE("a predicate on the produced column stops at with_column") {
            // The join key is the derived column, which the source does not
            // carry; the offer must not be forwarded as if it were col 0.
            LazyFrame lf =
                LazyFrame::scan(make_narrow_source(fx))
                    .with_column("twice",
                                 col(0) * dftracer::utils::dataframe::lit(
                                              std::int64_t{2}))
                    .join(right, {"twice"}, {"key"}, JoinHow::Inner);
            DataFrame out = run(lf.collect());
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
        REQUIRE(dftu_node_register("test.forward", &vt, &flog) == 0);

        NarrowLog log;
        NarrowSource fx{{{"val"}, {1, 2, 3, 4, 5, 6}}, &log};
        LazyFrame right = right_keys({3, 5}).lazy();
        LazyFrame lf = LazyFrame::scan(make_narrow_source(fx))
                           .op("test.forward", OpArgs())
                           .join(right, {"val"}, {"key"}, JoinHow::Inner)
                           .memory_budget(64 * 1024 * 1024);
        DataFrame out = run(lf.collect());
        REQUIRE(out.num_rows() == 2);

        // The offer went through the node's own narrow slot to the source.
        REQUIRE(log.masks.size() == 1);
        const std::vector<bool> want{false, false, true, false, true, false};
        CHECK(log.masks[0] == want);

        // The node's declared size ran past the budget, so the driver asked
        // it for the excess and it shrank to fit.
        REQUIRE(!flog.reclaims.empty());
        CHECK(flog.reclaims[0] == (std::uint64_t{1} << 40) - 64 * 1024 * 1024);
        CHECK(flog.bytes == 64 * 1024 * 1024);

        CHECK(dftu_node_unregister("test.forward") == 0);
    }

    TEST_CASE(
        "a provider under a plugin node is still measured and reclaimed") {
        // The node owns its input cursor once open() succeeds, so the driver
        // cannot reach the provider through the node; the registry every
        // stage joined when the chain was built reaches it regardless.
        constexpr std::uint64_t BUDGET = 64 * 1024 * 1024;
        ForwardLog flog;  // the node itself holds nothing
        dftu_node_vt vt = make_forward_vt();
        REQUIRE(dftu_node_register("test.forward.under", &vt, &flog) == 0);

        NarrowLog log;
        log.bytes = std::uint64_t{1} << 40;
        NarrowSource fx{{{"val"}, {1, 2, 3, 4, 5, 6}}, &log};
        LazyFrame lf =
            LazyFrame::scan(make_narrow_source(fx))
                .op("test.forward.under", OpArgs())
                .op("test.forward.under", OpArgs())  // two nodes deep
                .with_column("twice", col(0) * dftracer::utils::dataframe::lit(
                                                   std::int64_t{2}))
                .memory_budget(BUDGET);
        DataFrame out = run(lf.collect());
        REQUIRE(out.num_rows() == 6);

        CHECK(flog.reclaims.empty());
        REQUIRE(!log.reclaims.empty());
        CHECK(log.reclaims[0] == (std::uint64_t{1} << 40) - BUDGET);
        CHECK(log.bytes == BUDGET);

        CHECK(dftu_node_unregister("test.forward.under") == 0);
    }
}
