// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

constexpr int PARTS = 60;

dstd::Vector<DataFrame> shared(const dstd::Vector<DataFrame>& parts) {
    dstd::Vector<DataFrame> out;
    for (const DataFrame& p : parts) {
        DataFrame df;
        df.names = p.names;
        for (const Series& c : p.columns) df.columns.push_back(c.share());
        out.push_back(std::move(df));
    }
    return out;
}

class PartsCursor : public Cursor {
   public:
    explicit PartsCursor(dstd::SharedPtr<const dstd::Vector<DataFrame>> parts)
        : parts_(std::move(parts)) {}

    coro::CoroTask<MorselResult> next(std::int64_t) override {
        if (next_ >= parts_->size()) co_return MorselResult(dstd::nullopt);
        const DataFrame& df = (*parts_)[next_++];
        Morsel m;
        m.rows = df.num_rows();
        auto& dyn = m.dyn_state();
        dyn.intern = intern_;
        for (std::size_t c = 0; c < df.columns.size(); ++c) {
            m.columns.push_back(df.columns[c].share());
            dyn.name_ids.push_back(intern_->get_or_insert(df.names[c]));
        }
        co_return MorselResult(std::move(m));
    }

   private:
    dstd::SharedPtr<const dstd::Vector<DataFrame>> parts_;
    dstd::SharedPtr<dstd::StringIntern> intern_ =
        dstd::make_shared<dstd::StringIntern>();
    std::size_t next_ = 0;
};

class PartsSource : public Source {
   public:
    PartsSource(const dstd::Vector<DataFrame>& parts, std::size_t leading)
        : parts_(
              dstd::make_shared<const dstd::Vector<DataFrame>>(shared(parts))),
          leading_(leading) {}

    Schema schema() const override {
        Schema s;
        const DataFrame& first = parts_->front();
        for (std::size_t c = 0; c < leading_; ++c)
            s.fields.push_back(Field{
                first.names[c],
                dftracer::utils::dataframe::scalar(first.columns[c].type()),
                true});
        return s;
    }

    dstd::Result<dftracer::utils::dataframe::ScanResult> scan(
        const dftracer::utils::dataframe::ScanRequest& req) const override {
        dftracer::utils::dataframe::ScanResult r;
        r.cursor = dstd::make_unique<PartsCursor>(parts_);
        r.filters.assign(req.filters.size(),
                         dftracer::utils::dataframe::Pushed::No);
        return r;
    }

   private:
    dstd::SharedPtr<const dstd::Vector<DataFrame>> parts_;
    std::size_t leading_;
};

dstd::Result<DataFrame> collect_result(const dstd::Vector<DataFrame>& parts,
                                       std::size_t leading) {
    auto source = dstd::make_shared<PartsSource>(parts, leading);
    return dftracer::utils::default_runtime()
        .submit(LazyFrame::scan(source).collect())
        .get();
}

DataFrame collect(const dstd::Vector<DataFrame>& parts, std::size_t leading) {
    return dftu_utils_test::ok(collect_result(parts, leading));
}

DataFrame serial(const dstd::Vector<DataFrame>& parts, ConcatHow how) {
    dstd::Vector<const DataFrame*> ptrs;
    for (const DataFrame& p : parts) ptrs.push_back(&p);
    return dftu_utils_test::ok(dftracer::utils::dataframe::concat(ptrs, how));
}

void check_equal(const DataFrame& got, const DataFrame& want) {
    REQUIRE(got.names == want.names);
    REQUIRE(got.num_rows() == want.num_rows());
    for (std::size_t c = 0; c < want.columns.size(); ++c) {
        CAPTURE(want.names[c]);
        const bool chunked = got.columns[c].encoding() == Encoding::Chunked;
        const Series g =
            chunked ? got.columns[c].materialize() : got.columns[c].share();
        const Series& w = want.columns[c];
        REQUIRE(g.type() == w.type());
        if (!chunked) CHECK(g.encoding() == w.encoding());
        CHECK(g.is_json() == w.is_json());
        for (std::int64_t i = 0; i < w.length(); ++i) {
            REQUIRE(g.is_null(i) == w.is_null(i));
            if (w.is_null(i)) continue;
            if (w.type() == TypeId::Int64)
                CHECK(g.values<std::int64_t>()[i] ==
                      w.values<std::int64_t>()[i]);
            else if (w.type() == TypeId::Float64)
                CHECK(g.values<double>()[i] == w.values<double>()[i]);
            else
                CHECK(g.string_at(i) == w.string_at(i));
        }
    }
}

