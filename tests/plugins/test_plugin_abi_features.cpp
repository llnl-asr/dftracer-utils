// SPDX-License-Identifier: MIT
// In-process coverage for the plugin ABI surface the host-service suite leaves
// untested: the config tree, query compile/match and plan_duql event routing,
// and the coroutine control ops (drive path).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/config.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <doctest/doctest.h>
#include <plugins/test_abi_ctx.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

using namespace dftracer::utils;

namespace {

struct ConfigSlice {
    std::int64_t threshold = 0;
    double ratio = 0.0;
    dstd::String label;
    bool enabled = false;
    std::int64_t inner = 0;
    std::int64_t missing = 0;
    dstd::Vector<std::int64_t> buckets;

    explicit ConfigSlice(const dftracer::utils::plugins::Config& c) {
        threshold = c.get_int("threshold");
        ratio = c.get_double("ratio");
        label = dstd::String(c.get("label"));
        enabled = c.get_bool("enabled");
        inner = c.child("nested").get_int("inner");
        missing = c.get_int("missing", -1);
        const dftracer::utils::plugins::ConfigValue arr = c.value("buckets");
        if (arr.is_array())
            for (std::uint32_t i = 0; i < arr.size(); ++i)
                buckets.push_back(arr[i].as_i64());
    }
    void step(dataframe::FrameView, dftracer::utils::plugins::Host) {}
    void merge(ConfigSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

struct CountState {
    std::uint64_t seen = 0;
    dstd::Vector<dstd::String> fhashes;
    dstd::Vector<dstd::String> hhashes;
};
CountState g_count;

struct CountSlice {
    explicit CountSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftracer::utils::plugins::Batch& b,
              dftracer::utils::plugins::Host) {
        g_count.seen += b.size();
        for (const dftracer::utils::plugins::Event& e : b) {
            g_count.fhashes.emplace_back(e.fhash());
            g_count.hhashes.emplace_back(e.hhash());
        }
    }
    void merge(CountSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

trace::views::detail::FoldEvent make_event(dstd::StringIntern& intern,
                                           const char* fhash,
                                           const char* hhash) {
    trace::views::detail::FoldEvent fe;
    fe.cat_id = intern.get_or_insert("POSIX");
    fe.name_id = intern.get_or_insert("read");
    fe.fhash_id = intern.get_or_insert(fhash);
    fe.hhash_id = intern.get_or_insert(hhash);
    fe.phase = trace::RecordPhase::Complete;
    fe.ts = 1;
    fe.dur = 5;
    fe.has_dur = true;
    return fe;
}

dftu_status inc_fn(dftu_ctx*, void* p) {
    static_cast<std::atomic<int>*>(p)->fetch_add(1);
    return DFTU_OK;
}
dftu_status append1_fn(dftu_ctx*, void* p) {
    static_cast<dstd::Vector<int>*>(p)->push_back(1);
    return DFTU_OK;
}
dftu_status append2_fn(dftu_ctx*, void* p) {
    static_cast<dstd::Vector<int>*>(p)->push_back(2);
    return DFTU_OK;
}

plugins::Task fanout_all(plugins::Host h, std::atomic<int>* ran, int n) {
    dstd::Vector<dstd::Result<dftu_task*>> kids;
    for (int i = 0; i < n; ++i)
        kids.push_back(plugins::detail::spawn_raw(h, inc_fn, ran).release());
    plugins::AsyncAll all(std::move(kids), plugins::detail::host_raw(h));
    co_await all;
}

plugins::Task drop_then_fail(plugins::Host h, std::atomic<int>* ran) {
    plugins::AsyncOp dropped = plugins::detail::spawn_raw(h, inc_fn, ran);
    if (ran != nullptr) {
        dstd::Error gave_up = plugins::io_error("gave up");
        co_await plugins::fail(std::move(gave_up));
    }
    co_await std::move(dropped);
}

dftu_status spawn_fails_fn(dftu_ctx* ctx, void*) {
    return dftu_fail(ctx, DFTU_E_IO, "spawned work failed");
}

// Counts the frames it lives in that were destroyed, to show a frame goes once.
struct FrameCounter {
    static inline int destroyed = 0;
    FrameCounter() = default;
    FrameCounter(const FrameCounter&) = delete;
    FrameCounter& operator=(const FrameCounter&) = delete;
    ~FrameCounter() { ++destroyed; }
};

// A failed awaited task ends the task: the line after the await never runs, and
// a child started earlier and still owned by a local is given back unrun.
plugins::Task await_failed_op(plugins::Host h, std::atomic<int>* child,
                              bool* reached) {
    FrameCounter counter;
    plugins::AsyncOp started = plugins::detail::spawn_raw(h, inc_fn, child);
    co_await plugins::detail::spawn_raw(h, spawn_fails_fn, nullptr);
    *reached = true;
    co_await std::move(started);
}

// `co_await` on an error Result ends the task with it.
plugins::Task await_error_result(plugins::Host h, bool* reached) {
    FrameCounter counter;
    auto missing = h.run_op("dftu.test.nonexistent_op", {});
    co_await std::move(missing);
    *reached = true;
}

// `.expected()` hands the failure back: the task goes on and ends in success.
plugins::Task await_expected(plugins::Host h, dstd::Optional<dftu_status>* seen,
                             bool* reached) {
    plugins::AsyncOp failing =
        plugins::detail::spawn_raw(h, spawn_fails_fn, nullptr);
    auto outcome = co_await failing.expected();
    if (!outcome.has_value()) *seen = plugins::status_of(outcome.error());
    *reached = true;
}

plugins::Task return_error(plugins::Host, bool* reached) {
    FrameCounter counter;
    *reached = true;
    dstd::Error gave_up = plugins::parse_error("gave up with a parse error");
    co_await plugins::fail(std::move(gave_up));
}

// The task service of a host whose spawn refuses the call numbered
// `refuse_call` (1-based), and which counts discards.
struct RefusingHost {
    static inline dftu_plugin_host* real = nullptr;
    static inline const dftu_svc_task* real_svc = nullptr;
    static inline dftu_svc_task svc{};
    static inline int spawn_calls = 0;
    static inline int refuse_call = 0;
    static inline int discards = 0;
    dftu_plugin_host wrapped{};

    explicit RefusingHost(dftu_plugin_host& host, int refuse) {
        real = &host;
        real_svc = static_cast<const dftu_svc_task*>(
            host.get_service(host.h, DFTU_SVC_TASK));
        svc = *real_svc;
        spawn_calls = 0;
        refuse_call = refuse;
        discards = 0;
        svc.spawn = [](dftu_ctx* ctx, void* h,
                       dftu_status (*fn)(dftu_ctx*, void*), void* arg,
                       dftu_job_where where, dftu_task** out) -> dftu_status {
            if (++spawn_calls == refuse_call)
                return dftu_fail(ctx, DFTU_E_TIMEOUT, "host refused the start");
            return real_svc->spawn(ctx, h, fn, arg, where, out);
        };
        svc.discard = [](void* h, dftu_task* t) {
            ++discards;
            real_svc->discard(h, t);
        };
        wrapped = host;
        wrapped.get_service = [](void* h, const char* id) -> const void* {
            return std::strcmp(id, DFTU_SVC_TASK) == 0
                       ? &svc
                       : real->get_service(h, id);
        };
    }
};

plugins::Task await_refused(plugins::Host h, std::atomic<int>* ran,
                            bool* reached) {
    FrameCounter counter;
    co_await plugins::detail::spawn_raw(h, inc_fn, ran);
    *reached = true;
}

plugins::Task all_refused(plugins::Host h, std::atomic<int>* ran,
                          bool* reached) {
    FrameCounter counter;
    plugins::AsyncOp first = plugins::detail::spawn_raw(h, inc_fn, ran);
    plugins::AsyncOp second = plugins::detail::spawn_raw(h, inc_fn, ran);
    plugins::AsyncAll both =
        plugins::detail::all_raw(h, {first.release(), second.release()});
    co_await std::move(both);
    *reached = true;
}

plugins::Task await_released_refused(plugins::Host h, std::atomic<int>* ran,
                                     bool* reached) {
    plugins::AsyncOp op = plugins::detail::spawn_raw(h, inc_fn, ran);
    co_await plugins::detail::await_result(h, op.release());
    *reached = true;
}

plugins::Task all_one_fails(plugins::Host h, std::atomic<int>* ran) {
    plugins::AsyncOp bad =
        plugins::detail::spawn_raw(h, spawn_fails_fn, nullptr);
    plugins::AsyncOp good = plugins::detail::spawn_raw(h, inc_fn, ran);
    plugins::AsyncAll both =
        plugins::detail::all_raw(h, {bad.release(), good.release()});
    co_await std::move(both);
}

plugins::Task fail_on_entry() {
    FrameCounter counter;
    dstd::Error gave_up = plugins::parse_error("failed on entry");
    co_await plugins::fail(std::move(gave_up));
}

plugins::Task return_ok() {
    FrameCounter counter;
    co_return;
}

plugins::Task chain(plugins::Host h, dstd::Vector<int>* order) {
    co_await plugins::detail::spawn_raw(h, append1_fn, order);
    co_await plugins::detail::spawn_raw(h, append2_fn, order);
}

using I64 = std::int64_t;

// (in*2) then (+10): a value threaded through the pipe.
plugins::Task typed_pipe(plugins::Host h, I64* result) {
    auto a =
        co_await plugins::make_job<I64, I64>(h, [](I64 x) { return x * 2; });
    auto b =
        co_await plugins::make_job<I64, I64>(h, [](I64 x) { return x + 10; });
    auto piped = co_await (a | b);
    I64 in = 5, out = 0;
    co_await plugins::run(piped, in, out);
    *result = out;
}

// when_all: both leaves see the same input; the output is their values
// concatenated.
plugins::Task jobs_all(plugins::Host h, std::array<I64, 2>* out) {
    auto a =
        co_await plugins::make_job<I64, I64>(h, [](I64 x) { return x * 2; });
    auto b =
        co_await plugins::make_job<I64, I64>(h, [](I64 x) { return x + 10; });
    auto both = co_await plugins::when_all(a, b);
    I64 in = 5;
    co_await plugins::run(both, in, *out);
}

// when_any: both racers compute the same value, so the winner is
// deterministic.
plugins::Task jobs_any(plugins::Host h, I64* result) {
    auto a =
        co_await plugins::make_job<I64, I64>(h, [](I64 x) { return x + 10; });
    auto b =
        co_await plugins::make_job<I64, I64>(h, [](I64 x) { return x + 10; });
    auto any = co_await plugins::when_any(a, b);
    I64 in = 5;
    co_await plugins::run(any, in, *result);
}

// map: one body run per item, outputs in item order.
plugins::Task jobs_map(plugins::Host h, dstd::Vector<I64>* out) {
    auto body =
        co_await plugins::make_job<I64, I64>(h, [](I64 x) { return x * x; });
    dstd::Vector<I64> items = {1, 2, 3, 4};
    out->assign(items.size(), 0);
    dftu_job_items in{items.data(), items.size()};
    dftu_job_slots slots{out->data(), out->size()};
    const dftu_svc_task* svc = co_await plugins::detail::task_service(h);
    dftu_job* m = nullptr;
    co_await plugins::detail::reported(
        svc->map(plugins::detail::host_ctx(h), plugins::detail::host_raw(h)->h,
                 plugins::detail::job_raw(body), 2, &m));
    dftu_task* t = nullptr;
    co_await plugins::detail::reported(
        svc->start(plugins::detail::host_ctx(h),
                   plugins::detail::host_raw(h)->h, m, &in, &slots, &t));
    co_await plugins::detail::await_raw(h, t);
}

// A column rides the pipe as a first-class DFTU_T_SERIES value: the source
// [1,2,3,4] is doubled twice via a real SIMD add, so the output is [4,8,12,16].
// Only the `dftu_series*` handle crosses each job (zero-copy); `a` borrows its
// input (the caller keeps `src`), `b` consumes the intermediate.
plugins::Task series_pipe(plugins::Host h, dftu_series** result) {
    I64 vals[4] = {1, 2, 3, 4};
    dftu_series* src = dftu_series_new_flat(DFTU_TYPE_INT64, vals, 4, nullptr);
    auto a = co_await plugins::make_job<dftu_series*, dftu_series*>(
        h, [](dftu_series* s) { return dftu_series_add(s, s); });
    auto b = co_await plugins::make_job<dftu_series*, dftu_series*>(
        h, [](dftu_series* s) {
            dftu_series* r = dftu_series_add(s, s);
            dftu_series_free(s);
            return r;
        });
    auto piped = co_await (a | b);
    dftu_series* out = nullptr;
    co_await plugins::run(piped, src, out);
    dftu_series_free(src);
    *result = out;
}

// Typed pipes are checked at compile time: a matching pipe is well-formed, a
// mismatched one is not.
template <class A, class B>
concept Pipeable = requires(A a, B b) { a | b; };
using JobII = plugins::Job<I64, I64>;
using JobDD = plugins::Job<double, double>;
static_assert(Pipeable<JobII, JobII>, "i64->i64 | i64->i64 must pipe");
static_assert(!Pipeable<JobII, JobDD>, "i64->i64 | f64->f64 must not pipe");

dftu_status job_noop(dftu_ctx*, void*, const void*, void*, dftu_task**) {
    return DFTU_OK;
}

dftu_status make_raw_job(plugins::Host h, dftu_type in_ty, dftu_type out_ty,
                         dftu_job** out) {
    dftu_job_desc d{};
    d.struct_size = sizeof(d);
    d.fn = job_noop;
    d.in_ty = in_ty;
    d.in_size = 8;
    d.out_ty = out_ty;
    d.out_size = 8;
    auto svc = plugins::detail::task_service(h);
    if (!svc.has_value()) return plugins::status_of(svc.error());
    return (*svc)->job(plugins::detail::host_ctx(h),
                       plugins::detail::host_raw(h)->h, &d, out);
}

template <class MakeTask>
dftu_status drive_task(dftu_plugin_host& host, MakeTask make,
                       dstd::String* message = nullptr) {
    test_plugin_common::TaskDriver driver(host);
    return driver.run(make(driver.host()), message);
}

// Owns a PluginFold plus its backing plugin; the fold references the plugin, so
// it is destroyed first.
template <class Slice>
struct FoldFixture {
    dstd::StringIntern intern;
    dftu_plugin* plugin;
    dstd::UniquePtr<plugins::host::PluginFold> fold;
    explicit FoldFixture(const dftu_value* config)
        : plugin(dftracer::utils::plugins::make_plugin<Slice>(config)),
          fold(dstd::make_unique<plugins::host::PluginFold>(plugin, intern)) {}
    dftu_plugin_host& host() { return fold->host(); }
    ~FoldFixture() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
};

// A raw columnar plugin: on_batch gets the batch as a dftu_dataframe and
// SIMD-reduces its `dur` column. Mirrors the slice total into a global so the
// test can read it.
struct ColState {
    std::int64_t dur_sum = 0;
};
ColState g_col_state;

dftu_status col_make_slice(dftu_ctx*, void*, void** out) {
    *out = new ColState();
    return DFTU_OK;
}
void col_destroy_slice(void* s) { delete static_cast<ColState*>(s); }
void col_destroy(void*) {}
dftu_status col_merge(dftu_ctx*, void* into, void* other) {
    static_cast<ColState*>(into)->dur_sum +=
        static_cast<ColState*>(other)->dur_sum;
    return DFTU_OK;
}
const char* col_plan_duql(void*) { return nullptr; }
dftu_status col_on_finalize(dftu_ctx*, void*, const dftu_plugin_host*,
                            dftu_task**) {
    return DFTU_OK;
}
dftu_status col_on_batch(dftu_ctx*, void* slice, const dftu_dataframe* df,
                         const dftu_plugin_host*) {
    dftu_series* col = dftu_dataframe_column(df, "dur");
    if (col) {
        dftu_scalar s = dftu_series_reduce(col, DFTU_REDUCE_SUM);
        auto* st = static_cast<ColState*>(slice);
        st->dur_sum += s.kind == DFTU_SCALAR_TAG_U64
                           ? static_cast<std::int64_t>(s.value.u)
                           : s.value.i;
        g_col_state.dur_sum = st->dur_sum;
        dftu_series_free(col);
    }
    return DFTU_OK;
}

// Owning wrapper for a hand-built [cat, name] test dataframe (freed on scope
// exit), for dftu_svc_duql::duql_matches tests outside an on_batch call.
struct QueryFrame {
    dftu_dataframe* df = nullptr;
    ~QueryFrame() {
        if (df) dftu_dataframe_free(df);
    }
    QueryFrame(const QueryFrame&) = delete;
    QueryFrame& operator=(const QueryFrame&) = delete;
    QueryFrame() = default;
    QueryFrame(QueryFrame&& o) noexcept : df(o.df) { o.df = nullptr; }
};

dftu_series* string_column(const dstd::Vector<dstd::String>& vals) {
    dstd::Vector<std::int32_t> offsets(vals.size() + 1, 0);
    dstd::String data;
    for (std::size_t i = 0; i < vals.size(); ++i) {
        data += vals[i];
        offsets[i + 1] = static_cast<std::int32_t>(data.size());
    }
    return dftu_series_new_string(DFTU_TYPE_STRING, offsets.data(), data.data(),
                                  static_cast<std::int64_t>(vals.size()),
                                  nullptr);
}

QueryFrame cat_name_frame(const dstd::Vector<dstd::String>& cats,
                          const dstd::Vector<dstd::String>& names) {
    const char* col_names[2] = {"cat", "name"};
    dftu_series* cols[2] = {string_column(cats), string_column(names)};
    QueryFrame f;
    f.df = dftu_dataframe_new(col_names, cols, 2);
    return f;
}

dftu_duql* compile_query(const dftu_svc_duql* qx, void* h,
                         const dstd::String& src, dftu_status* status = nullptr,
                         dstd::String* message = nullptr) {
    test_plugin_common::Ctx c;
    dftu_duql* q = nullptr;
    const dftu_status st = qx->duql_compile(
        c.get(), h, src.data(), static_cast<std::uint32_t>(src.size()), &q);
    if (status) *status = st;
    if (message) *message = c.message();
    return st < 0 ? nullptr : q;
}

}  // namespace

TEST_CASE("plugin ABI: on_batch hands the batch as columns") {
    g_col_state = {};
    dftu_plugin p{};
    p.struct_size = sizeof(p);
    p.plan_duql = col_plan_duql;
    p.make_slice = col_make_slice;
    p.merge = col_merge;
    p.on_finalize = col_on_finalize;
    p.destroy_slice = col_destroy_slice;
    p.destroy = col_destroy;
    p.on_batch = col_on_batch;

    dstd::StringIntern intern;
    plugins::host::PluginFold fold(&p, intern);
    dstd::Vector<trace::views::detail::FoldEvent> evs = {
        make_event(intern, "f", "h"), make_event(intern, "f", "h"),
        make_event(intern, "f", "h")};
    trace::views::detail::ScanUnit unit{};
    dftu_utils_test::ok(fold.step(trace::views::detail::FoldBatch{
        dstd::Span<const trace::views::detail::FoldEvent>(evs), unit, {}}));
    CHECK(g_col_state.dur_sum == 15);  // 3 events x dur 5, SIMD-reduced
}

TEST_CASE("plugin ABI: config tree reads scalars, nested, array, and default") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String path = env.dir() + "/config.json";
    {
        dstd::fs::write_file(
            dstd::PathView(path),
            common::str_cat(
                R"({"threshold":42,"ratio":2.5,"label":"posix","enabled":true,)",
                R"("nested":{"inner":7},"buckets":[10,20,30]})"))
            .discard();
    }

    auto loaded = plugins::host::ConfigTree::from_json_file(path);
    REQUIRE(loaded.has_value());
    plugins::host::ConfigTree tree = std::move(*loaded);
    dftu_plugin* p =
        dftracer::utils::plugins::make_plugin<ConfigSlice>(tree.root());
    test_plugin_common::Ctx slice_ctx;
    void* made = nullptr;
    REQUIRE(p->make_slice(slice_ctx.get(), p->self, &made) == DFTU_OK);
    auto* slice = static_cast<ConfigSlice*>(made);
    REQUIRE(slice != nullptr);

    CHECK(slice->threshold == 42);
    CHECK(slice->ratio == doctest::Approx(2.5));
    CHECK(slice->label == "posix");
    CHECK(slice->enabled == true);
    CHECK(slice->inner == 7);
    CHECK(slice->missing == -1);
    REQUIRE(slice->buckets.size() == 3);
    CHECK(slice->buckets[0] == 10);
    CHECK(slice->buckets[2] == 30);

    p->destroy_slice(slice);
    p->destroy(p->self);
}

TEST_CASE("plugin ABI: duql_compile then duql_matches on known events") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    const auto* qx = static_cast<const dftu_svc_duql*>(
        host.get_service(host.h, DFTU_SVC_DUQL));
    REQUIRE(qx != nullptr);

