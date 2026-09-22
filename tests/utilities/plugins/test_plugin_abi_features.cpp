// In-process coverage for the plugin ABI surface the host-service suite leaves
// untested: the config tree, query compile/match and plan_query event routing,
// and the coroutine control ops (drive path).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/plugins/config.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>

#include <memory>
// After plugin.h/fold_adapter.h so nanoarrow is already set up for arrow_abi.h.
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/compose.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing_utilities.h"

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::StringIntern;
using dftracer::utils::plugins::ConfigTree;
using dftracer::utils::plugins::PluginFold;
namespace coro = dftracer::utils::coro;
namespace views = dftracer::utils::trace::views;
using dftracer::utils::trace::RecordPhase;
using views::detail::FoldBatch;
using views::detail::FoldEvent;
using views::detail::ScanUnit;

namespace {

coro::CoroTask<void>* as_coro(::dftu_task* t) {
    return reinterpret_cast<coro::CoroTask<void>*>(t);
}

struct ConfigSlice {
    std::int64_t threshold = 0;
    double ratio = 0.0;
    std::string label;
    bool enabled = false;
    std::int64_t inner = 0;
    std::int64_t missing = 0;
    std::vector<std::int64_t> buckets;

    explicit ConfigSlice(const dftracer::utils::plugins::Config& c) {
        threshold = c.get_int("threshold");
        ratio = c.get_double("ratio");
        label = std::string(c.get("label"));
        enabled = c.get_bool("enabled");
        inner = c.child("nested").get_int("inner");
        missing = c.get_int("missing", -1);
        const dftu_value* arr = c.find("buckets");
        if (arr && arr->kind == DFTU_VAL_ARRAY)
            for (std::uint32_t i = 0; i < arr->count; ++i)
                buckets.push_back(dftu_as_i64(&arr->as.items[i], 0));
    }
    void step(const dftu_dataframe*, dftracer::utils::plugins::Host) {}
    void merge(ConfigSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

struct CountState {
    std::uint64_t seen = 0;
    std::vector<std::string> fhashes;
    std::vector<std::string> hhashes;
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

FoldEvent make_event(StringIntern& intern, const char* fhash,
                     const char* hhash) {
    FoldEvent fe;
    fe.cat_id = intern.get_or_insert("POSIX");
    fe.name_id = intern.get_or_insert("read");
    fe.fhash_id = intern.get_or_insert(fhash);
    fe.hhash_id = intern.get_or_insert(hhash);
    fe.phase = RecordPhase::COMPLETE;
    fe.ts = 1;
    fe.dur = 5;
    fe.has_dur = true;
    return fe;
}

void inc_fn(void* p) { static_cast<std::atomic<int>*>(p)->fetch_add(1); }
void append1_fn(void* p) { static_cast<std::vector<int>*>(p)->push_back(1); }
void append2_fn(void* p) { static_cast<std::vector<int>*>(p)->push_back(2); }

const dftu_svc_coro* coro_ext(dftracer::utils::plugins::Host h) {
    return static_cast<const dftu_svc_coro*>(
        h.raw()->get_service(h.raw()->h, DFTU_SVC_CORO));
}

dftracer::utils::plugins::Task fanout_all(dftracer::utils::plugins::Host h,
                                          std::atomic<int>* ran, int n) {
    const dftu_svc_coro* c = coro_ext(h);
    std::vector<::dftu_task*> kids;
    for (int i = 0; i < n; ++i)
        kids.push_back(c->spawn(h.raw()->h, inc_fn, ran));
    ::dftu_task* all =
        c->when_all(h.raw()->h, kids.data(), static_cast<std::uint32_t>(n));
    co_await h.await(all);
}

dftracer::utils::plugins::Task fanout_any(dftracer::utils::plugins::Host h,
                                          std::atomic<int>* ran) {
    const dftu_svc_coro* c = coro_ext(h);
    ::dftu_task* a = c->spawn(h.raw()->h, inc_fn, ran);
    ::dftu_task* b = c->spawn(h.raw()->h, inc_fn, ran);
    ::dftu_task* kids[2] = {a, b};
    co_await h.await(c->when_any(h.raw()->h, kids, 2));
}

dftracer::utils::plugins::Task chain(dftracer::utils::plugins::Host h,
                                     std::vector<int>* order) {
    const dftu_svc_coro* c = coro_ext(h);
    ::dftu_task* t = c->spawn(h.raw()->h, append1_fn, order);
    co_await h.await(c->then(h.raw()->h, t, append2_fn, order));
}

dftracer::utils::plugins::Task compose_all(dftracer::utils::plugins::Host h,
                                           std::atomic<int>* ran) {
    const dftu_svc_coro* c = coro_ext(h);
    ::dftu_task* a = c->spawn(h.raw()->h, inc_fn, ran);
    ::dftu_task* b = c->spawn(h.raw()->h, inc_fn, ran);
    co_await (dftracer::utils::plugins::compose(h.raw(), a) &&
              dftracer::utils::plugins::compose(h.raw(), b));
}

dftracer::utils::plugins::Task compose_any(dftracer::utils::plugins::Host h,
                                           std::atomic<int>* ran) {
    const dftu_svc_coro* c = coro_ext(h);
    ::dftu_task* a = c->spawn(h.raw()->h, inc_fn, ran);
    ::dftu_task* b = c->spawn(h.raw()->h, inc_fn, ran);
    co_await (dftracer::utils::plugins::compose(h.raw(), a) ||
              dftracer::utils::plugins::compose(h.raw(), b));
}

dftracer::utils::plugins::Task compose_map(dftracer::utils::plugins::Host h,
                                           std::atomic<int>* ran) {
    const dftu_svc_coro* c = coro_ext(h);
    std::vector<int> items = {0, 1, 2, 3};
    co_await dftracer::utils::plugins::map(
        h.raw(), items, [&](int) { return c->spawn(h.raw()->h, inc_fn, ran); });
}

// dftu_op / dftu_svc_compose: leaf value transforms (int64 -> int64), run
// inline.
const dftu_svc_compose* compose_ext(dftracer::utils::plugins::Host h) {
    return static_cast<const dftu_svc_compose*>(
        h.raw()->get_service(h.raw()->h, DFTU_SVC_COMPOSE));
}
::dftu_task* op_double(void*, const void* in, void* out, int* rc) {
    *static_cast<std::int64_t*>(out) =
        *static_cast<const std::int64_t*>(in) * 2;
    if (rc) *rc = 0;
    return nullptr;
}
::dftu_task* op_plus10(void*, const void* in, void* out, int* rc) {
    *static_cast<std::int64_t*>(out) =
        *static_cast<const std::int64_t*>(in) + 10;
    if (rc) *rc = 0;
    return nullptr;
}

// (in*2) then (+10): value threaded through the pipe.
dftracer::utils::plugins::Task op_pipe(dftracer::utils::plugins::Host h,
                                       std::int64_t* result) {
    const dftu_svc_compose* c = compose_ext(h);
    void* hh = h.raw()->h;
    ::dftu_op* a = c->make_op(hh, op_double, nullptr, nullptr, DFTU_T_I64, 8,
                              DFTU_T_I64, 8);
    ::dftu_op* b = c->make_op(hh, op_plus10, nullptr, nullptr, DFTU_T_I64, 8,
                              DFTU_T_I64, 8);
    ::dftu_op* pipe = c->then(hh, a, b);
    std::int64_t in = 5, out = 0;
    int rc = -1;
    co_await h.await(c->run(hh, pipe, &in, &out, &rc));
    *result = (rc == 0) ? out : -1;
}

// Typed compose: compile-time type safety. A matching pipe is well-formed; a
// mismatched one is not (this is the value over the raw runtime null check).
template <class A, class B>
concept Pipeable = requires(A a, B b) { a | b; };
using OpII = dftracer::utils::plugins::Op<std::int64_t, std::int64_t>;
using OpDD = dftracer::utils::plugins::Op<double, double>;
static_assert(Pipeable<OpII, OpII>, "i64->i64 | i64->i64 must pipe");
static_assert(!Pipeable<OpII, OpDD>, "i64->i64 | f64->f64 must not pipe");

dftracer::utils::plugins::Task typed_pipe(dftracer::utils::plugins::Host h,
                                          std::int64_t* result) {
    OpII a = dftracer::utils::plugins::make_op<std::int64_t, std::int64_t>(
        h, [](std::int64_t x) { return x * 2; });
    OpII b = dftracer::utils::plugins::make_op<std::int64_t, std::int64_t>(
        h, [](std::int64_t x) { return x + 10; });
    std::int64_t in = 5, out = 0;
    int rc = -1;
    co_await dftracer::utils::plugins::run(a | b, in, out, rc);
    *result = (rc == 0) ? out : -1;
}

// A real column rides the pipe as a first-class DFTU_T_SERIES value: the source
// [1,2,3,4] is doubled twice via a real SIMD add, so the output is [4,8,12,16].
// Only the `dftu_series*` handle crosses each op (zero-copy); `a` borrows its
// input (the caller keeps `src`), `b` consumes the intermediate.
dftracer::utils::plugins::Task series_pipe(dftracer::utils::plugins::Host h,
                                           dftu_series** result) {
    using SOp = dftracer::utils::plugins::Op<dftu_series*, dftu_series*>;
    std::int64_t vals[4] = {1, 2, 3, 4};
    dftu_series* src = dftu_series_new_flat(DFTU_TYPE_INT64, vals, 4, nullptr);
    SOp a = dftracer::utils::plugins::make_op<dftu_series*, dftu_series*>(
        h, [](dftu_series* s) { return dftu_series_add(s, s); });
    SOp b = dftracer::utils::plugins::make_op<dftu_series*, dftu_series*>(
        h, [](dftu_series* s) {
            dftu_series* r = dftu_series_add(s, s);
            dftu_series_free(s);
            return r;
        });
    dftu_series* out = nullptr;
    int rc = -1;
    co_await dftracer::utils::plugins::run(a | b, src, out, rc);
    dftu_series_free(src);
    *result = (rc == 0) ? out : nullptr;
}

// when_any: both racers compute the same value, so the winner is deterministic.
dftracer::utils::plugins::Task op_any(dftracer::utils::plugins::Host h,
                                      std::int64_t* result) {
    const dftu_svc_compose* c = compose_ext(h);
    void* hh = h.raw()->h;
    ::dftu_op* ops[2] = {c->make_op(hh, op_plus10, nullptr, nullptr, DFTU_T_I64,
                                    8, DFTU_T_I64, 8),
                         c->make_op(hh, op_plus10, nullptr, nullptr, DFTU_T_I64,
                                    8, DFTU_T_I64, 8)};
    ::dftu_op* any = c->when_any(hh, ops, 2);
    std::int64_t in = 5, out = 0;
    int rc = -1;
    co_await h.await(c->run(hh, any, &in, &out, &rc));
    *result = out;
}

// when_all: both leaves fan the same input; output is their values
// concatenated.
dftracer::utils::plugins::Task op_all(dftracer::utils::plugins::Host h,
                                      std::int64_t* out0, std::int64_t* out1) {
    const dftu_svc_compose* c = compose_ext(h);
    void* hh = h.raw()->h;
    ::dftu_op* ops[2] = {c->make_op(hh, op_double, nullptr, nullptr, DFTU_T_I64,
                                    8, DFTU_T_I64, 8),
                         c->make_op(hh, op_plus10, nullptr, nullptr, DFTU_T_I64,
                                    8, DFTU_T_I64, 8)};
    ::dftu_op* all = c->when_all(hh, ops, 2);
    std::int64_t in = 5;
    std::int64_t out[2] = {0, 0};
    int rc = -1;
    co_await h.await(c->run(hh, all, &in, out, &rc));
    *out0 = out[0];
    *out1 = out[1];
}

// Owns a PluginFold plus its backing plugin; the fold references the plugin, so
// it is destroyed first.
template <class Slice>
struct FoldFixture {
    StringIntern intern;
    dftu_plugin* plugin;
    std::unique_ptr<PluginFold> fold;
    explicit FoldFixture(const dftu_value* config)
        : plugin(dftracer::utils::plugins::make_plugin<Slice>(config)),
          fold(std::make_unique<PluginFold>(plugin, intern)) {}
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

void* col_make_slice(void*) { return new ColState(); }
void col_destroy_slice(void* s) { delete static_cast<ColState*>(s); }
void col_destroy(void*) {}
void col_merge(void* into, void* other) {
    static_cast<ColState*>(into)->dur_sum +=
        static_cast<ColState*>(other)->dur_sum;
}
const char* col_plan_query(void*) { return nullptr; }
::dftu_task* col_on_finalize(void*, const dftu_plugin_host*) { return nullptr; }
::dftu_task* col_on_batch(void* slice, const dftu_dataframe* df,
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
    return nullptr;
}

// Owning wrapper for a hand-built [cat, name] test dataframe (freed on scope
// exit), for dftu_svc_query::query_matches tests outside an on_batch call.
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

dftu_series* string_column(const std::vector<std::string>& vals) {
    std::vector<std::int32_t> offsets(vals.size() + 1, 0);
    std::string data;
    for (std::size_t i = 0; i < vals.size(); ++i) {
        data += vals[i];
        offsets[i + 1] = static_cast<std::int32_t>(data.size());
    }
    return dftu_series_new_string(DFTU_TYPE_STRING, offsets.data(), data.data(),
                                  static_cast<std::int64_t>(vals.size()),
                                  nullptr);
}

QueryFrame cat_name_frame(const std::vector<std::string>& cats,
                          const std::vector<std::string>& names) {
    const char* col_names[2] = {"cat", "name"};
    dftu_series* cols[2] = {string_column(cats), string_column(names)};
    QueryFrame f;
    f.df = dftu_dataframe_new(col_names, cols, 2);
    return f;
}

}  // namespace

TEST_CASE("plugin ABI: on_batch hands the batch as columns") {
    g_col_state = {};
    dftu_plugin p{};
    p.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    p.plan_query = col_plan_query;
    p.make_slice = col_make_slice;
    p.merge = col_merge;
    p.on_finalize = col_on_finalize;
    p.destroy_slice = col_destroy_slice;
    p.destroy = col_destroy;
    p.on_batch = col_on_batch;

    StringIntern intern;
    PluginFold fold(&p, intern);
    std::vector<FoldEvent> evs = {make_event(intern, "f", "h"),
                                  make_event(intern, "f", "h"),
                                  make_event(intern, "f", "h")};
    ScanUnit unit{};
    fold.step(FoldBatch{std::span<const FoldEvent>(evs), unit, {}});
    CHECK(g_col_state.dur_sum == 15);  // 3 events x dur 5, SIMD-reduced
}

TEST_CASE("plugin ABI: config tree reads scalars, nested, array, and default") {
    dftu_utils_test::TestEnvironment env(0);
    std::string path = env.get_dir() + "/config.json";
    {
        std::ofstream f(path);
        f << R"({"threshold":42,"ratio":2.5,"label":"posix","enabled":true,)"
          << R"("nested":{"inner":7},"buckets":[10,20,30]})";
    }

    ConfigTree tree = ConfigTree::from_json_file(path);
    dftu_plugin* p =
        dftracer::utils::plugins::make_plugin<ConfigSlice>(tree.root());
    auto* slice = static_cast<ConfigSlice*>(p->make_slice(p->self));
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

TEST_CASE("plugin ABI: query_compile then query_matches on known events") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    const auto* qx = static_cast<const dftu_svc_query*>(
        host.get_service(host.h, DFTU_SVC_QUERY));
    REQUIRE(qx != nullptr);

    std::string src = "cat == \"POSIX\"";
    dftu_query* q = qx->query_compile(host.h, src.data(),
                                      static_cast<std::uint32_t>(src.size()));
    REQUIRE(q != nullptr);

    // Row 0 is the hit (cat=POSIX), row 1 the miss (cat=STDIO); both name
    // "read".
    QueryFrame frame = cat_name_frame({"POSIX", "STDIO"}, {"read", "read"});
    CHECK(qx->query_matches(host.h, q, frame.df, 0) == 1);
    CHECK(qx->query_matches(host.h, q, frame.df, 1) == 0);

    SUBCASE("malformed query source does not compile and match is safe") {
        std::string bad = "cat ==";
        dftu_query* bq = qx->query_compile(
            host.h, bad.data(), static_cast<std::uint32_t>(bad.size()));
        CHECK(bq == nullptr);
        // A null query must yield a defined 0, never a throw across the ABI.
        CHECK(qx->query_matches(host.h, nullptr, frame.df, 0) == 0);
    }
}

TEST_CASE("plugin ABI: plan_query on fhash delivers only matching events") {
    SUBCASE("fhash predicate keeps every match, drops the rest") {
        g_count = CountState{};
        ConfigTree tree;
        tree.set("query", "fhash == \"fhAAA\"");
        FoldFixture<CountSlice> fx(tree.root());

        std::vector<FoldEvent> evs;
        evs.push_back(make_event(fx.intern, "fhAAA", "hh1"));
        evs.push_back(make_event(fx.intern, "fhBBB", "hh1"));
        evs.push_back(make_event(fx.intern, "fhAAA", "hh2"));
        evs.push_back(make_event(fx.intern, "fhCCC", "hh2"));
        evs.push_back(make_event(fx.intern, "fhAAA", "hh3"));
        ScanUnit unit{};
        FoldBatch batch{std::span<const FoldEvent>(evs), unit};

        fx.fold->step(batch);
        CHECK(fx.fold->take_pending() == nullptr);

        // The regression: a plan_query on fhash must not silently drop matches.
        CHECK(g_count.seen == 3);
        for (const auto& f : g_count.fhashes) CHECK(f == "fhAAA");
    }

    SUBCASE("hhash predicate routes on a non cat/name field too") {
        g_count = CountState{};
        ConfigTree tree;
        tree.set("query", "hhash == \"hhZ\"");
        FoldFixture<CountSlice> fx(tree.root());

        std::vector<FoldEvent> evs;
        evs.push_back(make_event(fx.intern, "fh1", "hhZ"));
        evs.push_back(make_event(fx.intern, "fh2", "hhY"));
        evs.push_back(make_event(fx.intern, "fh3", "hhZ"));
        ScanUnit unit{};
        FoldBatch batch{std::span<const FoldEvent>(evs), unit};

        fx.fold->step(batch);
        CHECK(g_count.seen == 2);
        for (const auto& h : g_count.hhashes) CHECK(h == "hhZ");
    }
}

TEST_CASE("plugin ABI: spawn fan-out joined by when_all runs every child") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    Runtime rt(1);
    std::atomic<int> ran{0};
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          ::dftu_task* d = dftracer::utils::plugins::detail::drive_coro<int>(
              &host,
              fanout_all(dftracer::utils::plugins::Host{&host}, &ran, 4));
          co_await *as_coro(d);
      }).wait();
    rt.shutdown();
    CHECK(ran.load() == 4);
}

TEST_CASE("plugin ABI: when_any returns after the first child") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    Runtime rt(1);
    std::atomic<int> ran{0};
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          ::dftu_task* d = dftracer::utils::plugins::detail::drive_coro<int>(
              &host, fanout_any(dftracer::utils::plugins::Host{&host}, &ran));
          co_await *as_coro(d);
      }).wait();
    rt.shutdown();
    CHECK(ran.load() >= 1);
}

