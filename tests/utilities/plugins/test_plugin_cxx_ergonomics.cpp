// SPDX-License-Identifier: MIT
// Exercises the additive C++-ergonomics layer over the plugin C ABI: the
// Sketch RAII owner, co_await-able Io ops, the StrId interning view, the
// AggCol / agg:: aggregate builders, and the owning Arrow helpers.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/runtime/runtime.h>
// After fold_adapter.h so nanoarrow is set up before dataframe/abi.h's
// arrow_abi.
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg_op_codes.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/result_registry.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstring>
#include <utility>
#include <variant>

using namespace dftracer::utils;

namespace {

struct TrivialSlice {
    explicit TrivialSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftu_dataframe*, dftracer::utils::plugins::Host) {}
    void merge(TrivialSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

struct HostFixture {
    dstd::StringIntern intern;
    dftu_plugin* plugin =
        dftracer::utils::plugins::make_plugin<TrivialSlice>(nullptr);
    dstd::UniquePtr<plugins::PluginFold> fold =
        dstd::make_unique<plugins::PluginFold>(plugin, intern);
    dftu_plugin_host& host() { return fold->host(); }
    dftu_str intern_str(const char* s) {
        return host().intern(host().h, s,
                             static_cast<std::uint32_t>(std::strlen(s)));
    }
    ~HostFixture() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
};

template <class MakeTask>
void drive_task(dftu_plugin_host& host, int workers, MakeTask make) {
    Runtime rt(workers);
    dftu_utils_test::ok(
        rt.submit(coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                      dftu_task* driver =
                          dftracer::utils::plugins::detail::drive_coro<int>(
                              &host, make());
                      co_await *reinterpret_cast<coro::CoroTask<void>*>(driver);
                  }),
                  "caller")
            .wait());
    rt.shutdown();
}

// co_awaits the Io ops directly, and once through Host::await to prove the
// AsyncOp->dftu_task* decay still composes with the old call site.
dftracer::utils::plugins::Task read_probe(dftracer::utils::plugins::Host h,
                                          const char* path,
                                          std::int64_t* bytes) {
    dftracer::utils::plugins::Io io = h.io();
    int fd = -1;
    co_await io.open(path, O_RDONLY, 0, &fd);
    if (fd < 0) co_return;
    char buf[128];
    std::int64_t n = 0;
    co_await io.read(fd, buf, sizeof(buf), &n);
    if (n > 0) *bytes += n;
    int rc = 0;
    co_await h.await(io.close(fd, &rc));
}

}  // namespace

TEST_CASE("plugin cxx: Sketch RAII adds, merges, and frees") {
    HostFixture fx;
    dftracer::utils::plugins::Host h{&fx.host()};

    dftracer::utils::plugins::Sketch a = h.make_sketch();
    dftracer::utils::plugins::Sketch b{h};
    REQUIRE(a);
    REQUIRE(b);

    for (int v = 1; v <= 500; ++v) a.add(static_cast<double>(v));
    for (int v = 501; v <= 1000; ++v) b.add(static_cast<double>(v));
    a.merge(b);

    dftu_quantiles q = a.result();
    CHECK(q.count == 1000);
    CHECK(q.min == doctest::Approx(1.0));
    CHECK(q.max == doctest::Approx(1000.0));
    CHECK(q.mean == doctest::Approx(500.5).epsilon(0.01));

    // Moved-from Sketch is empty; its destructor must not double-free (asan).
    dftracer::utils::plugins::Sketch c = std::move(a);
    CHECK(!a);
    CHECK(c);
    CHECK(c.result().count == 1000);
}

TEST_CASE("plugin cxx: Io ops are co_await-able directly") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String path = env.dir() + "/probe.txt";
    const dstd::String body = "alpha\nbeta\ngamma\n";
    {
        dstd::fs::write_file(dstd::PathView(path), body).discard();
    }

    HostFixture fx;
    std::int64_t bytes = 0;
    drive_task(fx.host(), 1, [&] {
        return read_probe(dftracer::utils::plugins::Host{&fx.host()},
                          path.c_str(), &bytes);
    });
    CHECK(bytes == static_cast<std::int64_t>(body.size()));
}

