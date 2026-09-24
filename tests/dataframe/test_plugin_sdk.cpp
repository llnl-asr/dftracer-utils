// The plugin SDK's C++ wrappers: a source (plugins/plugin/source.h) and a plan
// node (plugins/plugin/node.h) written as classes, adapted to their C vtables
// and driven by the host through the same adapters a loaded plugin gets.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/provider_source.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/plugins/plugin/node.h>
#include <dftracer/utils/plugins/plugin/source.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace df = dftracer::utils::dataframe;
namespace pl = dftracer::utils::plugins;

namespace {

df::DataFrame run(dftracer::utils::coro::CoroTask<df::DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

std::vector<std::int64_t> ints(const df::DataFrame& f, const char* name) {
    const df::Series& c = f.column(name);
    const std::int64_t* p = c.data<std::int64_t>();
    return {p, p + c.length()};
}

struct Counters {
    int filters_absorbed = 0;
    int joins_absorbed = 0;
    int sources_alive = 0;
    int cursors_destroyed = 0;
};

// Rows id = lo..hi-1, val = id * 10, kept in memory.
class RangeSource {
   public:
    RangeSource(std::shared_ptr<Counters> c, std::int64_t lo, std::int64_t hi,
                bool fail = false)
        : c_(std::move(c)), lo_(lo), hi_(hi), fail_(fail) {
        ++c_->sources_alive;
    }
    RangeSource(const RangeSource&) = delete;
    ~RangeSource() { --c_->sources_alive; }

    std::vector<std::string> names() const { return {"id", "val"}; }

    void schema(pl::SchemaBuilder& b) const {
        b.add("id", DFTU_TYPE_INT64, false);
        b.add("val", DFTU_TYPE_INT64, false);
    }

    class Cursor {
       public:
        Cursor(pl::OwnedFrame f, bool fail, std::shared_ptr<Counters> c)
            : f_(std::move(f)), fail_(fail), c_(std::move(c)) {}
        Cursor(const Cursor&) = delete;
        ~Cursor() { ++c_->cursors_destroyed; }
        std::optional<pl::OwnedFrame> next(std::int64_t) {
            if (fail_) throw std::runtime_error("range cursor broke");
            if (!f_) return std::nullopt;
            return std::move(f_);
        }

       private:
        pl::OwnedFrame f_;
        bool fail_;
        std::shared_ptr<Counters> c_;
    };

    pl::ScanResult<Cursor> scan(const pl::ScanView& req) const {
        std::vector<std::int64_t> ids, vals;
        for (std::int64_t i = lo_; i < hi_; ++i) {
            ids.push_back(i);
            vals.push_back(i * 10);
        }
        const auto n = static_cast<std::int64_t>(ids.size());
        std::vector<const char*> names;
        std::vector<dftu_series*> cols;
        auto want = [&](const char* name) {
            if (req.projection.empty()) return true;
            for (std::string_view p : req.projection)
                if (p == name) return true;
            return false;
        };
        if (want("id")) {
            names.push_back("id");
            cols.push_back(
                dftu_series_new_flat(DFTU_TYPE_INT64, ids.data(), n, nullptr));
        }
        if (want("val")) {
            names.push_back("val");
            cols.push_back(
                dftu_series_new_flat(DFTU_TYPE_INT64, vals.data(), n, nullptr));
        }
        pl::OwnedFrame f(dftu_dataframe_new(
            names.data(), cols.data(), static_cast<std::int32_t>(cols.size())));
        return {std::make_unique<Cursor>(std::move(f), fail_, c_), {}};
    }

    // `id > k` (and `id >= k`) narrows the range exactly.
    std::optional<pl::Applied<RangeSource>> apply_filter(
        pl::ExprView pred) const {
        auto cmp = pred.as_compare();
        if (!cmp || cmp->column != 0 || cmp->rhs.kind != DFTU_SCALAR_TAG_I64)
            return std::nullopt;
        std::int64_t lo = lo_;
        if (cmp->op == DFTU_CMP_GT)
            lo = cmp->rhs.value.i + 1;
        else if (cmp->op == DFTU_CMP_GE)
            lo = cmp->rhs.value.i;
        else
            return std::nullopt;
        if (lo <= lo_) return std::nullopt;
        ++c_->filters_absorbed;
        return pl::Applied<RangeSource>{
            std::make_unique<RangeSource>(c_, lo, hi_, fail_),
            pl::Apply::Exact};
    }

    std::optional<pl::Applied<RangeSource>> apply_limit(std::int64_t offset,
                                                        std::int64_t n) const {
        const std::int64_t lo = lo_ + offset;
        const std::int64_t hi = std::min(hi_, lo + n);
        if (lo == lo_ && hi == hi_) return std::nullopt;
        return pl::Applied<RangeSource>{
            std::make_unique<RangeSource>(c_, lo, hi, fail_), pl::Apply::Exact};
    }

    // Records the offer of an inner join on id, then leaves it to the engine.
    std::optional<pl::Applied<RangeSource>> apply_join(
        const pl::JoinView<RangeSource>& j) const {
        if (j.how != DFTU_JOIN_INNER || j.left_on.size() != 1 ||
            j.left_on[0] != "id" || j.right_on[0] != "id")
            return std::nullopt;
        (void)j.other;
        ++c_->joins_absorbed;
        return std::nullopt;
    }

   private:
    std::shared_ptr<Counters> c_;
    std::int64_t lo_;
    std::int64_t hi_;
    bool fail_;
};

// A root source for the test: the adapter's self and the host Source over it.
struct Root {
    const dftu_source_vt* vt = pl::source_vtable<RangeSource>();
    void* self;
    std::shared_ptr<df::Source> source;

    Root(std::shared_ptr<Counters> c, std::int64_t lo, std::int64_t hi,
         bool fail = false)
        : self(pl::make_source_self(
              std::make_unique<RangeSource>(std::move(c), lo, hi, fail))),
          source(df::make_provider_source(*vt, self)) {}
    ~Root() {
        source.reset();
        vt->destroy(self);
    }
};

}  // namespace

TEST_SUITE("plugin SDK source") {
    TEST_CASE("the vtable carries the optional slots the class provides") {
        const dftu_source_vt* vt = pl::source_vtable<RangeSource>();
        CHECK(vt->schema_types != nullptr);
        CHECK(vt->apply != nullptr);
    }

    TEST_CASE("a plain scan reads every row with typed columns") {
        auto c = std::make_shared<Counters>();
        Root r(c, 0, 5);
        df::LazyFrame lf = df::LazyFrame::scan(r.source);
        df::Schema s = r.source->schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[0].type.id == df::TypeId::Int64);
        df::DataFrame f = run(lf.collect());
        CHECK(ints(f, "id") == std::vector<std::int64_t>{0, 1, 2, 3, 4});
    }