TEST_CASE("plugin ABI: compose && joins both tasks (when_all)") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    Runtime rt(1);
    std::atomic<int> ran{0};
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          ::dftu_task* d = dftracer::utils::plugins::detail::drive_coro<int>(
              &host, compose_all(dftracer::utils::plugins::Host{&host}, &ran));
          co_await *as_coro(d);
      }).wait();
    rt.shutdown();
    CHECK(ran.load() == 2);
}

TEST_CASE("plugin ABI: compose || races the tasks (when_any)") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    Runtime rt(1);
    std::atomic<int> ran{0};
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          ::dftu_task* d = dftracer::utils::plugins::detail::drive_coro<int>(
              &host, compose_any(dftracer::utils::plugins::Host{&host}, &ran));
          co_await *as_coro(d);
      }).wait();
    rt.shutdown();
    CHECK(ran.load() >= 1);
}

TEST_CASE("plugin ABI: compose map joins one task per item (when_all)") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    Runtime rt(1);
    std::atomic<int> ran{0};
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          ::dftu_task* d = dftracer::utils::plugins::detail::drive_coro<int>(
              &host, compose_map(dftracer::utils::plugins::Host{&host}, &ran));
          co_await *as_coro(d);
      }).wait();
    rt.shutdown();
    CHECK(ran.load() == 4);
}