TEST_CASE("plugin cxx: StrId hides the raw interned-id integer") {
    HostFixture fx;
    dftracer::utils::plugins::Host h{&fx.host()};

    const dftracer::utils::plugins::StrId id = h.intern("POSIX");
    CHECK_FALSE(id.absent());
    CHECK(h.str(id) == "POSIX");
    CHECK(h.intern("POSIX") == id);  // interning is stable
    CHECK(h.intern("STDIO") != id);

    // A default StrId is the DFTU_STR_NONE sentinel and resolves to nothing.
    const dftracer::utils::plugins::StrId none;
    CHECK(none.absent());
    CHECK(none.raw() == DFTU_STR_NONE);
    CHECK(h.str(none).empty());
}

TEST_CASE("plugin cxx: AggCol builds a dftu_agg_col without hand casts") {
    const dftu_agg_col raw = plugins::AggCol(plugins::AggOp::Pct)
                                 .value("dur")
                                 .out("p90_dur")
                                 .param(0.9)
                                 .raw();
    CHECK(raw.op == DFTU_AGG_PCT);
    CHECK(dstd::String(raw.value) == "dur");
    CHECK(dstd::String(raw.out) == "p90_dur");
    CHECK(raw.param == doctest::Approx(0.9));
    CHECK(raw.by == nullptr);

    // The op stays a named enum on both sides of the seam.
    CHECK(static_cast<dftu_agg_op>(plugins::AggOp::Argmax) == DFTU_AGG_ARGMAX);
    CHECK(static_cast<dftu_agg_op>(plugins::AggOp::SetUnion) ==
          DFTU_AGG_SET_UNION);
}

TEST_CASE("plugin cxx: agg:: factories fill only the fields their op takes") {
    // Single-input reductions: value + out, no param, no by.
    for (const plugins::AggCol& c :
         {plugins::agg::sumsq("dur", "sumsq_dur"),
          plugins::agg::var("dur", "var_dur"),
          plugins::agg::std_dev("dur", "std_dur"),
          plugins::agg::skew("dur", "skew_dur"),
          plugins::agg::kurt("dur", "kurt_dur"),
          plugins::agg::first("name", "first_name"),
          plugins::agg::last("name", "last_name"),
          plugins::agg::set_union("name", "names"),
          plugins::agg::hist("dur", "hist_dur"),
          plugins::agg::count_valid("dur", "n_dur")}) {
        const dftu_agg_col raw = c.raw();
        CHECK(raw.value != nullptr);
        CHECK(raw.out != nullptr);
        CHECK(raw.by == nullptr);
        CHECK(raw.param == doctest::Approx(0.0));
    }
    CHECK(plugins::agg::sumsq("dur", "sumsq_dur").raw().op == DFTU_AGG_SUMSQ);
    CHECK(plugins::agg::hist("dur", "hist_dur").raw().op == DFTU_AGG_HIST);
    CHECK(plugins::agg::count_valid("dur", "n_dur").raw().op ==
          DFTU_AGG_COUNT_VALID);

    // The occupancy ops take the (ts, dur) pair plus the snap tolerance.
    const dftu_agg_col conc =
        plugins::agg::concurrency("ts", "dur", "conc", 4.0).raw();
    CHECK(conc.op == DFTU_AGG_CONCURRENCY);
    CHECK(dstd::String(conc.value) == "ts");
    CHECK(dstd::String(conc.by) == "dur");
    CHECK(conc.param == doctest::Approx(4.0));

    const dftu_agg_col util =
        plugins::agg::utilization("ts", "dur", "util").raw();
    CHECK(util.op == DFTU_AGG_UTILIZATION);
    CHECK(util.param == doctest::Approx(0.0));  // exact endpoints by default

    const dftu_agg_col act = plugins::agg::active("ts", "dur", "peak").raw();
    CHECK(act.op == DFTU_AGG_ACTIVE);
    CHECK(dstd::String(act.by) == "dur");
}

