// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <stdexcept>

using namespace dftracer::utils;

namespace {

constexpr int MORSELS = 2;
constexpr std::int64_t ROWS = 3;

dataframe::Series i64(std::int64_t base) {
    std::int64_t v[ROWS] = {base, base + 1, base + 2};
    return dataframe::Series::flat(dataframe::TypeId::Int64, v, ROWS);
}

// Morsels named by name_ids: the declared a and b, then the undeclared `dyn`.
class DynCursor : public dataframe::Cursor {
   public:
    explicit DynCursor(dstd::Vector<dstd::String> dyn) : dyn_(std::move(dyn)) {}

    coro::CoroTask<dataframe::MorselResult> next(std::int64_t) override {
        if (next_ >= MORSELS) co_return dataframe::MorselResult(dstd::nullopt);
        dataframe::Morsel m;
        m.rows = ROWS;
        m.batch_index = next_;
        auto& d = m.dyn_state();
        d.intern = intern_;
        m.columns.push_back(i64(10 * next_));
        d.name_ids.push_back(intern_->get_or_insert("a"));
        m.columns.push_back(i64(100 + 10 * next_));
        d.name_ids.push_back(intern_->get_or_insert("b"));
        std::int64_t base = 1000;
        for (const dstd::String& n : dyn_) {
            m.columns.push_back(i64(base + 10 * next_));
            d.name_ids.push_back(intern_->get_or_insert(n));
            base += 1000;
        }
        ++next_;
        co_return dataframe::MorselResult(std::move(m));
    }

   private:
    dstd::Vector<dstd::String> dyn_;
    dstd::SharedPtr<dstd::StringIntern> intern_ =
        dstd::make_shared<dstd::StringIntern>();
    int next_ = 0;
};

class DynSource : public dataframe::Source {
   public:
    explicit DynSource(dstd::Vector<dstd::String> dyn) : dyn_(std::move(dyn)) {}

    dataframe::Schema schema() const override {
        dataframe::Schema s;
        s.fields.push_back(dataframe::Field{
            "a", dftracer::utils::dataframe::scalar(dataframe::TypeId::Int64),
            true});
        s.fields.push_back(dataframe::Field{
            "b", dftracer::utils::dataframe::scalar(dataframe::TypeId::Int64),
            true});
        return s;
    }

    bool undeclared_columns() const override { return true; }

    dftracer::utils::dataframe::ScanResult scan(
        const dftracer::utils::dataframe::ScanRequest& req) const override {
        dftracer::utils::dataframe::ScanResult r;
        r.cursor = dstd::make_unique<DynCursor>(dyn_);
        r.filters.assign(req.filters.size(),
                         dftracer::utils::dataframe::Pushed::No);
        return r;
    }

   private:
    dstd::Vector<dstd::String> dyn_;
};

dataframe::LazyFrame scan(dstd::Vector<dstd::String> dyn = {"x", "y"}) {
    return dataframe::LazyFrame::scan(
        dstd::make_shared<DynSource>(std::move(dyn)));
}

dstd::Result<dataframe::DataFrame> collect_result(
    const dataframe::LazyFrame& lf) {
    return dftracer::utils::default_runtime().submit(lf.collect()).get();
}

dataframe::DataFrame run(const dataframe::LazyFrame& lf) {
    return dftu_utils_test::ok(collect_result(lf));
}

using Names = dstd::Vector<dstd::String>;

std::int64_t cell(const dataframe::DataFrame& df, const dstd::String& name,
                  std::int64_t row) {
    for (std::size_t c = 0; c < df.names.size(); ++c)
        if (df.names[c] == name)
            return df.columns[c].materialize().values<std::int64_t>()[row];
    FAIL("no column " << name);
    return 0;
}

}  // namespace

TEST_CASE("drop removes static and dyn columns by name") {
    dataframe::DataFrame df = run(scan().drop({"b", "x"}));
    CHECK(df.names == Names{"a", "y"});
    CHECK(df.num_rows() == MORSELS * ROWS);
    CHECK(cell(df, "y", 0) == 2000);
    CHECK(cell(df, "y", ROWS) == 2010);
}

TEST_CASE("drop ignores a missing name") {
    dataframe::DataFrame df = run(scan().drop({"nope"}));
    CHECK(df.names == Names{"a", "b", "x", "y"});
}

TEST_CASE("drop shrinks the static schema") {
    CHECK(scan().drop({"a"}).schema() == Names{"b"});
}