TEST_CASE("plugin ABI: compose dftu_op then threads a value through the pipe") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    Runtime rt(1);
    std::int64_t result = 0;
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          ::dftu_task* d = dftracer::utils::plugins::detail::drive_coro<int>(
              &host, op_pipe(dftracer::utils::plugins::Host{&host}, &result));
          co_await *as_coro(d);
      }).wait();
    rt.shutdown();
    CHECK(result == 20);  // (5*2)+10
}

TEST_CASE("plugin ABI: compose dftu_op when_all concatenates outputs") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    Runtime rt(1);
    std::int64_t a = 0, b = 0;
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          ::dftu_task* d = dftracer::utils::plugins::detail::drive_coro<int>(
              &host, op_all(dftracer::utils::plugins::Host{&host}, &a, &b));
          co_await *as_coro(d);
      }).wait();
    rt.shutdown();
    CHECK(a == 10);  // 5*2
    CHECK(b == 15);  // 5+10
}

TEST_CASE("plugin ABI: compose dftu_op when_any yields the winner's value") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    Runtime rt(1);
    std::int64_t result = 0;
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          ::dftu_task* d = dftracer::utils::plugins::detail::drive_coro<int>(
              &host, op_any(dftracer::utils::plugins::Host{&host}, &result));
          co_await *as_coro(d);
      }).wait();
    rt.shutdown();
    CHECK(result == 15);  // both racers compute 5+10
}