namespace {
// A Slice that checks the Batch/Event cursor from inside step(), since a
// dftu_dataframe batch is valid only for the on_batch call that delivers it.
struct BatchCheckSlice {
    std::uint32_t seen = 0;
    explicit BatchCheckSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftracer::utils::plugins::Batch& b,
              dftracer::utils::plugins::Host) {
        CHECK(b.size() == 3);
        CHECK(!b.empty());

        std::uint32_t i = 0;
        std::uint64_t pid_sum = 0, dur_sum = 0;
        for (const dftracer::utils::plugins::Event& e : b) {
            CHECK(e.pid() == 100 + i);
            CHECK(e.tid() == 200 + i);
            CHECK(e.ts() == 1000 + i);
            CHECK(e.dur() == 10 * (i + 1));
            CHECK(e.has_dur());
            CHECK(e.phase() == dftracer::utils::plugins::Phase::Complete);
            CHECK(e.cat() == "POSIX");
            CHECK(e.name() == "read");
            CHECK(!e.has_arg("ret"));
            pid_sum += e.pid();
            dur_sum += e.dur();
            ++i;
        }
        CHECK(i == 3);
        CHECK(pid_sum == 100 + 101 + 102);
        CHECK(dur_sum == 10 + 20 + 30);

        // operator[] agrees with iteration.
        CHECK(b[0].pid() == 100);
        CHECK(b[2].dur() == 30);
        seen += static_cast<std::uint32_t>(b.size());
    }
    void merge(BatchCheckSlice& o) { seen += o.seen; }
    void finalize(dftracer::utils::plugins::Host) {}
};
}  // namespace

TEST_CASE("plugin cxx: Batch view iterates typed Events") {
    dstd::StringIntern intern;
    dftu_plugin* p =
        dftracer::utils::plugins::make_plugin<BatchCheckSlice>(nullptr);
    {
        // fold must be destroyed (it calls plugin_->destroy_slice) before
        // p->destroy frees the plugin's own storage, which the vtable pointed
        // to by `p` lives inside of.
        plugins::PluginFold fold(p, intern);

        const std::uint32_t N = 3;
        dstd::Vector<trace::views::detail::FoldEvent> evs(N);
        for (std::uint32_t i = 0; i < N; ++i) {
            evs[i] = trace::views::detail::FoldEvent{};
            evs[i].cat_id = intern.get_or_insert("POSIX");
            evs[i].name_id = intern.get_or_insert("read");
            evs[i].pid = 100 + i;
            evs[i].tid = 200 + i;
            evs[i].ts = 1000 + i;
            evs[i].dur = 10 * (i + 1);
            evs[i].phase = dftracer::utils::trace::RecordPhase::Complete;
            evs[i].has_dur = true;
        }
        trace::views::detail::ScanUnit unit{};
        dftu_utils_test::ok(fold.step(trace::views::detail::FoldBatch{
            dstd::Span<const trace::views::detail::FoldEvent>(evs), unit, {}}));
    }

    if (p->destroy) p->destroy(p->self);
}

TEST_CASE("plugin cxx: Event args expose typed values and dotted lookup") {
    dstd::StringIntern intern;
    struct ArgCheckSlice {
        explicit ArgCheckSlice(const dftracer::utils::plugins::Config&) {}
        void step(const dftracer::utils::plugins::Batch& b,
                  dftracer::utils::plugins::Host) {
            const dftracer::utils::plugins::Event& e = b[0];
            CHECK(e.pid() == 7);
            CHECK(e.has_arg("ret"));
            CHECK(e.arg_is_i64("ret"));
            CHECK(e.arg_i64("ret") == 42);
            CHECK(e.has_arg("fd"));
            CHECK(e.arg_is_f64("fd"));
            CHECK(e.arg_f64("fd") == doctest::Approx(2.5));
            CHECK(e.has_arg("path"));
            CHECK(e.arg_is_str("path"));
            CHECK(e.arg_str("path") == "/tmp/a");
            CHECK(!e.has_arg("missing"));
        }
        void merge(ArgCheckSlice&) {}
        void finalize(dftracer::utils::plugins::Host) {}
    };
    dftu_plugin* p =
        dftracer::utils::plugins::make_plugin<ArgCheckSlice>(nullptr);
    {
        // fold must be destroyed (it calls plugin_->destroy_slice) before
        // p->destroy frees the plugin's own storage, which the vtable pointed
        // to by `p` lives inside of.
        plugins::PluginFold fold(p, intern);

        trace::views::detail::FoldEvent fe;
        fe.pid = 7;
        fe.phase = dftracer::utils::trace::RecordPhase::Complete;
        fe.args.emplace_back(intern.get_or_insert("ret"), std::int64_t{42});
        fe.args.emplace_back(intern.get_or_insert("fd"), 2.5);
        fe.args.emplace_back(intern.get_or_insert("path"),
                             intern.get_or_insert("/tmp/a"));
        dstd::Vector<trace::views::detail::FoldEvent> evs = {fe};
        trace::views::detail::ScanUnit unit{};
        dftu_utils_test::ok(fold.step(trace::views::detail::FoldBatch{
            dstd::Span<const trace::views::detail::FoldEvent>(evs), unit, {}}));
    }

    if (p->destroy) p->destroy(p->self);
}