    TEST_CASE("an absorbed filter and limit reach the class") {
        auto c = std::make_shared<Counters>();
        {
            Root r(c, 0, 10);
            df::LazyFrame lf = df::LazyFrame::scan(r.source)
                                   .filter(df::col(0) > std::int64_t{4})
                                   .head(3);
            CHECK(lf.explain() == "scan [id, val]");
            df::DataFrame f = run(lf.collect());
            CHECK(ints(f, "id") == std::vector<std::int64_t>{5, 6, 7});
            CHECK(ints(f, "val") == std::vector<std::int64_t>{50, 60, 70});
            CHECK(c->filters_absorbed >= 1);
        }
        CHECK(c->sources_alive == 0);
    }

    TEST_CASE("an untranslatable filter stays with the engine") {
        auto c = std::make_shared<Counters>();
        Root r(c, 0, 10);
        df::LazyFrame lf =
            df::LazyFrame::scan(r.source).filter(df::col(1) > std::int64_t{60});
        CHECK(lf.explain().find("filter") != std::string::npos);
        CHECK(ints(run(lf.collect()), "id") ==
              std::vector<std::int64_t>{7, 8, 9});
        CHECK(c->filters_absorbed == 0);
    }

    TEST_CASE("a join offers the other side as the class type") {
        auto c = std::make_shared<Counters>();
        Root left(c, 0, 5);
        Root right(c, 3, 8);
        df::LazyFrame lf = df::LazyFrame::scan(left.source)
                               .join(df::LazyFrame::scan(right.source), {"id"});
        df::DataFrame f = run(lf.collect());
        CHECK(c->joins_absorbed >= 1);
        CHECK(ints(f, "id") == std::vector<std::int64_t>{3, 4});
    }