TEST_CASE("plugin ABI: typed compose Op pipes real values") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    Runtime rt(1);
    std::int64_t result = 0;
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          ::dftu_task* d = dftracer::utils::plugins::detail::drive_coro<int>(
              &host,
              typed_pipe(dftracer::utils::plugins::Host{&host}, &result));
          co_await *as_coro(d);
      }).wait();
    rt.shutdown();
    CHECK(result == 20);  // (5*2)+10, fully typed
}

TEST_CASE(
    "plugin ABI: a column rides the compose pipe as a DFTU_T_SERIES value") {
    static_assert(
        dftracer::utils::plugins::type_tag<dftu_series*>() == DFTU_T_SERIES,
        "a dftu_series* handle tags as DFTU_T_SERIES");
    static_assert(
        dftracer::utils::plugins::type_tag<dftu_dataframe*>() == DFTU_T_TABLE,
        "a dftu_dataframe* handle tags as DFTU_T_TABLE");
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    Runtime rt(1);
    dftu_series* out = nullptr;
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          ::dftu_task* d = dftracer::utils::plugins::detail::drive_coro<int>(
              &host, series_pipe(dftracer::utils::plugins::Host{&host}, &out));
          co_await *as_coro(d);
      }).wait();
    rt.shutdown();

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
    dftracer::utils::plugins::Host h{&fx.host()};
    const dftu_svc_compose* c = compose_ext(h);
    void* hh = h.raw()->h;
    // Both are 8-byte handle values; as opaque DFTU_T_BYTES they would have
    // piped. A distinct tag makes a column-out reject a table-in, and accept a
    // column-in. (Bodies never run; `then` only checks the type + size seam.)
    ::dftu_op* col_out = c->make_op(hh, op_double, nullptr, nullptr, DFTU_T_I64,
                                    8, DFTU_T_SERIES, 8);
    ::dftu_op* tbl_in = c->make_op(hh, op_double, nullptr, nullptr,
                                   DFTU_T_TABLE, 8, DFTU_T_I64, 8);
    CHECK(c->then(hh, col_out, tbl_in) == nullptr);  // SERIES out != TABLE in
    ::dftu_op* col_in = c->make_op(hh, op_double, nullptr, nullptr,
                                   DFTU_T_SERIES, 8, DFTU_T_I64, 8);
    CHECK(c->then(hh, col_out, col_in) != nullptr);  // SERIES out -> SERIES in
}