    dstd::String src = "cat == \"POSIX\"";
    dftu_duql* q = compile_query(qx, host.h, src);
    REQUIRE(q != nullptr);

    // Row 0 is the hit (cat=POSIX), row 1 the miss (cat=STDIO); both name
    // "read".
    QueryFrame frame = cat_name_frame({"POSIX", "STDIO"}, {"read", "read"});
    CHECK(qx->duql_matches(host.h, q, frame.df, 0) == 1);
    CHECK(qx->duql_matches(host.h, q, frame.df, 1) == 0);

    SUBCASE("malformed query source does not compile and match is safe") {
        dstd::String bad = "cat ==";
        dftu_status st = DFTU_OK;
        dstd::String message;
        dftu_duql* bq = compile_query(qx, host.h, bad, &st, &message);
        CHECK(bq == nullptr);
        CHECK(st == DFTU_E_PARSE);
        CHECK_FALSE(message.empty());
        // A null query must yield a defined 0, never a throw across the ABI.
        CHECK(qx->duql_matches(host.h, nullptr, frame.df, 0) == 0);
    }
}

TEST_CASE("plugin ABI: duql_matches sees each new frame, not cached columns") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();
    const auto* qx = static_cast<const dftu_svc_duql*>(
        host.get_service(host.h, DFTU_SVC_DUQL));
    REQUIRE(qx != nullptr);