TEST_CASE("rename_columns renames static and dyn columns by name") {
    dataframe::DataFrame df =
        run(scan().rename_columns({"b", "x"}, {"B", "X"}));
    CHECK(df.names == Names{"a", "B", "X", "y"});
    CHECK(cell(df, "B", 0) == 100);
    CHECK(cell(df, "X", 0) == 1000);
}

TEST_CASE("rename_columns ignores a missing name") {
    dataframe::DataFrame df = run(scan().rename_columns({"nope"}, {"z"}));
    CHECK(df.names == Names{"a", "b", "x", "y"});
}

TEST_CASE("rename_columns swaps two names at once") {
    dataframe::DataFrame df =
        run(scan().rename_columns({"a", "b"}, {"b", "a"}));
    CHECK(df.names == Names{"b", "a", "x", "y"});
    CHECK(cell(df, "b", 0) == 0);
}

TEST_CASE("rename_columns refuses a static collision when planned") {
    using dftracer::utils::common::ErrorCode;
    dftu_utils_test::expect_error(scan().rename_columns({"a"}, {"b"}).status(),
                                  ErrorCode::InvalidArgument);
    dftu_utils_test::expect_error(
        collect_result(scan().rename_columns({"a"}, {"b"})),
        ErrorCode::InvalidArgument);
}

TEST_CASE("rename_columns refuses a dyn collision per morsel") {
    CHECK_FALSE(
        collect_result(scan().rename_columns({"x"}, {"a"})).has_value());
    CHECK_FALSE(
        collect_result(scan().rename_columns({"x"}, {"y"})).has_value());
    CHECK_FALSE(
        collect_result(scan().rename_columns({"b"}, {"x"})).has_value());
}

TEST_CASE("rename_columns refuses lists of different lengths") {
    dftu_utils_test::expect_error(
        scan().rename_columns({"a", "b"}, {"c"}).status(),
        dftracer::utils::common::ErrorCode::InvalidArgument);
}

TEST_CASE("drop and rename_columns on an eager frame") {
    dataframe::DataFrame df;
    df.names = {"a", "b", "c"};
    df.columns.push_back(i64(0));
    df.columns.push_back(i64(10));
    df.columns.push_back(i64(20));
    auto lf = dataframe::LazyFrame::scan(
        dstd::make_shared<dftracer::utils::dataframe::InMemorySource>(
            std::move(df)));
    dataframe::DataFrame out =
        run(lf.drop({"b", "zz"}).rename_columns({"c"}, {"C"}));
    CHECK(out.names == Names{"a", "C"});
    CHECK(cell(out, "C", 1) == 21);
    CHECK_FALSE(collect_result(lf.rename_columns({"a"}, {"c"})).has_value());
}

TEST_CASE("C ABI drop and rename_columns") {
    dataframe::DataFrame df;
    df.names = {"a", "b"};
    df.columns.push_back(i64(0));
    df.columns.push_back(i64(10));
    dftu_lazyframe* base = nullptr;
    {
        dstd::Vector<const char*> names = {"a", "b"};
        dstd::Vector<dftu_series*> cols;
        cols.push_back(dftu_series_new_flat(
            DFTU_TYPE_INT64, df.columns[0].values<std::int64_t>().data(), ROWS,
            nullptr));
        cols.push_back(dftu_series_new_flat(
            DFTU_TYPE_INT64, df.columns[1].values<std::int64_t>().data(), ROWS,
            nullptr));
        dftu_dataframe* f = dftu_dataframe_new(names.data(), cols.data(), 2);
        REQUIRE(f != nullptr);
        base = dftu_dataframe_lazy(f);
        dftu_dataframe_free(f);
    }
    REQUIRE(base != nullptr);
    const char* drop_names[] = {"b"};
    dftu_lazyframe* d = dftu_lazyframe_drop(base, drop_names, 1);
    REQUIRE(d != nullptr);
    char* schema = dftu_lazyframe_schema(d);
    CHECK(dstd::String(schema).find('b') == dstd::String::npos);
    dftu_duql_string_free(schema);
    const char* from[] = {"a"};
    const char* to[] = {"z"};
    dftu_lazyframe* r = dftu_lazyframe_rename_columns(base, from, 1, to, 1);
    CHECK(r != nullptr);
    CHECK(dftu_lazyframe_rename_columns(base, from, 1, to, 0) == nullptr);
    CHECK(dftu_lazyframe_rename_columns(nullptr, from, 1, to, 1) == nullptr);
    CHECK(dftu_lazyframe_drop(base, nullptr, 1) == nullptr);
    CHECK(dftu_lazyframe_drop(base, drop_names, -1) == nullptr);
    dftu_lazyframe_free(r);
    dftu_lazyframe_free(d);
    dftu_lazyframe_free(base);
}