TEST_CASE("plugin ABI: compose dftu_op then rejects a type mismatch") {
    FoldFixture<CountSlice> fx(nullptr);
    dftracer::utils::plugins::Host h{&fx.host()};
    const dftu_svc_compose* c = compose_ext(h);
    void* hh = h.raw()->h;
    ::dftu_op* a = c->make_op(hh, op_double, nullptr, nullptr, DFTU_T_I64, 8,
                              DFTU_T_I64, 8);
    // Same byte size (8) but a different type must not pipe.
    ::dftu_op* f = c->make_op(hh, op_plus10, nullptr, nullptr, DFTU_T_F64, 8,
                              DFTU_T_F64, 8);
    CHECK(c->then(hh, a, f) == nullptr);
    // Matching type pipes fine.
    ::dftu_op* i = c->make_op(hh, op_plus10, nullptr, nullptr, DFTU_T_I64, 8,
                              DFTU_T_I64, 8);
    CHECK(c->then(hh, a, i) != nullptr);
}

TEST_CASE("plugin ABI: then chains a continuation after its task") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();

    Runtime rt(1);
    std::vector<int> order;
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          ::dftu_task* d = dftracer::utils::plugins::detail::drive_coro<int>(
              &host, chain(dftracer::utils::plugins::Host{&host}, &order));
          co_await *as_coro(d);
      }).wait();
    rt.shutdown();
    REQUIRE(order.size() == 2);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
}