    TEST_CASE("a cursor exception surfaces as a scan error") {
        auto c = std::make_shared<Counters>();
        Root r(c, 0, 3, true);
        CHECK_THROWS_WITH(run(df::LazyFrame::scan(r.source).collect()),
                          doctest::Contains("range cursor broke"));
    }
}

namespace {

enum class OpenMode { Normal, Throw, DropInput };

// Doubles column "val"; keeps every column in place.
class DoubleVal {
   public:
    explicit DoubleVal(OpenMode mode) : mode_(mode) {}

    void output_schema(const pl::SchemaView& in, const pl::OpArgs&,
                       pl::SchemaBuilder& out) const {
        in.copy_all(out);
    }

    class Cursor {
       public:
        explicit Cursor(std::optional<pl::InputCursor> in)
            : in_(std::move(in)) {}
        std::optional<pl::OwnedFrame> next(std::int64_t max_rows) {
            if (!in_) return std::nullopt;
            std::optional<pl::OwnedFrame> f = in_->next(max_rows);
            if (!f) return std::nullopt;
            dftu_series* id = dftu_dataframe_column(f->get(), "id");
            dftu_series* val = dftu_dataframe_column(f->get(), "val");
            const std::int64_t n = dftu_series_length(val);
            const auto* v =
                static_cast<const std::int64_t*>(dftu_series_data(val));
            std::vector<std::int64_t> doubled(v, v + n);
            for (std::int64_t& x : doubled) x *= 2;
            const char* names[] = {"id", "val"};
            dftu_series* cols[] = {
                id, dftu_series_new_flat(DFTU_TYPE_INT64, doubled.data(), n,
                                         nullptr)};
            dftu_series_free(val);
            return pl::OwnedFrame(dftu_dataframe_new(names, cols, 2));
        }

       private:
        std::optional<pl::InputCursor> in_;
    };

    std::unique_ptr<Cursor> open(pl::InputCursor in, const pl::OpArgs&) const {
        if (mode_ == OpenMode::Throw) throw std::runtime_error("no open");
        if (mode_ == OpenMode::DropInput)
            return std::make_unique<Cursor>(std::nullopt);
        return std::make_unique<Cursor>(std::move(in));
    }

   private:
    OpenMode mode_;
};

struct RegisteredNode {
    std::string name;
    RegisteredNode(std::string n, OpenMode mode) : name(std::move(n)) {
        REQUIRE(dftu_node_register(
                    name.c_str(), pl::node_vtable<DoubleVal>(),
                    pl::make_node_self(std::make_unique<DoubleVal>(mode))) ==
                0);
    }
    ~RegisteredNode() { dftu_node_unregister(name.c_str()); }
};

}  // namespace

TEST_SUITE("plugin SDK node") {
    TEST_CASE("a node class runs as a plan step") {
        auto c = std::make_shared<Counters>();
        RegisteredNode node("sdk.double", OpenMode::Normal);
        {
            Root r(c, 0, 4);
            df::LazyFrame lf =
                df::LazyFrame::scan(r.source).op("sdk.double", df::OpArgs{});
            CHECK(lf.explain().find("op sdk.double") != std::string::npos);
            df::DataFrame f = run(lf.collect());
            CHECK(ints(f, "id") == std::vector<std::int64_t>{0, 1, 2, 3});
            CHECK(ints(f, "val") == std::vector<std::int64_t>{0, 20, 40, 60});
        }
        CHECK(c->cursors_destroyed == 1);
    }

    TEST_CASE("a throwing open leaves the upstream with the host") {
        auto c = std::make_shared<Counters>();
        RegisteredNode node("sdk.double_throw", OpenMode::Throw);
        {
            Root r(c, 0, 4);
            CHECK_THROWS_WITH(run(df::LazyFrame::scan(r.source)
                                      .op("sdk.double_throw", df::OpArgs{})
                                      .collect()),
                              doctest::Contains("sdk.double_throw"));
        }
        CHECK(c->cursors_destroyed == 1);
    }

    TEST_CASE("an input dropped inside open is still destroyed once") {
        auto c = std::make_shared<Counters>();
        RegisteredNode node("sdk.double_drop", OpenMode::DropInput);
        {
            Root r(c, 0, 4);
            df::DataFrame f = run(df::LazyFrame::scan(r.source)
                                      .op("sdk.double_drop", df::OpArgs{})
                                      .collect());
            CHECK(f.num_rows() == 0);
        }
        CHECK(c->cursors_destroyed == 1);
    }
}
