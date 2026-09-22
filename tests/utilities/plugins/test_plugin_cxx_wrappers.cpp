// Exercises the additive C++ wrappers layered over the plugin C ABI: the typed
// Agg accumulator (Host::agg, the agg:: factories, Host::agg_result), the
// Config array accessors, and the typed publish/consume ports. Each keeps the
// raw Host method it wraps.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/fold_adapter.h>
// After fold_adapter.h so nanoarrow is set up before dataframe/abi.h's
// arrow_abi.
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/result_registry.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace views = dftracer::utils::trace::views;
using dftracer::utils::Runtime;
using dftracer::utils::StringIntern;
using dftracer::utils::plugins::Host;
using dftracer::utils::plugins::NamedResultRegistry;
using dftracer::utils::plugins::OwnedDataFrame;
using dftracer::utils::plugins::PluginFold;
using dftracer::utils::plugins::SharedResultRegistry;
using dftracer::utils::trace::RecordPhase;
using views::detail::CoverageSet;
using views::detail::FoldBatch;
using views::detail::FoldEvent;
using views::detail::FoldPortBus;
using views::detail::ScanUnit;
namespace agg = dftracer::utils::plugins::agg;
namespace coro = dftracer::utils::coro;

namespace {

FoldEvent evt(StringIntern& intern, std::uint64_t pid, std::uint64_t dur,
              const char* name = "read") {
    FoldEvent e;
    e.pid = pid;
    e.dur = dur;
    e.has_dur = true;
    e.name_id = intern.get_or_insert(name);
    e.phase = RecordPhase::COMPLETE;
    return e;
}

// One columnar step builds every accumulator the cases below read: the
// zero-key scalar form, a pid-keyed pair of reductions, a two-key
// (pid, name) count, and a per-pid name set. All through Host::agg + the
// agg:: factories, so no case names a raw DFTU_AGG_* code.
::dftu_task* wrappers_columns(void* slice, const dftu_dataframe* df,
                              const dftu_plugin_host* host) {
    (void)slice;
    Host h{host};
    if (const auto a = h.agg("wrap_scalar", {},
                             {agg::count("n"), agg::sum("dur", "sum_dur")}))
        a.accumulate(df);
    if (const auto a = h.agg("wrap_by_pid", {"pid"},
                             {agg::count("hits"), agg::sum("dur", "sum_dur"),
                              agg::mean("dur", "mean_dur")}))
        a.accumulate(df);
    if (const auto a =
            h.agg("wrap_by_pid_name", {"pid", "name"}, {agg::count("n")}))
        a.accumulate(df);
    if (const auto a =
            h.agg("wrap_names", {"pid"}, {agg::set_union("name", "names")}))
        a.accumulate(df);
    return nullptr;
}

dftu_plugin make_wrappers_plugin() {
    dftu_plugin p{};
    p.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    p.plan_query = [](void*) -> const char* { return nullptr; };
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
    p.on_batch = wrappers_columns;
    return p;
}

// Two worker slices merged into a master. pid 1: durs {10, 20, 5}, names
// {read, read, write}; pid 2: dur 7, name write.
void run_wrappers(StringIntern& intern, NamedResultRegistry& named) {
    dftu_plugin p = make_wrappers_plugin();
    PluginFold master(&p, intern, nullptr, &named);
    const std::vector<std::vector<FoldEvent>> slices = {
        {evt(intern, 1, 10, "read"), evt(intern, 1, 20, "read")},
        {evt(intern, 1, 5, "write"), evt(intern, 2, 7, "write")}};
    for (const auto& evs : slices) {
        auto slice = master.slice();
        auto* pf = static_cast<PluginFold*>(slice.get());
        ScanUnit unit;
        pf->step(FoldBatch{std::span<const FoldEvent>(evs), unit, {}});
        master.merge(*pf);
    }
    Runtime rt(1);
    rt.scope("fin", [&](dftracer::utils::CoroScope&) -> coro::CoroTask<void> {
          co_await master.finalize(CoverageSet{});
      }).wait();
    rt.shutdown();
}

dftu_dataframe* result_frame(NamedResultRegistry& named, const char* name) {
    auto it = named.results().find(name);
    if (it == named.results().end()) return nullptr;
    if (!std::holds_alternative<OwnedDataFrame>(it->second)) return nullptr;
    return std::get<OwnedDataFrame>(it->second).handle;
}

double num_at(const dftu_series* col, std::int64_t r) {
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

std::string str_at(const dftu_series* col, std::int64_t r) {
    dftu_series* flat = dftu_series_materialize(col);
    const std::int32_t* off = dftu_series_offsets(flat);
    const char* data = static_cast<const char*>(dftu_series_data(flat));
    std::string out(data + off[r],
                    static_cast<std::size_t>(off[r + 1] - off[r]));
    dftu_series_free(flat);
    return out;
}

}  // namespace

TEST_SUITE("PluginCxxWrappers") {
    TEST_CASE("a zero-key Agg reduces the whole scan to one row") {
        StringIntern intern;
        NamedResultRegistry named;
        run_wrappers(intern, named);

        dftu_dataframe* out = result_frame(named, "wrap_scalar");
        REQUIRE(out != nullptr);
        REQUIRE(dftu_dataframe_num_rows(out) == 1);

        dftu_series* n = dftu_dataframe_column(out, "n");
        dftu_series* sum = dftu_dataframe_column(out, "sum_dur");
        REQUIRE(n);
        REQUIRE(sum);
        CHECK(num_at(n, 0) == doctest::Approx(4.0));
        CHECK(num_at(sum, 0) == doctest::Approx(42.0));
        dftu_series_free(n);
        dftu_series_free(sum);
    }

    TEST_CASE("a keyed Agg merges several reductions per key") {
        StringIntern intern;
        NamedResultRegistry named;
        run_wrappers(intern, named);

        dftu_dataframe* out = result_frame(named, "wrap_by_pid");
        REQUIRE(out != nullptr);
        REQUIRE(dftu_dataframe_num_rows(out) == 2);

        dftu_series* pid = dftu_dataframe_column(out, "pid");
        dftu_series* hits = dftu_dataframe_column(out, "hits");
        dftu_series* sum = dftu_dataframe_column(out, "sum_dur");
        dftu_series* mean = dftu_dataframe_column(out, "mean_dur");
        REQUIRE(pid);
        REQUIRE(hits);
        REQUIRE(sum);
        REQUIRE(mean);

        bool saw1 = false, saw2 = false;
        for (std::int64_t r = 0; r < dftu_dataframe_num_rows(out); ++r) {
            if (num_at(pid, r) == doctest::Approx(1.0)) {
                CHECK(num_at(hits, r) == doctest::Approx(3.0));
                CHECK(num_at(sum, r) == doctest::Approx(35.0));
                CHECK(num_at(mean, r) == doctest::Approx(35.0 / 3.0));
                saw1 = true;
            } else if (num_at(pid, r) == doctest::Approx(2.0)) {
                CHECK(num_at(hits, r) == doctest::Approx(1.0));
                CHECK(num_at(sum, r) == doctest::Approx(7.0));
                CHECK(num_at(mean, r) == doctest::Approx(7.0));
                saw2 = true;
            }
        }
        CHECK(saw1);
        CHECK(saw2);

        dftu_series_free(pid);
        dftu_series_free(hits);
        dftu_series_free(sum);
        dftu_series_free(mean);
    }

    TEST_CASE("a String key column materializes as text, not a raw id") {
        StringIntern intern;
        NamedResultRegistry named;
        run_wrappers(intern, named);

        dftu_dataframe* out = result_frame(named, "wrap_by_pid_name");
        REQUIRE(out != nullptr);
        REQUIRE(dftu_dataframe_num_rows(out) == 3);

        dftu_series* pid = dftu_dataframe_column(out, "pid");
        dftu_series* name = dftu_dataframe_column(out, "name");
        dftu_series* n = dftu_dataframe_column(out, "n");
        REQUIRE(pid);
        REQUIRE(name);
        REQUIRE(n);

        // Each key column keeps its own type: pid stays numeric, name resolves
        // to the interned bytes.
        std::map<std::string, double> counts;
        for (std::int64_t r = 0; r < dftu_dataframe_num_rows(out); ++r)
            counts[std::to_string(static_cast<int>(num_at(pid, r))) + "/" +
                   str_at(name, r)] = num_at(n, r);
        CHECK(counts["1/read"] == doctest::Approx(2.0));
        CHECK(counts["1/write"] == doctest::Approx(1.0));
        CHECK(counts["2/write"] == doctest::Approx(1.0));

        dftu_series_free(pid);
        dftu_series_free(name);
        dftu_series_free(n);
    }

    TEST_CASE("set_union collapses each key's distinct values") {
        StringIntern intern;
        NamedResultRegistry named;
        run_wrappers(intern, named);

        dftu_dataframe* out = result_frame(named, "wrap_names");
        REQUIRE(out != nullptr);
        REQUIRE(dftu_dataframe_num_rows(out) == 2);

        dftu_series* pid = dftu_dataframe_column(out, "pid");
        dftu_series* names = dftu_dataframe_column(out, "names");
        REQUIRE(pid);
        REQUIRE(names);

        for (std::int64_t r = 0; r < dftu_dataframe_num_rows(out); ++r) {
            const std::string v = str_at(names, r);
            if (num_at(pid, r) == doctest::Approx(1.0)) {
                CHECK(v.find("read") != std::string::npos);
                CHECK(v.find("write") != std::string::npos);
            } else {
                CHECK(v.find("write") != std::string::npos);
                CHECK(v.find("read") == std::string::npos);
            }
        }

        dftu_series_free(pid);
        dftu_series_free(names);
    }

    TEST_CASE("Host::agg_result yields an empty frame for an unknown name") {
        StringIntern intern;
        dftu_plugin p = make_wrappers_plugin();
        auto fold = std::make_unique<PluginFold>(&p, intern);
        Host h{&fold->host()};
        const OwnedDataFrame miss = h.agg_result("no_such_accumulator");
        CHECK(miss.handle == nullptr);
        fold.reset();
    }
}

TEST_SUITE("PluginCxxConfigArrays") {
    TEST_CASE("Config array accessors read i64/f64/str arrays") {
        dftu_value ints[3];
        for (int i = 0; i < 3; ++i) {
            ints[i] = {};
            ints[i].kind = DFTU_VAL_I64;
            ints[i].as.i64 = 10 * (i + 1);
        }
        dftu_value dbls[2];
        for (int i = 0; i < 2; ++i) {
            dbls[i] = {};
            dbls[i].kind = DFTU_VAL_F64;
            dbls[i].as.f64 = 1.5 * (i + 1);
        }
        const char* words[2] = {"alpha", "beta"};
        dftu_value strs[2];
        for (int i = 0; i < 2; ++i) {
            strs[i] = {};
            strs[i].kind = DFTU_VAL_STR;
            strs[i].as.str = words[i];
            strs[i].count = static_cast<std::uint32_t>(std::strlen(words[i]));
        }

        dftu_value arr_i{};
        arr_i.kind = DFTU_VAL_ARRAY;
        arr_i.count = 3;
        arr_i.as.items = ints;
        dftu_value arr_f{};
        arr_f.kind = DFTU_VAL_ARRAY;
        arr_f.count = 2;
        arr_f.as.items = dbls;
        dftu_value arr_s{};
        arr_s.kind = DFTU_VAL_ARRAY;
        arr_s.count = 2;
        arr_s.as.items = strs;

        dftu_member members[3] = {
            {"nums", 4, &arr_i}, {"reals", 5, &arr_f}, {"words", 5, &arr_s}};
        dftu_value root{};
        root.kind = DFTU_VAL_OBJECT;
        root.count = 3;
        root.as.members = members;

        dftracer::utils::plugins::Config cfg{&root};
        CHECK(cfg.get_int_array("nums") ==
              std::vector<std::int64_t>{10, 20, 30});
        CHECK(cfg.get_double_array("reals") == std::vector<double>{1.5, 3.0});
        auto ws = cfg.get_string_array("words");
        REQUIRE(ws.size() == 2);
        CHECK(ws[0] == "alpha");
        CHECK(ws[1] == "beta");

        // Absent / wrong-kind keys yield empty vectors and a null array().
        CHECK(cfg.get_int_array("missing").empty());
        CHECK(cfg.array("missing").is_null());
    }
}

namespace {

constexpr const char* PORT_CAP = "com.example.wrap.port";

struct Edge {
    std::uint64_t count;
    double sum;
};

std::uint64_t g_count = 0;
double g_sum = 0;
bool g_present = false;

// #7 typed ports: the producer publishes an Edge; the consumer reads it back
// as a typed value the same batch.
struct PortProducer {
    explicit PortProducer(const dftracer::utils::plugins::Config&) {}
    void step(const dftracer::utils::plugins::Batch& b,
              dftracer::utils::plugins::Host h) {
        Edge e{static_cast<std::uint64_t>(b.size()), 0.0};
        for (const dftracer::utils::plugins::Event& ev : b)
            e.sum += static_cast<double>(ev.dur());
        auto port = h.publish_port<Edge>(PORT_CAP);
        port.send(e);
    }
    void merge(PortProducer&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

struct PortConsumer {
    explicit PortConsumer(const dftracer::utils::plugins::Config&) {}
    void step(const dftracer::utils::plugins::Batch&,
              dftracer::utils::plugins::Host h) {
        auto in = h.consume_port<Edge>(PORT_CAP);
        std::optional<Edge> v = in.recv();
        g_present = v.has_value();
        g_count = g_present ? v->count : 0;
        g_sum = g_present ? v->sum : 0.0;
    }
    void merge(PortConsumer&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

struct PortHolder {
    dftu_plugin* plugin;
    std::unique_ptr<PluginFold> fold;
    PortHolder(dftu_plugin* p, StringIntern& intern, FoldPortBus* bus)
        : plugin(p), fold(std::make_unique<PluginFold>(p, intern)) {
        fold->bind_port_bus(bus);
    }
    ~PortHolder() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
};

}  // namespace

TEST_SUITE("PluginCxxPorts") {
    TEST_CASE("typed publish_port/consume_port round-trip a struct value") {
        StringIntern intern;
        FoldPortBus bus;
        PortHolder prod(
            dftracer::utils::plugins::make_plugin<PortProducer>(nullptr),
            intern, &bus);
        PortHolder cons(
            dftracer::utils::plugins::make_plugin<PortConsumer>(nullptr),
            intern, &bus);

        std::vector<FoldEvent> evs = {evt(intern, 1, 10), evt(intern, 1, 20),
                                      evt(intern, 2, 30)};
        ScanUnit unit;
        FoldBatch fb{std::span<const FoldEvent>(evs), unit};

        g_present = false;
        g_count = 0;
        g_sum = 0;
        bus.clear();
        prod.fold->step(fb);
        cons.fold->step(fb);
        CHECK(g_present);
        CHECK(g_count == 3);
        CHECK(g_sum == doctest::Approx(60.0));

        // Consumer before producer sees nothing (the ordering rule).
        bus.clear();
        g_present = true;
        cons.fold->step(fb);
        CHECK_FALSE(g_present);
    }
}
