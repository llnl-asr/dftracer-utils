// SPDX-License-Identifier: MIT
// A plan node and a provider registered through the C++ interfaces alone:
// nothing here names a plugin type. LazyFrame::op stacks the node on a plan,
// and dftu_lazyframe_from_provider builds a plan over the provider by name.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/lazyframe_handle.h>
#include <dftracer/utils/dataframe/internal/node_registry.h>
#include <dftracer/utils/dataframe/internal/provider_registry.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

DataFrame run(coro::CoroTask<dstd::Result<DataFrame>> t) {
    DataFrame out = dftu_utils_test::ok(
        dftracer::utils::default_runtime().submit(std::move(t)).get());
    for (Series& c : out.columns) c = c.materialize();
    return out;
}

DataFrame ids(std::int64_t n) {
    dstd::Vector<std::int64_t> v;
    for (std::int64_t i = 0; i < n; ++i) v.push_back(i);
    DataFrame df;
    df.names = dstd::make_shared_strings({"id"});
    df.columns.push_back(Series::flat_i64(v.data(), n));
    return df;
}

LazyFrame ids_plan(std::int64_t n) {
    return LazyFrame::scan(dstd::make_shared<InMemorySource>(ids(n)));
}

// Hands every morsel through and counts the ones it saw.
class CountingCursor final : public Cursor {
   public:
    CountingCursor(dstd::UniquePtr<Cursor> in, std::int64_t* morsels)
        : in_(std::move(in)), morsels_(morsels) {}

    coro::CoroTask<MorselResult> next(std::int64_t max_rows) override {
        auto reading = in_->next(max_rows);
        MorselResult read = co_await std::move(reading);
        if (read && *read) ++*morsels_;
        co_return std::move(read);
    }

   private:
    dstd::UniquePtr<Cursor> in_;
    std::int64_t* morsels_;
};

class PassThroughNode final : public NodeImpl {
   public:
    dstd::Result<Schema> output_schema(const Schema& in,
                                       const OpArgs&) const override {
        return in;
    }

    dstd::Result<dstd::UniquePtr<Cursor>> open(
        dstd::UniquePtr<Cursor> input,
        const dstd::Vector<dstd::String>& input_names, const OpArgs&,
        std::uint64_t memory_budget) const override {
        names_seen = input_names;
        budget_seen = memory_budget;
        return dstd::UniquePtr<Cursor>(
            dstd::make_unique<CountingCursor>(std::move(input), &morsels));
    }

    mutable dstd::Vector<dstd::String> names_seen;
    mutable std::uint64_t budget_seen = 0;
    mutable std::int64_t morsels = 0;
};

class RefusingNode final : public NodeImpl {
   public:
    dstd::Result<Schema> output_schema(const Schema& in,
                                       const OpArgs&) const override {
        return in;
    }

    dstd::Result<dstd::UniquePtr<Cursor>> open(
        dstd::UniquePtr<Cursor>, const dstd::Vector<dstd::String>&,
        const OpArgs&, std::uint64_t) const override {
        return dstd::Error::from_format<"the node will not open">(
            dstd::Condition::InvalidArgument);
    }
};

class FixedProvider final : public ProviderFactory {
   public:
    dstd::SharedPtr<Source> make_source() override {
        return dstd::make_shared<InMemorySource>(ids(4));
    }
};

}  // namespace

TEST_SUITE("node registry") {
    TEST_CASE("a node registered in C++ runs as a plan step") {
        auto node = dstd::make_shared<PassThroughNode>();
        REQUIRE(register_node("test.registry.pass", node));
        LazyFrame lf = ids_plan(5)
                           .op("test.registry.pass", OpArgs())
                           .memory_budget(1u << 20);
        CHECK(lf.explain().find("op test.registry.pass") != dstd::String::npos);
        DataFrame out = run(lf.collect());
        CHECK(out.num_rows() == 5);
        CHECK(node->morsels >= 1);
        CHECK(node->names_seen == dstd::Vector<dstd::String>{"id"});
        CHECK(node->budget_seen == (1u << 20));
        CHECK(unregister_node("test.registry.pass"));
    }

    TEST_CASE("a registered name is refused a second time") {
        REQUIRE(register_node("test.registry.twice",
                              dstd::make_shared<PassThroughNode>()));
        CHECK_FALSE(register_node("test.registry.twice",
                                  dstd::make_shared<PassThroughNode>()));
        CHECK(unregister_node("test.registry.twice"));
        CHECK_FALSE(unregister_node("test.registry.twice"));
        CHECK(register_node("test.registry.twice",
                            dstd::make_shared<PassThroughNode>()));
        CHECK(unregister_node("test.registry.twice"));
    }

    TEST_CASE("an unknown node name fails the plan at the op call") {
        LazyFrame lf = ids_plan(2).op("test.registry.absent", OpArgs());
        const auto status = lf.status();
        REQUIRE_FALSE(status);
        CHECK(
            dstd::String(status.error().c_str()).find("test.registry.absent") !=
            dstd::String::npos);
    }

    TEST_CASE("a plan keeps its node after the name is unregistered") {
        auto node = dstd::make_shared<PassThroughNode>();
        REQUIRE(register_node("test.registry.held", node));
        LazyFrame lf = ids_plan(3).op("test.registry.held", OpArgs());
        REQUIRE(unregister_node("test.registry.held"));
        CHECK(run(lf.collect()).num_rows() == 3);
        CHECK(node->morsels >= 1);
    }

    TEST_CASE("a node that fails to open ends the collect with its error") {
        REQUIRE(register_node("test.registry.refuse",
                              dstd::make_shared<RefusingNode>()));
        const auto collected =
            dftracer::utils::default_runtime()
                .submit(
                    ids_plan(2).op("test.registry.refuse", OpArgs()).collect())
                .get();
        dftu_utils_test::expect_error(
            collected, dftracer::utils::common::ErrorCode::InvalidArgument);
        CHECK(unregister_node("test.registry.refuse"));
    }

    TEST_CASE("dftu_node_unregister removes a C++ node by name") {
        REQUIRE(register_node("test.registry.c_unregister",
                              dstd::make_shared<PassThroughNode>()));
        CHECK(dftu_node_unregister("test.registry.c_unregister") == 0);
        CHECK(dftu_node_unregister("test.registry.c_unregister") != 0);
    }
}

TEST_SUITE("provider registry") {
    TEST_CASE(
        "a provider registered in C++ backs dftu_lazyframe_from_provider") {
        REQUIRE(register_provider("test.registry.rows",
                                  dstd::make_shared<FixedProvider>()));
        dftu_lazyframe* handle =
            dftu_lazyframe_from_provider("test.registry.rows");
        REQUIRE(handle != nullptr);
        CHECK(run(lazyframe_handle_unwrap(handle).collect()).num_rows() == 4);
        dftu_lazyframe_free(handle);
        CHECK(dftu_lazyframe_from_provider("test.registry.absent") == nullptr);
        CHECK(dftu_provider_unregister("test.registry.rows") == 0);
        CHECK(dftu_provider_unregister("test.registry.rows") != 0);
    }

    TEST_CASE("a provider name is refused a second time") {
        REQUIRE(register_provider("test.registry.dup",
                                  dstd::make_shared<FixedProvider>()));
        CHECK_FALSE(register_provider("test.registry.dup",
                                      dstd::make_shared<FixedProvider>()));
        CHECK(unregister_provider("test.registry.dup"));
    }
}
