// SPDX-License-Identifier: MIT
// Generic named result channel: a plugin emits an opaque blob at finalize and
// the host collects it verbatim into the named-result registry; a second case
// checks an emitted lazyframe is owned and collects to the frame.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/result_registry.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <variant>

using namespace dftracer::utils;

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
    dstd::Result<void> finalize(dftracer::utils::plugins::Host h) {
        dstd::String s = "count=" + dstd::to_string(total);
        return h.emit_result("counts", s);
    }
    std::uint64_t total = 0;
};

struct FoldHolder {
    dftu_plugin* plugin;
    dstd::UniquePtr<plugins::host::PluginFold> fold;
    FoldHolder(dftu_plugin* p, dstd::StringIntern& intern,
               plugins::host::SharedResultRegistry* reg,
               plugins::NamedResultRegistry* named)
        : plugin(p),
          fold(dstd::make_unique<plugins::host::PluginFold>(p, intern, reg,
                                                            named)) {}
    ~FoldHolder() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
};

dstd::Vector<trace::views::detail::FoldEvent> make_events(std::uint64_t count) {
    dstd::Vector<trace::views::detail::FoldEvent> evs(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        evs[i].ts = i;
        // step() drops METADATA/UNKNOWN-phase events before it ever reaches
        // on_batch; the default FoldEvent phase is UNKNOWN.
        evs[i].phase = dftracer::utils::trace::RecordPhase::Complete;
    }
    return evs;
}

void finalize_now(plugins::host::PluginFold& f) {
    Runtime rt(1);
    dftu_utils_test::ok(
        rt.submit(coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                      const trace::views::detail::CoverageSet covered{};
                      auto finalized = co_await f.finalize(covered);
                      CHECK(finalized.has_value());
                  }),
                  "fin")
            .wait());
    rt.shutdown();
}

}  // namespace

TEST_SUITE("PluginResults") {
    TEST_CASE("a plugin's emitted blob round-trips to the registry verbatim") {
        dstd::StringIntern intern;
        plugins::host::SharedResultRegistry reg;
        plugins::NamedResultRegistry named;
        FoldHolder master(
            dftracer::utils::plugins::make_plugin<CountBlobSlice>(nullptr),
            intern, &reg, &named);

        const std::uint64_t counts[3] = {5, 7, 4};
        std::uint64_t total = 0;
        for (std::uint64_t n : counts) {
            auto slice = master.fold->slice();
            auto* pf = static_cast<plugins::host::PluginFold*>(slice.get());
            auto evs = make_events(n);
            trace::views::detail::ScanUnit unit;
            trace::views::detail::FoldBatch fb{
                dstd::Span<const trace::views::detail::FoldEvent>(evs), unit};
            dftu_utils_test::ok(pf->step(fb));
            dftu_utils_test::ok(master.fold->merge(*pf));
            total += n;
        }
        finalize_now(*master.fold);

        auto it = named.results().find("counts");
        REQUIRE(it != named.results().end());
        REQUIRE(std::holds_alternative<dstd::Vector<std::byte>>(it->second));
        const auto& bytes = std::get<dstd::Vector<std::byte>>(it->second);
        dstd::String got(reinterpret_cast<const char*>(bytes.data()),
                         bytes.size());
        CHECK(got == "count=" + dstd::to_string(total));
    }

    TEST_CASE("an emitted lazyframe is owned and collects to the frame") {
        plugins::NamedResultRegistry named;

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
        REQUIRE(std::holds_alternative<plugins::OwnedLazyFrame>(it->second));

        dftu_lazyframe* got =
            std::get<plugins::OwnedLazyFrame>(it->second).get();
        REQUIRE(got != nullptr);
        dftu_dataframe* out = dftu_lazyframe_collect(got, 0);
        REQUIRE(out != nullptr);
        CHECK(dftu_dataframe_num_rows(out) == 4);
        dftu_dataframe_free(out);

        named.clear();  // frees the owned lazyframe handle (asan-clean).
    }

    TEST_CASE("an emit with an empty name is refused and keeps no result") {
        dstd::StringIntern intern;
        plugins::host::SharedResultRegistry reg;
        plugins::NamedResultRegistry named;
        FoldHolder master(
            dftracer::utils::plugins::make_plugin<CountBlobSlice>(nullptr),
            intern, &reg, &named);

        const char payload[] = "x";
        dftu_result_value v{};
        v.kind = DFTU_RESULT_KIND_BYTES;
        v.u.bytes.data = payload;
        v.u.bytes.len = 1;

        auto emitted = master.fold->emit_result(dstd::StringView(), &v);
        REQUIRE_FALSE(emitted.has_value());
        CHECK(emitted.error().message().find("name and value must be set") !=
              dstd::StringView::npos);
        CHECK(named.results().empty());
    }
}