    dstd::String cat_src = "cat == \"POSIX\"";
    dstd::String name_src = "name == \"write\"";
    dftu_duql* by_cat = compile_query(qx, host.h, cat_src);
    dftu_duql* by_name = compile_query(qx, host.h, name_src);
    REQUIRE(by_cat != nullptr);
    REQUIRE(by_name != nullptr);

    QueryFrame first = cat_name_frame({"POSIX", "STDIO"}, {"read", "write"});
    for (std::int64_t row = 0; row < 2; ++row) {
        CHECK(qx->duql_matches(host.h, by_cat, first.df, row) == (row == 0));
        CHECK(qx->duql_matches(host.h, by_name, first.df, row) == (row == 1));
    }

    // The same shape with the values swapped, made right after the free so
    // the allocator is likely to hand back the same address.
    const dftu_dataframe* old_address = first.df;
    dftu_dataframe_free(first.df);
    first.df = nullptr;
    QueryFrame second = cat_name_frame({"STDIO", "POSIX"}, {"write", "read"});
    MESSAGE("frame address reused: ", second.df == old_address);
    for (std::int64_t row = 0; row < 2; ++row) {
        CHECK(qx->duql_matches(host.h, by_cat, second.df, row) == (row == 1));
        CHECK(qx->duql_matches(host.h, by_name, second.df, row) == (row == 0));
    }
}

