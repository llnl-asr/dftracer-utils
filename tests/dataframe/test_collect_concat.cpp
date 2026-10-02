#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::coro::CoroTask;
using dftracer::utils::dataframe::ConcatHow;
using dftracer::utils::dataframe::ConcatPlan;
using dftracer::utils::dataframe::Cursor;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::Encoding;
using dftracer::utils::dataframe::Field;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::Morsel;
using dftracer::utils::dataframe::Schema;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::Source;
using dftracer::utils::dataframe::TypeId;

namespace {

constexpr int PARTS = 60;

std::vector<DataFrame> shared(const std::vector<DataFrame>& parts) {
    std::vector<DataFrame> out;
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
    PartsCursor(std::shared_ptr<const std::vector<DataFrame>> parts,
                std::size_t leading)
        : parts_(std::move(parts)), leading_(leading) {}

    CoroTask<std::optional<Morsel>> next(std::int64_t) override {
        if (next_ >= parts_->size()) co_return std::nullopt;
        const DataFrame& df = (*parts_)[next_++];
        Morsel m;
        m.rows = df.num_rows();
        for (std::size_t c = 0; c < df.columns.size(); ++c) {
            if (c < leading_) {
                m.columns.push_back(df.columns[c].share());
            } else {
                auto& dyn = m.dyn_state();
                dyn.dyn_names.push_back(df.names[c]);
                dyn.dyn_columns.push_back(df.columns[c].share());
            }
        }
        co_return m;
    }

   private:
    std::shared_ptr<const std::vector<DataFrame>> parts_;
    std::size_t leading_;
    std::size_t next_ = 0;
};

class PartsSource : public Source {
   public:
    PartsSource(const std::vector<DataFrame>& parts, std::size_t leading)
        : parts_(std::make_shared<const std::vector<DataFrame>>(shared(parts))),
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

    dftracer::utils::dataframe::ScanResult scan(
        const dftracer::utils::dataframe::ScanRequest& req) const override {
        dftracer::utils::dataframe::ScanResult r;
        r.cursor = std::make_unique<PartsCursor>(parts_, leading_);
        r.filters.assign(req.filters.size(),
                         dftracer::utils::dataframe::Pushed::No);
        return r;
    }

   private:
    std::shared_ptr<const std::vector<DataFrame>> parts_;
    std::size_t leading_;
};

DataFrame collect(const std::vector<DataFrame>& parts, std::size_t leading) {
    auto source = std::make_shared<PartsSource>(parts, leading);
    return dftracer::utils::default_runtime()
        .submit(LazyFrame::scan(source).collect())
        .get();
}

DataFrame serial(const std::vector<DataFrame>& parts, ConcatHow how) {
    std::vector<const DataFrame*> ptrs;
    for (const DataFrame& p : parts) ptrs.push_back(&p);
    return dftracer::utils::dataframe::concat(ptrs, how);
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
    std::vector<std::int64_t> v(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = base + i;
    return Series::flat_i64(v.data(), n);
}

Series doubles(std::int64_t base, std::int64_t n) {
    std::vector<double> v(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = static_cast<double>(base + i) / 4;
    return Series::flat_f64(v.data(), n);
}

Series words(std::int64_t base, std::int64_t n) {
    std::vector<std::string> v;
    for (std::int64_t i = 0; i < n; ++i)
        v.push_back("w" + std::to_string(base + i));
    return Series::strings(v);
}

std::vector<DataFrame> equal_parts() {
    std::vector<DataFrame> parts;
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

std::vector<DataFrame> ragged_parts() {
    std::vector<DataFrame> parts;
    std::int64_t base = 0;
    for (int k = 0; k < PARTS; ++k) {
        const std::int64_t n = 4 + k % 5;
        DataFrame df;
        df.names = {"i"};
        df.columns.push_back(ints(base, n));
        df.names.push_back("x" + std::to_string(k % 3));
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
        std::vector<Series> field;
        field.push_back(k == 7 ? words(0, parts[k].num_rows())
                               : ints(0, parts[k].num_rows()));
        parts[k].names.push_back("st");
        parts[k].columns.push_back(Series::structs({"a"}, std::move(field)));
    }

    std::string serial_error;
    try {
        serial(parts, ConcatHow::Diagonal);
    } catch (const std::exception& e) {
        serial_error = e.what();
    }
    REQUIRE_FALSE(serial_error.empty());
    try {
        collect(parts, 1);
        FAIL("the collect must throw");
    } catch (const std::exception& e) {
        CHECK(std::string(e.what()) == serial_error);
    }
}

TEST_CASE("ConcatPlan columns equal the serial concat, column by column") {
    for (const ConcatHow how : {ConcatHow::Vertical, ConcatHow::Diagonal}) {
        const auto parts =
            how == ConcatHow::Vertical ? equal_parts() : ragged_parts();
        std::vector<const DataFrame*> ptrs;
        for (const DataFrame& p : parts) ptrs.push_back(&p);
        const ConcatPlan plan(ptrs, how);
        DataFrame got;
        got.names = plan.names();
        for (std::size_t c = 0; c < got.names.size(); ++c)
            got.columns.push_back(plan.column(c));
        check_equal(got, serial(parts, how));
    }
}

TEST_CASE("ConcatPlan refuses vertical parts with different schemas") {
    const auto parts = ragged_parts();
    std::vector<const DataFrame*> ptrs;
    for (const DataFrame& p : parts) ptrs.push_back(&p);
    CHECK_THROWS_AS(ConcatPlan(ptrs, ConcatHow::Vertical),
                    std::invalid_argument);
}
