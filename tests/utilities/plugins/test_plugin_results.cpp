// Generic named result channel: a plugin emits an opaque blob at finalize and
// the host collects it verbatim into the named-result registry; a second case
// checks the registry moves and releases an emitted Arrow array.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/result_registry.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace views = dftracer::utils::trace::views;
using dftracer::utils::Runtime;
using dftracer::utils::StringIntern;
using dftracer::utils::plugins::NamedResult;
using dftracer::utils::plugins::NamedResultRegistry;
using dftracer::utils::plugins::OwnedArrow;
using dftracer::utils::plugins::OwnedLazyFrame;
using dftracer::utils::plugins::PluginFold;
using dftracer::utils::plugins::SharedResultRegistry;
using views::detail::CoverageSet;
using views::detail::FoldBatch;
using views::detail::FoldEvent;
using views::detail::ScanUnit;
namespace coro = dftracer::utils::coro;

namespace {

// Counts events per slice, merges the counts, and emits "count=<total>" as a
// named opaque blob at finalize via the plugin.h Host wrapper.
struct CountBlobSlice {
    explicit CountBlobSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftracer::utils::plugins::Batch& b,
              dftracer::utils::plugins::Host) {
        total += b.size();
    }
    void merge(CountBlobSlice& o) { total += o.total; }
    void finalize(dftracer::utils::plugins::Host h) {
        std::string s = "count=" + std::to_string(total);
        h.emit_result("counts", s);
    }
    std::uint64_t total = 0;
};

struct FoldHolder {
    dftu_plugin* plugin;
    std::unique_ptr<PluginFold> fold;
    FoldHolder(dftu_plugin* p, StringIntern& intern, SharedResultRegistry* reg,
               NamedResultRegistry* named)
        : plugin(p),
          fold(std::make_unique<PluginFold>(p, intern, reg, named)) {}
    ~FoldHolder() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
};

std::vector<FoldEvent> make_events(std::uint64_t count) {
    std::vector<FoldEvent> evs(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        evs[i].ts = i;
        // step() drops METADATA/UNKNOWN-phase events before it ever reaches
        // on_batch; the default FoldEvent phase is UNKNOWN.
        evs[i].phase = dftracer::utils::trace::RecordPhase::COMPLETE;
    }
    return evs;
}

void finalize_now(PluginFold& f) {
    Runtime rt(1);
    rt.scope("fin", [&](dftracer::utils::CoroScope&) -> coro::CoroTask<void> {
          co_await f.finalize(CoverageSet{});
      }).wait();
    rt.shutdown();
}

int g_released = 0;
void array_release(ArrowArray* a) {
    ++g_released;
    a->release = nullptr;
}
void schema_release(ArrowSchema* s) {
    ++g_released;
    s->release = nullptr;
}

}  // namespace

TEST_SUITE("PluginResults") {
    TEST_CASE("a plugin's emitted blob round-trips to the registry verbatim") {
        StringIntern intern;
        SharedResultRegistry reg;
        NamedResultRegistry named;
        FoldHolder master(
            dftracer::utils::plugins::make_plugin<CountBlobSlice>(nullptr),
            intern, &reg, &named);

        const std::uint64_t counts[3] = {5, 7, 4};
        std::uint64_t total = 0;
        for (std::uint64_t n : counts) {
            auto slice = master.fold->slice();
            auto* pf = static_cast<PluginFold*>(slice.get());
            auto evs = make_events(n);
            ScanUnit unit;
            FoldBatch fb{std::span<const FoldEvent>(evs), unit};
            pf->step(fb);
            master.fold->merge(*pf);
            total += n;
        }
        finalize_now(*master.fold);

        auto it = named.results().find("counts");
        REQUIRE(it != named.results().end());
        REQUIRE(std::holds_alternative<std::vector<std::byte>>(it->second));
        const auto& bytes = std::get<std::vector<std::byte>>(it->second);
        std::string got(reinterpret_cast<const char*>(bytes.data()),
                        bytes.size());
        CHECK(got == "count=" + std::to_string(total));
    }

    TEST_CASE("emit_arrow moves the array/schema and releases them on clear") {
        NamedResultRegistry named;
        g_released = 0;

        ArrowArray a{};
        a.release = array_release;
        ArrowSchema s{};
        s.release = schema_release;

        REQUIRE(named.emit_arrow("tbl", &a, &s) == 0);
        // The registry stole ownership: the source structs are zeroed.
        CHECK(a.release == nullptr);
        CHECK(s.release == nullptr);

        auto it = named.results().find("tbl");
        REQUIRE(it != named.results().end());
        REQUIRE(std::holds_alternative<OwnedArrow>(it->second));
        CHECK(g_released == 0);

        named.clear();
        CHECK(g_released == 2);
    }

    TEST_CASE("emit_arrow rejects null arguments") {
        NamedResultRegistry named;
        ArrowArray a{};
        ArrowSchema s{};
        CHECK(named.emit_arrow(nullptr, &a, &s) == -1);
        CHECK(named.emit_arrow("x", nullptr, &s) == -1);
        CHECK(named.emit_arrow("x", &a, nullptr) == -1);
    }

    TEST_CASE("an emitted lazyframe is owned and collects to the frame") {
        NamedResultRegistry named;

        const std::int64_t v[4] = {5, 15, 20, 8};
        dftu_series* col = dftu_series_new_flat(DFTU_TYPE_INT64, v, 4, nullptr);
        const char* names[1] = {"v"};
        dftu_dataframe* df = dftu_dataframe_new(names, &col, 1);
        REQUIRE(df != nullptr);
        // A self-contained plan over a materialized frame: the supported way to
        // hand a LazyFrame across the ABI.
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);
        dftu_dataframe_free(df);

        named.emit_lazyframe("plan", lf);

        auto it = named.results().find("plan");
        REQUIRE(it != named.results().end());
        REQUIRE(std::holds_alternative<OwnedLazyFrame>(it->second));

        dftu_lazyframe* got = std::get<OwnedLazyFrame>(it->second).handle;
        REQUIRE(got != nullptr);
        dftu_dataframe* out = dftu_lazyframe_collect(got, 0);
        REQUIRE(out != nullptr);
        CHECK(dftu_dataframe_num_rows(out) == 4);
        dftu_dataframe_free(out);

        named.clear();  // frees the owned lazyframe handle (asan-clean).
    }
}