TEST_CASE("plugin ABI: duql_matches reads a non-flat number column") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();
    const auto* qx = static_cast<const dftu_svc_duql*>(
        host.get_service(host.h, DFTU_SVC_DUQL));
    REQUIRE(qx != nullptr);
    dstd::String src = "dur > 600";
    dftu_duql* q = compile_query(qx, host.h, src);
    REQUIRE(q != nullptr);

    const std::int64_t durs[] = {1, 500, 700, 3};
    dftu_series* base = dftu_series_new_flat(DFTU_TYPE_INT64, durs, 4, nullptr);
    dftu_series* dur =
        dftu_series_filter_gt(base, dataframe::to_scalar(std::int64_t{2}));
    dftu_series_free(base);
    REQUIRE(dur != nullptr);
    REQUIRE(dftu_series_encoding(dur) !=
            static_cast<std::int32_t>(dataframe::Encoding::Flat));

    const char* col_names[2] = {"cat", "dur"};
    dftu_series* cols[2] = {string_column({"POSIX", "POSIX", "POSIX"}), dur};
    QueryFrame frame;
    frame.df = dftu_dataframe_new(col_names, cols, 2);
    CHECK(qx->duql_matches(host.h, q, frame.df, 0) == 0);
    CHECK(qx->duql_matches(host.h, q, frame.df, 1) == 1);
    CHECK(qx->duql_matches(host.h, q, frame.df, 2) == 0);

    SUBCASE("a row outside the frame matches nothing") {
        CHECK(qx->duql_matches(host.h, q, frame.df, 3) == 0);
        CHECK(qx->duql_matches(host.h, q, frame.df, -1) == 0);
    }
}