TEST_CASE("plugin ABI: host utility ops fnv1a and hex64_parse") {
    FoldFixture<CountSlice> fx(nullptr);
    dftu_plugin_host& host = fx.host();
    dftracer::utils::plugins::Host h{&host};

    const char data[] = "POSIXabc0123456789abcdefnothexnothex1234";
    const std::int32_t offsets[] = {0, 5, 8, 24, 40};
    dftu_series* in =
        dftu_series_new_string(DFTU_TYPE_STRING, offsets, data, 4, nullptr);
    REQUIRE(in != nullptr);

    dftu_series* hashed = h.run_op("dftu.hash.fnv1a", {in});
    REQUIRE(hashed != nullptr);
    const auto* hv =
        static_cast<const std::uint64_t*>(dftu_series_data(hashed));
    REQUIRE(hv != nullptr);
    CHECK(hv[0] == 5527563327133061832ULL);  // fnv1a("POSIX")

    dftu_series* parsed = h.run_op("dftu.hex.parse64", {in});
    REQUIRE(parsed != nullptr);
    const auto* pv =
        static_cast<const std::uint64_t*>(dftu_series_data(parsed));
    REQUIRE(pv != nullptr);
    CHECK(pv[2] == 0x0123456789abcdefULL);
    CHECK(dftu_series_is_null(parsed, 0));  // "POSIX" is not 16 hex digits
    CHECK(dftu_series_is_null(parsed, 1));  // "abc" is too short
    CHECK(dftu_series_is_null(parsed, 3));  // 16 chars, not all hex

    dftu_series_free(parsed);
    dftu_series_free(hashed);
    dftu_series_free(in);
}