namespace {

// A Slice authored against the ergonomic Batch view: range-for over typed
// Events, no hand indexing of the raw dftu_dataframe columns.
struct ViewSlice {
    std::uint64_t count = 0;
    std::uint64_t pid_sum = 0;
    std::uint64_t ts_max = 0;
    explicit ViewSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftracer::utils::plugins::Batch& b,
              dftracer::utils::plugins::Host) {
        for (const dftracer::utils::plugins::Event& e : b) {
            ++count;
            pid_sum += e.pid();
            if (e.ts() > ts_max) ts_max = e.ts();
        }
    }
    void merge(ViewSlice& o) {
        count += o.count;
        pid_sum += o.pid_sum;
        if (o.ts_max > ts_max) ts_max = o.ts_max;
    }
    void finalize(dftracer::utils::plugins::Host) {}
};

}  // namespace

TEST_CASE("plugin cxx: make_plugin dispatches a Batch-view step") {
    HostFixture fx;
    dftu_plugin* p = dftracer::utils::plugins::make_plugin<ViewSlice>(nullptr);
    auto* slice = static_cast<ViewSlice*>(p->make_slice(p->self));
    REQUIRE(slice != nullptr);

    const std::uint32_t N = 4;
    dstd::Vector<std::uint64_t> pid(N), ts(N);
    for (std::uint32_t i = 0; i < N; ++i) {
        pid[i] = 10 + i;
        ts[i] = 500 + i;
    }
    const char* col_names[2] = {"pid", "ts"};
    const auto n64 = static_cast<std::int64_t>(N);
    dftu_series* cols[2] = {
        dftu_series_new_flat(DFTU_TYPE_UINT64, pid.data(), n64, nullptr),
        dftu_series_new_flat(DFTU_TYPE_UINT64, ts.data(), n64, nullptr)};
    dftu_dataframe* df = dftu_dataframe_new(col_names, cols, 2);
    REQUIRE(df != nullptr);

    dftu_task* t = p->on_batch(slice, df, &fx.host());
    dftu_dataframe_free(df);
    CHECK(t == nullptr);  // synchronous step returns no task
    CHECK(slice->count == N);
    CHECK(slice->pid_sum == 10 + 11 + 12 + 13);
    CHECK(slice->ts_max == 503);

    p->destroy_slice(slice);
    if (p->destroy) p->destroy(p->self);
}