TEST_CASE("plugin ABI: plan_duql on fhash delivers only matching events") {
    SUBCASE("fhash predicate keeps every match, drops the rest") {
        g_count = CountState{};
        plugins::host::ConfigTree tree;
        tree.set("duql", "fhash == \"fhAAA\"");
        FoldFixture<CountSlice> fx(tree.root());

        dstd::Vector<trace::views::detail::FoldEvent> evs;
        evs.push_back(make_event(fx.intern, "fhAAA", "hh1"));
        evs.push_back(make_event(fx.intern, "fhBBB", "hh1"));
        evs.push_back(make_event(fx.intern, "fhAAA", "hh2"));
        evs.push_back(make_event(fx.intern, "fhCCC", "hh2"));
        evs.push_back(make_event(fx.intern, "fhAAA", "hh3"));
        trace::views::detail::ScanUnit unit{};
        trace::views::detail::FoldBatch batch{
            dstd::Span<const trace::views::detail::FoldEvent>(evs), unit};

        dftu_utils_test::ok(fx.fold->step(batch));
        CHECK(fx.fold->take_pending() == nullptr);

        // The regression: a plan_duql on fhash must not silently drop matches.
        CHECK(g_count.seen == 3);
        for (const auto& f : g_count.fhashes) CHECK(f == "fhAAA");
    }

    SUBCASE("hhash predicate routes on a non cat/name field too") {
        g_count = CountState{};
        plugins::host::ConfigTree tree;
        tree.set("duql", "hhash == \"hhZ\"");
        FoldFixture<CountSlice> fx(tree.root());

        dstd::Vector<trace::views::detail::FoldEvent> evs;
        evs.push_back(make_event(fx.intern, "fh1", "hhZ"));
        evs.push_back(make_event(fx.intern, "fh2", "hhY"));
        evs.push_back(make_event(fx.intern, "fh3", "hhZ"));
        trace::views::detail::ScanUnit unit{};
        trace::views::detail::FoldBatch batch{
            dstd::Span<const trace::views::detail::FoldEvent>(evs), unit};

        dftu_utils_test::ok(fx.fold->step(batch));
        CHECK(g_count.seen == 2);
        for (const auto& h : g_count.hhashes) CHECK(h == "hhZ");
    }
}

