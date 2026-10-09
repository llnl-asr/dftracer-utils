// A scan whose morsels carry columns the plan schema does not declare: every
// op that keeps rows keeps them, equal to a plan that declares them.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace df = dftracer::utils::dataframe;
using df::col;
using df::DataFrame;
using df::LazyFrame;
using df::Series;
using df::TypeId;

namespace {

using Names = std::vector<std::string>;
using Cells = std::vector<std::optional<std::int64_t>>;
constexpr std::int64_t NUL = -999;

Series ints(const std::vector<std::int64_t>& v) {
    std::vector<std::int64_t> data(v);
    std::vector<std::uint8_t> valid((v.size() + 7) / 8, 0);
    bool nulls = false;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == NUL) {
            nulls = true;
            data[i] = 0;
        } else {
            valid[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
        }
    }
    return Series::flat_i64(data.data(), static_cast<std::int64_t>(v.size()),
                            nulls ? valid.data() : nullptr);
}

// One morsel: its columns by name, a list column `l` among them when asked.
struct Part {
    std::vector<std::pair<std::string, std::vector<std::int64_t>>> cols;
};

// m0: a, b, x; m1: a, b, x, y; m2: a, b. (1,20,101) and (1,20,103) differ
// only in x; (4,30,null x) and m2's (4,30) are one row; m2 repeats (6,80).
std::vector<Part> parts() {
    return {
        {{{"a", {3, 1, 4, 1}},
          {"b", {10, 20, 30, 20}},
          {"x", {100, 101, NUL, 103}}}},
        {{{"a", {5, 9, 2}},
          {"b", {NUL, 60, 70}},
          {"x", {200, 201, 202}},
          {"y", {1000, 1001, 1002}}}},
        {{{"a", {6, 4, 6}}, {"b", {80, 30, 80}}}},
    };
}

std::vector<Part> plain_parts() {
    return {{{{"a", {2, 1}}, {"b", {5, 6}}}}, {{{"a", {3}}, {"b", {7}}}}};
}

Series list_column(std::int64_t rows) {
    std::vector<std::int32_t> offs{0};
    std::vector<std::int64_t> values;
    for (std::int64_t i = 0; i < rows; ++i) {
        for (std::int64_t k = 0; k <= i % 3; ++k) values.push_back(i * 10 + k);
        offs.push_back(static_cast<std::int32_t>(values.size()));
    }
    return Series::list(offs, ints(values));
}

class PartsCursor : public df::Cursor {
   public:
    PartsCursor(std::vector<Part> parts, bool with_list)
        : parts_(std::move(parts)), with_list_(with_list) {}

    dftracer::utils::coro::CoroTask<std::optional<df::Morsel>> next(
        std::int64_t) override {
        if (next_ >= parts_.size()) co_return std::nullopt;
        const Part& p = parts_[next_];
        df::Morsel m;
        m.batch_index = static_cast<std::int64_t>(next_++);
        m.rows = static_cast<std::int64_t>(p.cols.front().second.size());
        auto& d = m.dyn_state();
        d.intern = intern_;
        for (const auto& [name, values] : p.cols) {
            d.name_ids.push_back(intern_->get_or_insert(name));
            m.columns.push_back(ints(values));
        }
        if (with_list_) {
            d.name_ids.push_back(intern_->get_or_insert("l"));
            m.columns.push_back(list_column(m.rows));
        }
        co_return m;
    }

   private:
    std::vector<Part> parts_;
    bool with_list_;
    std::shared_ptr<dftracer::utils::StringIntern> intern_ =
        std::make_shared<dftracer::utils::StringIntern>();
    std::size_t next_ = 0;
};

// Declares a and b (and l) only; with `declare_all` it declares x and y too
// and returns nothing undeclared.
class PartsSource : public df::Source {
   public:
    PartsSource(std::vector<Part> parts, bool declare_all, bool with_list)
        : parts_(std::move(parts)),
          declare_all_(declare_all),
          with_list_(with_list) {}

    df::Schema schema() const override {
        df::Schema s;
        Names names{"a", "b"};
        if (declare_all_) names.insert(names.end(), {"x", "y"});
        for (const std::string& n : names)
            s.fields.push_back(df::Field{n, df::scalar(TypeId::Int64), true});
        if (with_list_)
            s.fields.push_back(
                df::Field{"l", df::list_of(df::scalar(TypeId::Int64)), true});
        return s;
    }

    bool undeclared_columns() const override { return !declare_all_; }

    df::ScanResult scan(const df::ScanRequest& req) const override {
        df::ScanResult r;
        r.cursor = std::make_unique<PartsCursor>(parts_, with_list_);
        r.filters.assign(req.filters.size(), df::Pushed::No);
        return r;
    }