namespace {

trace::views::detail::FoldEvent evt(std::uint64_t pid, std::uint64_t dur,
                                    std::uint64_t tid) {
    trace::views::detail::FoldEvent e;
    e.pid = pid;
    e.dur = dur;
    e.has_dur = true;
    e.tid = tid;
    e.phase = dftracer::utils::trace::RecordPhase::Complete;
    return e;
}

// One accumulator keyed on pid built through the ergonomic Host::agg + agg::
// factories: a per-key row count and the tid at the largest duration. Neither
// names a raw DFTU_AGG_* code nor fills a dftu_agg_col by hand.
::dftu_task* agg_facade_columns(void*, const dftu_dataframe* df,
                                const dftu_plugin_host* host) {
    dftracer::utils::plugins::Host h{host};
    const auto acc = h.agg("by_pid", {"pid"},
                           {plugins::agg::count("hits"),
                            plugins::agg::argmax("tid", "tid_at_max", "dur")});
    if (acc) acc.accumulate(df);
    return nullptr;
}

dftu_plugin make_agg_facade_plugin() {
    dftu_plugin p{};
    p.abi_version = DFTRACER_UTILS_PLUGIN_ABI_VERSION;
    p.plan_duql = [](void*) -> const char* { return nullptr; };
    p.make_slice = [](void*) -> void* {
        static int sentinel;
        return &sentinel;
    };
    p.merge = [](void*, void*) {};
    p.on_finalize = [](void*, const dftu_plugin_host*) -> ::dftu_task* {
        return nullptr;
    };
    p.destroy_slice = [](void*) {};
    p.destroy = [](void*) {};
    p.on_batch = agg_facade_columns;
    return p;
}

dstd::String cell_str(const dftu_series* col, std::int64_t r) {
    dftu_series* flat = dftu_series_materialize(col);
    const std::int32_t* off = dftu_series_offsets(flat);
    const char* data = static_cast<const char*>(dftu_series_data(flat));
    dstd::String out(data + off[r],
                     static_cast<std::size_t>(off[r + 1] - off[r]));
    dftu_series_free(flat);
    return out;
}

double cell_num(const dftu_series* col, std::int64_t r) {
    dftu_series* flat = dftu_series_materialize(col);
    const void* d = dftu_series_data(flat);
    double out = 0.0;
    switch (dftu_series_type(flat)) {
        case DFTU_TYPE_INT64:
            out = static_cast<double>(static_cast<const std::int64_t*>(d)[r]);
            break;
        case DFTU_TYPE_UINT64:
            out = static_cast<double>(static_cast<const std::uint64_t*>(d)[r]);
            break;
        case DFTU_TYPE_FLOAT64:
            out = static_cast<const double*>(d)[r];
            break;
        default:
            break;
    }
    dftu_series_free(flat);
    return out;
}

}  // namespace

TEST_CASE("plugin cxx: Host::agg merges a count and an argmax across slices") {
    dstd::StringIntern intern;
    dftu_plugin p = make_agg_facade_plugin();
    plugins::NamedResultRegistry named;
    plugins::PluginFold master(&p, intern, nullptr, &named);

    // Merged counts: pid 1 -> 3, pid 2 -> 2. Argmax tid by duration: pid 1 at
    // dur 30 -> tid 300; pid 2 at dur 50 -> tid 500.
    const dstd::Vector<dstd::Vector<trace::views::detail::FoldEvent>> slices = {
        {evt(1, 10, 100), evt(1, 30, 300), evt(2, 5, 50)},
        {evt(1, 20, 200), evt(2, 50, 500)}};
    for (const auto& evs : slices) {
        auto slice = master.slice();
        auto* pf = static_cast<plugins::PluginFold*>(slice.get());
        trace::views::detail::ScanUnit unit;
        dftu_utils_test::ok(pf->step(trace::views::detail::FoldBatch{
            dstd::Span<const trace::views::detail::FoldEvent>(evs), unit, {}}));
        master.merge(*pf);
    }
    Runtime rt(1);
    dftu_utils_test::ok(
        rt.submit(coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                      co_await master.finalize(
                          trace::views::detail::CoverageSet{});
                  }),
                  "fin")
            .wait());
    rt.shutdown();

    auto it = named.results().find("by_pid");
    REQUIRE(it != named.results().end());
    REQUIRE(std::holds_alternative<plugins::OwnedDataFrame>(it->second));
    dftu_dataframe* out = std::get<plugins::OwnedDataFrame>(it->second).handle;
    REQUIRE(out != nullptr);
    REQUIRE(dftu_dataframe_num_rows(out) == 2);

    dftu_series* pid = dftu_dataframe_column(out, "pid");
    dftu_series* hits = dftu_dataframe_column(out, "hits");
    dftu_series* tid_at_max = dftu_dataframe_column(out, "tid_at_max");
    REQUIRE(pid);
    REQUIRE(hits);
    REQUIRE(tid_at_max);

    for (std::int64_t r = 0; r < dftu_dataframe_num_rows(out); ++r) {
        if (cell_num(pid, r) == doctest::Approx(1.0)) {
            CHECK(cell_num(hits, r) == doctest::Approx(3.0));
            CHECK(cell_str(tid_at_max, r) == "300");
        } else {
            CHECK(cell_num(pid, r) == doctest::Approx(2.0));
            CHECK(cell_num(hits, r) == doctest::Approx(2.0));
            CHECK(cell_str(tid_at_max, r) == "500");
        }
    }

    dftu_series_free(pid);
    dftu_series_free(hits);
    dftu_series_free(tid_at_max);
}