TEST_CASE("plugin ABI: spawn fan-out joined by all runs every child") {
    FoldFixture<CountSlice> fx(nullptr);
    std::atomic<int> ran{0};
    CHECK(drive_task(fx.host(), [&](plugins::Host h) {
              return fanout_all(h, &ran, 4);
          }) == DFTU_OK);
    CHECK(ran.load() == 4);
}

TEST_CASE("plugin ABI: a started task dropped on a returned error never runs") {
    FoldFixture<CountSlice> fx(nullptr);
    std::atomic<int> ran{0};
    CHECK(drive_task(fx.host(), [&](plugins::Host h) {
              return drop_then_fail(h, &ran);
          }) == DFTU_E_IO);
    CHECK(ran.load() == 0);
}

TEST_CASE("plugin ABI: a failed awaited task ends the task with its error") {
    FoldFixture<CountSlice> fx(nullptr);
    std::atomic<int> child{0};
    bool reached = false;
    FrameCounter::destroyed = 0;
    dstd::String message;
    CHECK(drive_task(
              fx.host(),
              [&](plugins::Host h) {
                  return await_failed_op(h, &child, &reached);
              },
              &message) == DFTU_E_IO);
    CHECK(message.find("spawned work failed") != dstd::String::npos);
    CHECK_FALSE(reached);
    // The child started before the failing await is given back, never run.
    CHECK(child.load() == 0);
    CHECK(FrameCounter::destroyed == 1);
}

TEST_CASE("plugin ABI: a started child is discarded when the task ends early") {
    FoldFixture<CountSlice> fx(nullptr);
    static dftu_plugin_host* real = nullptr;
    static const dftu_svc_task* real_svc = nullptr;
    static dftu_svc_task counting{};
    static int discards = 0;
    real = &fx.host();
    real_svc = static_cast<const dftu_svc_task*>(
        real->get_service(real->h, DFTU_SVC_TASK));
    REQUIRE(real_svc != nullptr);
    counting = *real_svc;
    discards = 0;
    counting.discard = [](void* h, dftu_task* t) {
        ++discards;
        real_svc->discard(h, t);
    };
    dftu_plugin_host wrapped = *real;
    wrapped.get_service = [](void* h, const char* id) -> const void* {
        return std::strcmp(id, DFTU_SVC_TASK) == 0 ? &counting
                                                   : real->get_service(h, id);
    };
    std::atomic<int> child{0};
    bool reached = false;
    CHECK(drive_task(wrapped, [&](plugins::Host h) {
              return await_failed_op(h, &child, &reached);
          }) == DFTU_E_IO);
    CHECK(child.load() == 0);
    CHECK(discards == 1);
}

TEST_CASE("plugin ABI: co_await on an error Result ends the task with it") {
    FoldFixture<CountSlice> fx(nullptr);
    bool reached = false;
    FrameCounter::destroyed = 0;
    dstd::String message;
    CHECK(drive_task(
              fx.host(),
              [&](plugins::Host h) { return await_error_result(h, &reached); },
              &message) == DFTU_E_NOT_FOUND);
    CHECK(message.find("dftu.test.nonexistent_op") != dstd::String::npos);
    CHECK_FALSE(reached);
    CHECK(FrameCounter::destroyed == 1);
}

TEST_CASE("plugin ABI: expected() hands a failure back and the task goes on") {
    FoldFixture<CountSlice> fx(nullptr);
    dstd::Optional<dftu_status> seen;
    bool reached = false;
    CHECK(drive_task(fx.host(), [&](plugins::Host h) {
              return await_expected(h, &seen, &reached);
          }) == DFTU_OK);
    CHECK(reached);
    REQUIRE(seen.has_value());
    CHECK(*seen == DFTU_E_IO);
}

TEST_CASE("plugin ABI: co_await fail(error) fails the task with its status") {
    FoldFixture<CountSlice> fx(nullptr);
    bool reached = false;
    FrameCounter::destroyed = 0;
    dstd::String message;
    CHECK(drive_task(
              fx.host(),
              [&](plugins::Host h) { return return_error(h, &reached); },
              &message) == DFTU_E_PARSE);
    CHECK(message == "gave up with a parse error");
    CHECK(reached);
    CHECK(FrameCounter::destroyed == 1);
}

TEST_CASE("plugin ABI: an op the host refuses fails the task with its error") {
    FoldFixture<CountSlice> fx(nullptr);
    RefusingHost refusing(fx.host(), 1);
    std::atomic<int> ran{0};
    bool reached = false;
    dstd::String message;
    CHECK(drive_task(
              refusing.wrapped,
              [&](plugins::Host h) { return await_refused(h, &ran, &reached); },
              &message) == DFTU_E_TIMEOUT);
    CHECK(message.find("host refused the start") != dstd::String::npos);
    CHECK_FALSE(reached);
    CHECK(ran.load() == 0);
}