   private:
    std::vector<Part> parts_;
    bool declare_all_;
    bool with_list_;
};

// A source declaring `declared` of each part's columns.
class RightSource : public df::Source {
   public:
    RightSource(std::vector<Part> parts, Names declared)
        : parts_(std::move(parts)), declared_(std::move(declared)) {}

    df::Schema schema() const override {
        df::Schema s;
        for (const std::string& n : declared_)
            s.fields.push_back(df::Field{n, df::scalar(TypeId::Int64), true});
        return s;
    }

    bool undeclared_columns() const override { return true; }

    df::ScanResult scan(const df::ScanRequest& req) const override {
        df::ScanResult r;
        r.cursor = std::make_unique<PartsCursor>(parts_, false);
        r.filters.assign(req.filters.size(), df::Pushed::No);
        return r;
    }

   private:
    std::vector<Part> parts_;
    Names declared_;
};

// Declares a and w; x and z are undeclared.
LazyFrame right_side() {
    return LazyFrame::scan(std::make_shared<RightSource>(
        std::vector<Part>{{{{"a", {1, 5, 6}},
                            {"w", {7, 8, 9}},
                            {"x", {11, 12, 13}},
                            {"z", {21, 22, 23}}}}},
        Names{"a", "w"}));
}

LazyFrame rest_scan(bool with_list = false) {
    return LazyFrame::scan(
        std::make_shared<PartsSource>(parts(), false, with_list));
}

LazyFrame declared_scan(bool with_list = false) {
    return LazyFrame::scan(
        std::make_shared<PartsSource>(parts(), true, with_list));
}

DataFrame run(const LazyFrame& lf) {
    DataFrame out =
        dftracer::utils::default_runtime().submit(lf.collect()).get();
    for (Series& c : out.columns) c = c.materialize();
    return out;
}

std::string cell(const Series& c, std::int64_t i) {
    return c.is_null(i) ? std::string("null") : df::cell_to_string(c, i);
}

std::vector<std::string> column(const DataFrame& f, const std::string& name) {
    const auto it = std::find(f.names.begin(), f.names.end(), name);
    REQUIRE(it != f.names.end());
    const Series& c = f.columns[static_cast<std::size_t>(it - f.names.begin())];
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < f.num_rows(); ++i) out.push_back(cell(c, i));
    return out;
}

// Equal columns by name: the undeclared ones follow the plan's columns, so
// their position differs from a plan that declares them.
void check_same(const DataFrame& got, const DataFrame& want) {
    Names g = got.names, w = want.names;
    std::sort(g.begin(), g.end());
    std::sort(w.begin(), w.end());
    REQUIRE(g == w);
    REQUIRE(got.num_rows() == want.num_rows());
    for (const std::string& name : g) {
        CAPTURE(name);
        CHECK(column(got, name) == column(want, name));
    }
}

using Op = std::function<LazyFrame(const LazyFrame&)>;

LazyFrame lookup_side() {
    DataFrame side;
    side.names = {"a", "w"};
    side.columns.push_back(ints({1, 5, 6}));
    side.columns.push_back(ints({7, 8, 9}));
    return LazyFrame::scan(
        std::make_shared<df::InMemorySource>(std::move(side)));
}

void node_schema(void*, const dftu_schema*, const dftu_op_arg*, dftu_schema*) {}

void* node_open(void*, void*, const dftu_cursor_vt*, const dftu_op_arg*,
                std::uint64_t, void**, const dftu_cursor_vt**) {
    return nullptr;
}

}  // namespace

