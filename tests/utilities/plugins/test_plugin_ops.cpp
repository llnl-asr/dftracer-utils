// DFTU_SVC_OPS: exposes the dataframe engine's op registry to a plugin by
// name, so it can run any registered column/aggregate/frame op on a
// Series/DataFrame it already holds without linking the dataframe C ABI.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/fold_adapter.h>

#include <memory>
// After fold_adapter.h so nanoarrow is set up before dataframe/abi.h's
// arrow_abi.
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/plugin.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using dftracer::utils::StringIntern;
using dftracer::utils::plugins::Host;
using dftracer::utils::plugins::PluginFold;

namespace {

struct TrivialSlice {
    explicit TrivialSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftu_dataframe*, Host) {}
    void merge(TrivialSlice&) {}
    void finalize(Host) {}
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

}  // namespace

TEST_CASE("plugin ops: run_op runs a registered column op by name") {
    HostFixture fx;
    Host h{&fx.host()};

    const std::int64_t a_vals[] = {1, 2, 3};
    const std::int64_t b_vals[] = {10, 20, 30};
    dftu_series* a = dftu_series_new_flat(DFTU_TYPE_INT64, a_vals, 3, nullptr);
    dftu_series* b = dftu_series_new_flat(DFTU_TYPE_INT64, b_vals, 3, nullptr);
    REQUIRE(a);
    REQUIRE(b);

    dftu_series* sum = h.run_op("dftu.series.add", {a, b});
    REQUIRE(sum);
    REQUIRE(dftu_series_length(sum) == 3);
    const auto* sum_data =
        static_cast<const std::int64_t*>(dftu_series_data(sum));
    REQUIRE(sum_data);
    for (std::int64_t i = 0; i < 3; ++i)
        CHECK(sum_data[i] == a_vals[i] + b_vals[i]);

    dftu_series_free(sum);
    dftu_series_free(a);
    dftu_series_free(b);
}

TEST_CASE("plugin ops: run_op reports unknown names and bad arity") {
    HostFixture fx;
    Host h{&fx.host()};

    CHECK(h.run_op("dftu.test.nonexistent_op", {}) == nullptr);

    const std::int64_t a_vals[] = {1, 2, 3};
    dftu_series* a = dftu_series_new_flat(DFTU_TYPE_INT64, a_vals, 3, nullptr);
    REQUIRE(a);
    // "dftu.series.add" takes two series operands; one is an arity mismatch.
    CHECK(h.run_op("dftu.series.add", {a}) == nullptr);
    dftu_series_free(a);
}

TEST_CASE("plugin ops: unknown op and a failed op are distinguishable") {
    HostFixture fx;
    const auto* ops = static_cast<const dftu_svc_ops*>(
        fx.host().get_service(fx.host().h, DFTU_SVC_OPS));
    REQUIRE(ops);
    REQUIRE(ops->run);

    dftu_result_series unknown =
        ops->run(fx.host().h, "dftu.test.nonexistent_op", nullptr, 0, nullptr);
    CHECK_FALSE(DFTU_RESULT_OK(unknown));
    CHECK(DFTU_RESULT_ERROR(unknown).condition == DFTU_COND_NOT_FOUND);

    const std::int64_t a_vals[] = {1, 2, 3};
    dftu_series* a = dftu_series_new_flat(DFTU_TYPE_INT64, a_vals, 3, nullptr);
    REQUIRE(a);
    const dftu_series* in[1] = {a};
    // "dftu.series.add" is a real op, but this call gives it the wrong arity.
    dftu_result_series failed =
        ops->run(fx.host().h, "dftu.series.add", in, 1, nullptr);
    CHECK_FALSE(DFTU_RESULT_OK(failed));
    CHECK(DFTU_RESULT_ERROR(failed).condition == DFTU_COND_INVALID_ARGUMENT);
    CHECK(DFTU_RESULT_ERROR(failed).condition !=
          DFTU_RESULT_ERROR(unknown).condition);
    dftu_series_free(a);
}

TEST_CASE("plugin ops: find_op resolves a built-in by name") {
    HostFixture fx;
    Host h{&fx.host()};

    const dftu_op_desc* desc = h.find_op("dftu.series.add");
    REQUIRE(desc);
    CHECK(dftu_op_kind_of(desc->sig) == DFTU_OP_KIND_SERIES);
    CHECK(h.find_op("dftu.test.nonexistent_op") == nullptr);
}

TEST_CASE("plugin ops: a dftu. op name is refused") {
    HostFixture fx;
    const auto* ops = static_cast<const dftu_svc_ops*>(
        fx.host().get_service(fx.host().h, DFTU_SVC_OPS));
    REQUIRE(ops);
    REQUIRE(ops->register_op);

    // Shadowing a host op would silently change what every other plugin and
    // the planner resolve, so the name is refused before registration and the
    // host's own op keeps the name.
    const dftu_op_desc* host_op = ops->find(fx.host().h, "dftu.hash.fnv1a");
    REQUIRE(host_op);
    dftu_op_desc shadow{};
    shadow.name = "dftu.hash.fnv1a";
    CHECK(ops->register_op(fx.host().h, &shadow) != 0);
    CHECK(ops->find(fx.host().h, "dftu.hash.fnv1a") == host_op);

    dftu_op_desc fresh{};
    fresh.name = "dftu.test.plugin_shadow";
    CHECK(ops->register_op(fx.host().h, &fresh) != 0);
    CHECK(ops->find(fx.host().h, "dftu.test.plugin_shadow") == nullptr);
}

TEST_CASE("plugin ops: a bare op name is refused") {
    HostFixture fx;
    const auto* ops = static_cast<const dftu_svc_ops*>(
        fx.host().get_service(fx.host().h, DFTU_SVC_OPS));
    REQUIRE(ops);

    // The bare namespace has no dot, so it fails the <plugin>.<name>
    // qualification a plugin op must have (reserved regardless of whether the
    // host currently populates it).
    dftu_op_desc bare{};
    bare.name = "my_op";
    CHECK(ops->register_op(fx.host().h, &bare) != 0);
    CHECK(ops->find(fx.host().h, "my_op") == nullptr);
}

TEST_CASE("plugin agg: a dftu. accumulator name is refused") {
    HostFixture fx;
    const auto* agg = static_cast<const dftu_svc_agg*>(
        fx.host().get_service(fx.host().h, DFTU_SVC_AGG));
    REQUIRE(agg);

    const dftu_agg_col specs[1] = {
        {DFTU_AGG_COUNT, nullptr, "count", 0.0, nullptr}};
    CHECK(agg->agg_new(fx.host().h, "dftu.scan.count", nullptr, 0, specs, 1) ==
          nullptr);
    CHECK(agg->agg_new(fx.host().h, "com.example.count", nullptr, 0, specs,
                       1) != nullptr);
}