TEST_CASE("plugin ABI: a refused op in all() keeps its status and message") {
    FoldFixture<CountSlice> fx(nullptr);
    RefusingHost refusing(fx.host(), 2);
    std::atomic<int> ran{0};
    bool reached = false;
    FrameCounter::destroyed = 0;
    dstd::String message;
    CHECK(drive_task(
              refusing.wrapped,
              [&](plugins::Host h) { return all_refused(h, &ran, &reached); },
              &message) == DFTU_E_TIMEOUT);
    CHECK(message.find("host refused the start") != dstd::String::npos);
    CHECK_FALSE(reached);
    // The child that did start is given back once and never runs.
    CHECK(RefusingHost::discards == 1);
    CHECK(ran.load() == 0);
    CHECK(FrameCounter::destroyed == 1);
}

TEST_CASE("plugin ABI: a refused op through Host::await keeps its error") {
    FoldFixture<CountSlice> fx(nullptr);
    RefusingHost refusing(fx.host(), 1);
    std::atomic<int> ran{0};
    bool reached = false;
    dstd::String message;
    CHECK(drive_task(
              refusing.wrapped,
              [&](plugins::Host h) {
                  return await_released_refused(h, &ran, &reached);
              },
              &message) == DFTU_E_TIMEOUT);
    CHECK(message.find("host refused the start") != dstd::String::npos);
    CHECK_FALSE(reached);
}

TEST_CASE("plugin ABI: all() with one failing child fails and awaits both") {
    FoldFixture<CountSlice> fx(nullptr);
    RefusingHost refusing(fx.host(), 0);
    std::atomic<int> ran{0};
    dstd::String message;
    CHECK(drive_task(
              refusing.wrapped,
              [&](plugins::Host h) { return all_one_fails(h, &ran); },
              &message) == DFTU_E_IO);
    CHECK(message.find("spawned work failed") != dstd::String::npos);
    CHECK(ran.load() == 1);
    CHECK(RefusingHost::discards == 0);
}

TEST_CASE(
    "plugin ABI: a failure on entry reports once and destroys the frame") {
    FoldFixture<CountSlice> fx(nullptr);
    FrameCounter::destroyed = 0;
    dstd::String message;
    CHECK(drive_task(
              fx.host(), [&](plugins::Host) { return fail_on_entry(); },
              &message) == DFTU_E_PARSE);
    CHECK(message == "failed on entry");
    CHECK(FrameCounter::destroyed == 1);
}

TEST_CASE("plugin ABI: a task that ends in success destroys its frame once") {
    FoldFixture<CountSlice> fx(nullptr);
    FrameCounter::destroyed = 0;
    CHECK(drive_task(fx.host(), [&](plugins::Host) { return return_ok(); }) ==
          DFTU_OK);
    CHECK(FrameCounter::destroyed == 1);
}

TEST_CASE("plugin SDK: an AsyncOp dropped unawaited discards its task") {
    static int discarded = 0;
    static dftu_task* seen = nullptr;
    dftu_svc_task svc{};
    svc.struct_size = sizeof(svc);
    svc.discard = [](void*, dftu_task* t) {
        ++discarded;
        seen = t;
    };
    dftu_plugin_host host{};
    host.struct_size = sizeof(host);
    host.h = &svc;
    host.get_service = [](void* h, const char* id) -> const void* {
        return std::strcmp(id, DFTU_SVC_TASK) == 0 ? h : nullptr;
    };
    auto* fake = reinterpret_cast<dftu_task*>(&host);
    {
        plugins::AsyncOp dropped(fake, &host);
    }
    CHECK(discarded == 1);
    CHECK(seen == fake);
    {
        plugins::AsyncOp handed(fake, &host);
        CHECK(*handed.release() == fake);
    }
    CHECK(discarded == 1);
}

TEST_CASE("plugin ABI: awaited spawns run in order") {
    FoldFixture<CountSlice> fx(nullptr);
    dstd::Vector<int> order;
    CHECK(drive_task(fx.host(), [&](plugins::Host h) {
              return chain(h, &order);
          }) == DFTU_OK);
    REQUIRE(order.size() == 2);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
}

TEST_CASE("plugin ABI: job then threads a value through the pipe") {
    FoldFixture<CountSlice> fx(nullptr);
    I64 result = 0;
    CHECK(drive_task(fx.host(), [&](plugins::Host h) {
              return typed_pipe(h, &result);
          }) == DFTU_OK);
    CHECK(result == 20);  // (5*2)+10
}

TEST_CASE("plugin ABI: job all concatenates outputs") {
    FoldFixture<CountSlice> fx(nullptr);
    std::array<I64, 2> out{0, 0};
    CHECK(drive_task(fx.host(), [&](plugins::Host h) {
              return jobs_all(h, &out);
          }) == DFTU_OK);
    CHECK(out[0] == 10);  // 5*2
    CHECK(out[1] == 15);  // 5+10
}

TEST_CASE("plugin ABI: job any yields the winner's value") {
    FoldFixture<CountSlice> fx(nullptr);
    I64 result = 0;
    CHECK(drive_task(fx.host(), [&](plugins::Host h) {
              return jobs_any(h, &result);
          }) == DFTU_OK);
    CHECK(result == 15);  // both racers compute 5+10
}

TEST_CASE("plugin ABI: job map runs the body once per item in order") {
    FoldFixture<CountSlice> fx(nullptr);
    dstd::Vector<I64> out;
    CHECK(drive_task(fx.host(), [&](plugins::Host h) {
              return jobs_map(h, &out);
          }) == DFTU_OK);
    REQUIRE(out.size() == 4);
    CHECK(out[0] == 1);
    CHECK(out[1] == 4);
    CHECK(out[2] == 9);
    CHECK(out[3] == 16);
}