TEST_SUITE("rest column") {
    TEST_CASE("a plain collect returns every scanned column") {
        check_same(run(rest_scan()), run(declared_scan()));
        CHECK(run(rest_scan()).names == Names{"a", "b", "x", "y"});
    }

    TEST_CASE("every op that moves rows keeps the undeclared columns") {
        const std::vector<std::pair<const char*, Op>> ops = {
            {"filter",
             [](const LazyFrame& l) {
                 return l.filter(col(0) > std::int64_t{2});
             }},
            {"head", [](const LazyFrame& l) { return l.head(5); }},
            {"slice", [](const LazyFrame& l) { return l.slice(2, 4); }},
            {"tail", [](const LazyFrame& l) { return l.tail(4); }},
            {"take", [](const LazyFrame& l) { return l.take({4, 0, 7, 5}); }},
            {"reverse", [](const LazyFrame& l) { return l.reverse(); }},
            {"sort", [](const LazyFrame& l) { return l.sort_by("a"); }},
            {"sort multi",
             [](const LazyFrame& l) {
                 return l.sort_by_multi({"a", "b"},
                                        std::vector<bool>{false, true});
             }},
            {"sample", [](const LazyFrame& l) { return l.sample(4, 7); }},
            {"head_by", [](const LazyFrame& l) { return l.head_by({"a"}, 1); }},
            {"topk", [](const LazyFrame& l) { return l.topk("a", 3); }},
            {"with_row_index",
             [](const LazyFrame& l) { return l.with_row_index("idx"); }},
            {"with_column",
             [](const LazyFrame& l) {
                 return l.with_column("c", col(0) + col(1));
             }},
            {"concat", [](const LazyFrame& l) { return l.concat(l); }},
            {"left join",
             [](const LazyFrame& l) {
                 return l.join(lookup_side(), {"a"}, df::JoinHow::Left);
             }},
            {"unique on a subset",
             [](const LazyFrame& l) { return l.unique({"a"}); }},
        };
        for (const auto& [name, op] : ops) {
            CAPTURE(name);
            check_same(run(op(rest_scan())), run(op(declared_scan())));
        }
    }

    TEST_CASE("a select that names the rest column keeps it") {
        check_same(
            run(rest_scan().select({"b", "a", "__rest"}).sort_by("a")),
            run(declared_scan().select({"b", "a", "x", "y"}).sort_by("a")));
        CHECK(rest_scan().select({"b", "a", "__rest"}).schema() ==
              Names{"b", "a"});
    }

    TEST_CASE("a derived column keeps its position for later ops") {
        DataFrame out =
            run(rest_scan()
                    .with_column("c", col(0) * df::lit(std::int64_t{2}))
                    .filter(col(2) > std::int64_t{6}));
        CHECK(out.names == Names{"a", "b", "c", "x", "y"});
        CHECK(column(out, "c") ==
              std::vector<std::string>{"8", "10", "18", "12", "8", "12"});
        CHECK(column(out, "x")[0] == "null");
        CHECK(column(out, "y")[2] == "1001");
    }

    TEST_CASE("explode keeps the undeclared columns") {
        check_same(run(rest_scan(true).explode("l")),
                   run(declared_scan(true).explode("l")));
    }

    TEST_CASE("drop_nulls counts only the plan's columns") {
        DataFrame out = run(rest_scan().drop_nulls());
        CHECK(out.names == Names{"a", "b", "x", "y"});
        CHECK(out.num_rows() == 9);
        CHECK(column(out, "x")[2] == "null");
        CHECK(column(out, "y")[4] == "1001");
    }

    TEST_CASE("fill_null fills the undeclared values a batch carries") {
        DataFrame out = run(rest_scan().fill_null(std::int64_t{0}));
        CHECK(out.names == Names{"a", "b", "x", "y"});
        CHECK(column(out, "b")[4] == "0");
        CHECK(column(out, "x")[2] == "0");
        CHECK(column(out, "x")[7] == "null");
        CHECK(column(out, "y")[0] == "null");
    }

    TEST_CASE("ops that build columns from named inputs leave them out") {
        CHECK(run(rest_scan().select({"a"})).names == Names{"a"});
        df::GroupAgg sum{df::Agg::Sum, "b", "sum_b"};
        DataFrame g = run(
            rest_scan().group_by(Names{"a"}, std::vector<df::GroupAgg>{sum}));
        CHECK(g.names == Names{"a", "sum_b"});
        CHECK(run(rest_scan().null_count()).names == Names{"a", "b"});
        CHECK(run(rest_scan().filter(col(0) > std::int64_t{0}).select({"b"}))
                  .names == Names{"b"});
    }

    TEST_CASE("whole-row identity includes the undeclared columns") {
        DataFrame u = run(rest_scan().unique());
        check_same(u, run(declared_scan().unique()));
        CHECK(u.num_rows() == 8);
        const auto x = column(u, "x");
        CHECK(std::count(x.begin(), x.end(), "101") == 1);
        CHECK(std::count(x.begin(), x.end(), "103") == 1);
        check_same(run(rest_scan().is_duplicated()),
                   run(declared_scan().is_duplicated()));
    }

    TEST_CASE("an undeclared column with the name of a plan column fails") {
        DataFrame side;
        side.names = {"a", "x"};
        side.columns.push_back(ints({1}));
        side.columns.push_back(ints({5}));
        const LazyFrame joined = rest_scan().join(
            LazyFrame::scan(
                std::make_shared<df::InMemorySource>(std::move(side))),
            {"a"}, df::JoinHow::Left);
        CHECK_THROWS_WITH_AS(run(joined), doctest::Contains("'x'"),
                             std::invalid_argument);
    }

    TEST_CASE("a concat keeps the undeclared columns of either side") {
        DataFrame left;
        left.names = {"a", "b"};
        left.columns.push_back(ints({7, 8}));
        left.columns.push_back(ints({70, 80}));
        const LazyFrame lf =
            LazyFrame::scan(
                std::make_shared<df::InMemorySource>(std::move(left)))
                .concat(rest_scan());
        DataFrame out = run(lf);
        CHECK(out.names == Names{"a", "b", "x", "y"});
        REQUIRE(out.num_rows() == 12);
        const auto x = column(out, "x");
        const auto y = column(out, "y");
        CHECK(x[0] == "null");
        CHECK(y[1] == "null");
        CHECK(x[2] == "100");
        CHECK(y[7] == "1001");
        CHECK(x[9] == "null");
    }

    TEST_CASE("a join keeps the undeclared columns of both sides") {
        for (const df::JoinHow how : {df::JoinHow::Left, df::JoinHow::Lookup}) {
            CAPTURE(static_cast<int>(how));
            DataFrame out = run(rest_scan().join(right_side(), {"a"}, how));
            Names names = out.names;
            std::sort(names.begin(), names.end());
            CHECK(names ==
                  Names{"a", "b", "w", "x", "x_right", "y", "z_right"});
            REQUIRE(out.num_rows() == 10);
            const auto x = column(out, "x");
            const auto xr = column(out, "x_right");
            const auto z = column(out, "z_right");
            const auto w = column(out, "w");
            CHECK(x[1] == "101");
            CHECK(xr[1] == "11");
            CHECK(z[1] == "21");
            CHECK(w[1] == "7");
            CHECK(x[3] == "103");
            CHECK(xr[3] == "11");
            CHECK(x[4] == "200");
            CHECK(xr[4] == "12");
            CHECK(z[4] == "22");
            CHECK(xr[0] == "null");
            CHECK(z[0] == "null");
            // The left batch without x still puts the right x under x_right.
            CHECK(x[7] == "null");
            CHECK(xr[7] == "13");
            CHECK(x[9] == "null");
            CHECK(xr[9] == "13");
        }
    }

    TEST_CASE("a declared-only left suffixes a right extra only on a clash") {
        DataFrame left;
        left.names = {"a", "b"};
        left.columns.push_back(ints({1, 6}));
        left.columns.push_back(ints({10, 60}));
        DataFrame out =
            run(LazyFrame::scan(
                    std::make_shared<df::InMemorySource>(std::move(left)))
                    .join(right_side(), {"a"}, df::JoinHow::Left));
        Names names = out.names;
        std::sort(names.begin(), names.end());
        CHECK(names == Names{"a", "b", "w", "x", "z"});
        CHECK(column(out, "x") == std::vector<std::string>{"11", "13"});
        CHECK(column(out, "z") == std::vector<std::string>{"21", "23"});

        DataFrame with_x;
        with_x.names = {"a", "x"};
        with_x.columns.push_back(ints({5}));
        with_x.columns.push_back(ints({-1}));
        DataFrame clash =
            run(LazyFrame::scan(
                    std::make_shared<df::InMemorySource>(std::move(with_x)))
                    .join(right_side(), {"a"}, df::JoinHow::Left));
        CHECK(column(clash, "x") == std::vector<std::string>{"-1"});
        CHECK(column(clash, "x_right") == std::vector<std::string>{"12"});
        CHECK(column(clash, "z") == std::vector<std::string>{"22"});
    }

    TEST_CASE("an op without a rule refuses the plan") {
        dftu_node_vt vt{};
        vt.output_schema = node_schema;
        vt.open = node_open;
        REQUIRE(dftu_node_register("test.rest.node", &vt, nullptr) == 0);
        CHECK_THROWS_WITH_AS(
            run(rest_scan().sort_by("a").op("test.rest.node", df::OpArgs())),
            doctest::Contains("op test.rest.node"), std::invalid_argument);
        CHECK(dftu_node_unregister("test.rest.node") == 0);
    }

    TEST_CASE("batches without undeclared columns give the declared ones") {
        const LazyFrame lf = LazyFrame::scan(
            std::make_shared<PartsSource>(plain_parts(), false, false));
        DataFrame out = run(lf.sort_by("a"));
        CHECK(out.names == Names{"a", "b"});
        CHECK(column(out, "a") == std::vector<std::string>{"1", "2", "3"});
        CHECK(lf.sort_by("a").schema() == Names{"a", "b"});
    }
}
