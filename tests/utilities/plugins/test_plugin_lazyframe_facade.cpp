// The OwnedLazyFrame fluent facade: every method reaches the host through
// DFTU_SVC_OPS::run_lazy (dftu.lazy.* in the op registry), never by linking
// dftu_lazyframe_* directly, so this exercises the run_lazy dispatch path
// end to end through a real fold_adapter host.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/fold_adapter.h>

#include <memory>
// After fold_adapter.h so nanoarrow is set up before dataframe/abi.h's
// arrow_abi.
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/result_registry.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using dftracer::utils::StringIntern;
using dftracer::utils::plugins::OwnedLazyFrame;
using dftracer::utils::plugins::PluginFold;

namespace {

struct TrivialSlice {
    explicit TrivialSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftu_dataframe*, dftracer::utils::plugins::Host) {}
    void merge(TrivialSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

struct HostFixture {
    StringIntern intern;
    dftu_plugin* plugin =
        dftracer::utils::plugins::make_plugin<TrivialSlice>(nullptr);
    std::unique_ptr<PluginFold> fold =
        std::make_unique<PluginFold>(plugin, intern);
    dftu_plugin_host& host() { return fold->host(); }
    ~HostFixture() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
};

/// Builds a 5-row (id, dur) frame and wraps it as an OwnedLazyFrame wired to
/// the fixture's real op registry, mirroring how a plugin would construct
/// the starting point of a plan (dftu_dataframe_lazy is a direct, unregistred
/// ABI call - only the plan-building steps ride the registry).
OwnedLazyFrame make_lazy(HostFixture& fx, dftu_dataframe** keep_source) {
    const auto* ops = static_cast<const dftu_svc_ops*>(
        fx.host().get_service(fx.host().h, DFTU_SVC_OPS));
    REQUIRE(ops);
    REQUIRE(ops->run_lazy);

    const std::int64_t ids[] = {1, 2, 3, 4, 5};
    const std::int64_t durs[] = {50, 10, 40, 20, 30};
    dftu_series* id_col =
        dftu_series_new_flat(DFTU_TYPE_INT64, ids, 5, nullptr);
    dftu_series* dur_col =
        dftu_series_new_flat(DFTU_TYPE_INT64, durs, 5, nullptr);
    REQUIRE(id_col);
    REQUIRE(dur_col);
    const char* names[2] = {"id", "dur"};
    dftu_series* cols[2] = {id_col, dur_col};
    dftu_dataframe* df = dftu_dataframe_new(names, cols, 2);
    REQUIRE(df);
    if (keep_source) *keep_source = df;

    dftu_lazyframe* lf = dftu_dataframe_lazy(df);
    REQUIRE(lf);
    return OwnedLazyFrame{lf, ops, fx.host().h};
}

}  // namespace

TEST_CASE("lazyframe facade: chained plan matches the equivalent direct plan") {
    HostFixture fx;
    dftu_dataframe* source = nullptr;
    OwnedLazyFrame lf = make_lazy(fx, &source);

    dftu_expr* col1 = dftu_expr_col(1);
    dftu_expr* gt15 =
        dftu_expr_cmp(DFTU_CMP_GT, col1, dftracer::utils::dataframe::i64(15));
    dftu_expr_free(col1);  // a child is shared, the handle is the caller's
    REQUIRE(gt15);

    OwnedLazyFrame out = lf.filter(gt15).sort_by("dur", true).head(3);
    dftu_expr_free(gt15);

    REQUIRE(out.handle);
    dftu_dataframe* collected = dftu_lazyframe_collect(out.handle, -1);
    REQUIRE(collected);
    REQUIRE(dftu_dataframe_num_rows(collected) == 3);

    dftu_lazyframe* direct = dftu_dataframe_lazy(source);
    REQUIRE(direct);
    dftu_expr* col1_direct = dftu_expr_col(1);
    dftu_expr* gt15_direct = dftu_expr_cmp(DFTU_CMP_GT, col1_direct,
                                           dftracer::utils::dataframe::i64(15));
    dftu_expr_free(col1_direct);
    dftu_lazyframe* filtered = dftu_lazyframe_filter(direct, gt15_direct);
    dftu_expr_free(gt15_direct);
    dftu_lazyframe_free(direct);
    REQUIRE(filtered);
    dftu_lazyframe* sorted = dftu_lazyframe_sort_by(filtered, "dur", 1);
    dftu_lazyframe_free(filtered);
    REQUIRE(sorted);
    dftu_lazyframe* limited = dftu_lazyframe_head(sorted, 3);
    dftu_lazyframe_free(sorted);
    REQUIRE(limited);
    dftu_dataframe* expected = dftu_lazyframe_collect(limited, -1);
    dftu_lazyframe_free(limited);
    REQUIRE(expected);

    REQUIRE(dftu_dataframe_num_rows(expected) ==
            dftu_dataframe_num_rows(collected));
    // A column handle is owned by the caller (dftu_series_free).
    dftu_series* got_col = dftu_dataframe_column(collected, "dur");
    dftu_series* want_col = dftu_dataframe_column(expected, "dur");
    const auto* got_durs =
        static_cast<const std::int64_t*>(dftu_series_data(got_col));
    const auto* want_durs =
        static_cast<const std::int64_t*>(dftu_series_data(want_col));
    REQUIRE(got_durs);
    REQUIRE(want_durs);
    for (std::int64_t i = 0; i < dftu_dataframe_num_rows(collected); ++i)
        CHECK(got_durs[i] == want_durs[i]);
    dftu_series_free(got_col);
    dftu_series_free(want_col);

    dftu_dataframe_free(collected);
    dftu_dataframe_free(expected);
    dftu_dataframe_free(source);
}

TEST_CASE(
    "lazyframe facade: a rejected op yields an empty handle that later "
    "calls short-circuit on") {
    HostFixture fx;
    dftu_dataframe* source = nullptr;
    OwnedLazyFrame lf = make_lazy(fx, &source);

    // filter_mask(nullptr) is an arity/shape mismatch the run_lazy dispatch
    // rejects outright (see dftu_op_run_lazy's SERIES-operand null check), so
    // this fails mid-chain rather than only surfacing at collect.
    OwnedLazyFrame out = lf.filter_mask(nullptr).head(1).sort_by("dur", false);
    CHECK_FALSE(out.handle);
    // The chain borrowed lf, so a failure inside it leaves the source intact
    // and still usable rather than emptying it as a side effect.
    CHECK(lf.handle);
    CHECK(lf.head(1).handle);

    // Calling a method again on an already-empty OwnedLazyFrame must not
    // crash or double-free; it stays empty.
    OwnedLazyFrame still_empty = out.head(5);
    CHECK_FALSE(still_empty.handle);

    // With no ops extension wired up at all, every op rejects the same way.
    OwnedLazyFrame unwired{dftu_dataframe_lazy(source)};
    OwnedLazyFrame unwired_out = unwired.head(1);
    CHECK_FALSE(unwired_out.handle);

    dftu_dataframe_free(source);
}

// The layers beneath borrow: dftu_lazyframe_* takes a const handle and
// LazyFrame's own methods are const. A facade that consumed instead would make
// the second call here hand back an empty plan, with no error at the call site.
TEST_CASE(
    "lazyframe facade: a plan is reusable for more than one derived plan") {
    HostFixture fx;
    dftu_dataframe* source = nullptr;
    OwnedLazyFrame lf = make_lazy(fx, &source);

    OwnedLazyFrame first = lf.head(2);
    OwnedLazyFrame second = lf.head(3);

    REQUIRE(lf.handle);  // still usable after both
    REQUIRE(first.handle);
    REQUIRE(second.handle);

    dftu_dataframe* a = dftu_lazyframe_collect(first.handle, -1);
    dftu_dataframe* b = dftu_lazyframe_collect(second.handle, -1);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(dftu_dataframe_num_rows(a) == 2);
    CHECK(dftu_dataframe_num_rows(b) == 3);
    dftu_dataframe_free(a);
    dftu_dataframe_free(b);
    dftu_dataframe_free(source);
}

TEST_CASE("lazyframe facade: take and group_by_dynamic reach the host") {
    HostFixture fx;
    dftu_dataframe* source = nullptr;
    OwnedLazyFrame lf = make_lazy(fx, &source);

    const std::int64_t idx[2] = {4, 0};
    OwnedLazyFrame taken = lf.take(idx);
    REQUIRE(taken.handle);
    dftu_dataframe* taken_df = dftu_lazyframe_collect(taken.handle, -1);
    REQUIRE(taken_df);
    REQUIRE(dftu_dataframe_num_rows(taken_df) == 2);
    dftu_series* id_col = dftu_dataframe_column(taken_df, "id");
    const auto* taken_ids =
        static_cast<const std::int64_t*>(dftu_series_data(id_col));
    REQUIRE(taken_ids);
    CHECK(taken_ids[0] == 5);
    CHECK(taken_ids[1] == 1);
    dftu_series_free(id_col);
    dftu_dataframe_free(taken_df);

    dftu_group_agg aggs[1] = {{"sum", "dur", "dur_sum"}};
    OwnedLazyFrame windowed =
        lf.group_by_dynamic("id", 2, 2, {aggs, 1}, 1, false);
    REQUIRE(windowed.handle);
    dftu_dataframe* windowed_df = dftu_lazyframe_collect(windowed.handle, -1);
    REQUIRE(windowed_df);
    CHECK(dftu_dataframe_num_rows(windowed_df) == 3);
    dftu_dataframe_free(windowed_df);

    dftu_dataframe_free(source);
}