Series ints(std::int64_t base, std::int64_t n) {
    dstd::Vector<std::int64_t> v(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = base + i;
    return Series::flat_i64(v.data(), n);
}

Series doubles(std::int64_t base, std::int64_t n) {
    dstd::Vector<double> v(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = static_cast<double>(base + i) / 4;
    return Series::flat_f64(v.data(), n);
}

Series words(std::int64_t base, std::int64_t n) {
    dstd::Vector<dstd::String> v;
    for (std::int64_t i = 0; i < n; ++i)
        v.push_back(dstd::fmt<"w{}">(base + i));
    return Series::strings(v);
}

dstd::Vector<DataFrame> equal_parts() {
    dstd::Vector<DataFrame> parts;
    std::int64_t base = 0;
    for (int k = 0; k < PARTS; ++k) {
        const std::int64_t n = 5 + k % 7;
        DataFrame df;
        df.names = {"i", "f", "s"};
        df.columns.push_back(ints(base, n));
        df.columns.push_back(doubles(base, n));
        df.columns.push_back(words(base, n));
        parts.push_back(std::move(df));
        base += n;
    }
    return parts;
}

dstd::Vector<DataFrame> ragged_parts() {
    dstd::Vector<DataFrame> parts;
    std::int64_t base = 0;
    for (int k = 0; k < PARTS; ++k) {
        const std::int64_t n = 4 + k % 5;
        DataFrame df;
        df.names = {"i"};
        df.columns.push_back(ints(base, n));
        df.names.push_back(dstd::fmt<"x{}">(k % 3));
        df.columns.push_back(ints(base * 2, n));
        df.names.push_back("v");
        df.columns.push_back(k % 2 ? doubles(base, n) : ints(base, n));
        if (k % 4 == 0) {
            df.names.push_back("s");
            df.columns.push_back(words(base, n));
        }
        parts.push_back(std::move(df));
        base += n;
    }
    return parts;
}

}  // namespace

TEST_CASE("a collect of many equal-schema parts equals the serial concat") {
    const auto parts = equal_parts();
    check_equal(collect(parts, 3), serial(parts, ConcatHow::Vertical));
}

TEST_CASE("a collect of parts with different columns equals the diagonal") {
    const auto parts = ragged_parts();
    const DataFrame want = serial(parts, ConcatHow::Diagonal);
    REQUIRE(want.names.size() == 6);
    const DataFrame got = collect(parts, 1);
    check_equal(got, want);
    std::int64_t nulls = 0;
    for (std::int64_t i = 0; i < got.num_rows(); ++i)
        nulls += got.columns[3].is_null(i);
    CHECK(nulls > 0);
}

TEST_CASE("an error in one column fails the collect") {
    auto parts = ragged_parts();
    for (std::size_t k = 0; k < parts.size(); ++k) {
        dstd::Vector<Series> field;
        dstd::Vector<Series> nested;
        nested.push_back(ints(0, parts[k].num_rows()));
        field.push_back(k == 7 ? Series::structs({"b"}, std::move(nested))
                               : ints(0, parts[k].num_rows()));
        parts[k].names.push_back("st");
        parts[k].columns.push_back(Series::structs({"a"}, std::move(field)));
    }

    dstd::Vector<const DataFrame*> ptrs;
    for (const DataFrame& p : parts) ptrs.push_back(&p);
    const auto serial_result =
        dftracer::utils::dataframe::concat(ptrs, ConcatHow::Diagonal);
    REQUIRE(!serial_result.has_value());
    const dstd::String serial_error = serial_result.error().c_str();
    REQUIRE_FALSE(serial_error.empty());
    const auto collected = collect_result(parts, 1);
    REQUIRE_FALSE(collected.has_value());
    CHECK(dstd::String(collected.error().c_str()) == serial_error);
}

TEST_CASE("ConcatPlan columns equal the serial concat, column by column") {
    for (const ConcatHow how : {ConcatHow::Vertical, ConcatHow::Diagonal}) {
        const auto parts =
            how == ConcatHow::Vertical ? equal_parts() : ragged_parts();
        dstd::Vector<const DataFrame*> ptrs;
        for (const DataFrame& p : parts) ptrs.push_back(&p);
        const ConcatPlan plan =
            dftu_utils_test::ok(ConcatPlan::make(ptrs, how));
        DataFrame got;
        got.names = plan.names();
        for (std::size_t c = 0; c < got.names.size(); ++c)
            got.columns.push_back(dftu_utils_test::ok(plan.column(c)));
        check_equal(got, serial(parts, how));
    }
}

TEST_CASE("ConcatPlan refuses vertical parts with different schemas") {
    const auto parts = ragged_parts();
    dstd::Vector<const DataFrame*> ptrs;
    for (const DataFrame& p : parts) ptrs.push_back(&p);
    const auto plan = ConcatPlan::make(ptrs, ConcatHow::Vertical);
    REQUIRE(!plan.has_value());
    CHECK(common::errorcode_of(plan.error()) ==
          common::ErrorCode::InvalidArgument);
}