TEST_CASE("plugin ABI: a column rides the job pipe as a DFTU_T_SERIES value") {
    static_assert(plugins::type_tag<dftu_series*>() == DFTU_T_SERIES,
                  "a dftu_series* handle tags as DFTU_T_SERIES");
    static_assert(plugins::type_tag<dftu_dataframe*>() == DFTU_T_TABLE,
                  "a dftu_dataframe* handle tags as DFTU_T_TABLE");
    FoldFixture<CountSlice> fx(nullptr);
    dftu_series* out = nullptr;
    CHECK(drive_task(fx.host(), [&](plugins::Host h) {
              return series_pipe(h, &out);
          }) == DFTU_OK);

    REQUIRE(out != nullptr);
    REQUIRE(dftu_series_length(out) == 4);
    const std::int64_t* d =
        static_cast<const std::int64_t*>(dftu_series_data(out));
    CHECK(d[0] == 4);
    CHECK(d[1] == 8);
    CHECK(d[2] == 12);
    CHECK(d[3] == 16);
    dftu_series_free(out);
}

TEST_CASE(
    "plugin ABI: DFTU_T_SERIES and DFTU_T_TABLE are distinct handle tags") {
    FoldFixture<CountSlice> fx(nullptr);
    test_plugin_common::Ctx c;
    plugins::Host h = plugins::detail::host_from_raw(&fx.host(), c.get());
    // Both are 8-byte handle values; as opaque DFTU_T_BYTES they would have
    // piped. A distinct tag makes a column-out reject a table-in, and accept a
    // column-in. (Bodies never run; `then` only checks the type + size seam.)
    dftu_job *col_out = nullptr, *tbl_in = nullptr, *col_in = nullptr,
             *piped = nullptr;
    REQUIRE(make_raw_job(h, DFTU_T_I64, DFTU_T_SERIES, &col_out) == DFTU_OK);
    REQUIRE(make_raw_job(h, DFTU_T_TABLE, DFTU_T_I64, &tbl_in) == DFTU_OK);
    REQUIRE(make_raw_job(h, DFTU_T_SERIES, DFTU_T_I64, &col_in) == DFTU_OK);
    CHECK(plugins::detail::task_service(h).value()->then(
              c.get(), plugins::detail::host_raw(h)->h, col_out, tbl_in,
              &piped) == DFTU_E_INVALID);  // SERIES out != TABLE in
    CHECK(plugins::detail::task_service(h).value()->then(
              c.get(), plugins::detail::host_raw(h)->h, col_out, col_in,
              &piped) == DFTU_OK);         // SERIES out -> SERIES in
}

TEST_CASE("plugin ABI: job then rejects a type mismatch") {
    FoldFixture<CountSlice> fx(nullptr);
    test_plugin_common::Ctx c;
    plugins::Host h = plugins::detail::host_from_raw(&fx.host(), c.get());
    dftu_job *a = nullptr, *f = nullptr, *i = nullptr, *piped = nullptr;
    REQUIRE(make_raw_job(h, DFTU_T_I64, DFTU_T_I64, &a) == DFTU_OK);
    // Same byte size (8) but a different type must not pipe.
    REQUIRE(make_raw_job(h, DFTU_T_F64, DFTU_T_F64, &f) == DFTU_OK);
    REQUIRE(make_raw_job(h, DFTU_T_I64, DFTU_T_I64, &i) == DFTU_OK);
    CHECK(plugins::detail::task_service(h).value()->then(
              c.get(), plugins::detail::host_raw(h)->h, a, f, &piped) ==
          DFTU_E_INVALID);
    CHECK_FALSE(c.message().empty());
    CHECK(plugins::detail::task_service(h).value()->then(
              c.get(), plugins::detail::host_raw(h)->h, a, i, &piped) ==
          DFTU_OK);
}

TEST_CASE("plugin ABI: host utility ops fnv1a and hex64_parse") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();
    test_plugin_common::Ctx c;
    dftracer::utils::plugins::Host h =
        dftracer::utils::plugins::detail::host_from_raw(&host, c.get());

    const char data[] = "POSIXabc0123456789abcdefnothexnothex1234";
    const std::int32_t offsets[] = {0, 5, 8, 24, 40};
    dftu_series* in =
        dftu_series_new_string(DFTU_TYPE_STRING, offsets, data, 4, nullptr);
    REQUIRE(in != nullptr);

    auto hashed_result =
        h.run_op("dftu.hash.fnv1a", {dataframe::ColumnView{in}});
    REQUIRE(hashed_result.has_value());
    dftu_series* hashed = hashed_result->get();
    REQUIRE(hashed != nullptr);
    const auto* hv =
        static_cast<const std::uint64_t*>(dftu_series_data(hashed));
    REQUIRE(hv != nullptr);
    CHECK(hv[0] == 5527563327133061832ULL);  // fnv1a("POSIX")

    auto parsed_result =
        h.run_op("dftu.hex.parse64", {dataframe::ColumnView{in}});
    REQUIRE(parsed_result.has_value());
    dftu_series* parsed = parsed_result->get();
    REQUIRE(parsed != nullptr);
    const auto* pv =
        static_cast<const std::uint64_t*>(dftu_series_data(parsed));
    REQUIRE(pv != nullptr);
    CHECK(pv[2] == 0x0123456789abcdefULL);
    CHECK(dftu_series_is_null(parsed, 0));  // "POSIX" is not 16 hex digits
    CHECK(dftu_series_is_null(parsed, 1));  // "abc" is too short
    CHECK(dftu_series_is_null(parsed, 3));  // 16 chars, not all hex

    dftu_series_free(in);
}
