// Source planning hooks: the SourceAbsorption rule offers ops bottom up to
// Source::apply_*, and ProviderSource mirrors the hooks over
// dftu_source_vt::apply. A C++ fake exercises the planner contract; a
// hand-written vtable exercises the C marshaling and derived-source ownership.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dataframe/internal/provider_source.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::coro::CoroTask;
using dftracer::utils::dataframe::Agg;
using dftracer::utils::dataframe::AggregateSpec;
using dftracer::utils::dataframe::ApplyStatus;
using dftracer::utils::dataframe::col;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::eval;
using dftracer::utils::dataframe::Expr;
using dftracer::utils::dataframe::expr_col_index;
using dftracer::utils::dataframe::expr_fingerprint;
using dftracer::utils::dataframe::GroupAgg;
using dftracer::utils::dataframe::InMemorySource;
using dftracer::utils::dataframe::JoinSpec;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::make_provider_source;
using dftracer::utils::dataframe::NamedExpr;
using dftracer::utils::dataframe::ScanRequest;
using dftracer::utils::dataframe::ScanResult;
using dftracer::utils::dataframe::Schema;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::SortSpec;
using dftracer::utils::dataframe::Source;
using dftracer::utils::dataframe::SourceApplication;
using dftracer::utils::dataframe::TypeId;
using dftracer::utils::dataframe::detail::apply_plan_rule;
using dftracer::utils::dataframe::detail::optimize_plan;
using dftracer::utils::dataframe::detail::plan_fingerprint;
using dftracer::utils::dataframe::detail::PlanRule;

namespace {

template <class T>
T run(CoroTask<T> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

DataFrame share(const DataFrame& df) {
    DataFrame out;
    out.names = df.names;
    for (const Series& c : df.columns) out.columns.push_back(c.share());
    return out;
}

DataFrame make_df() {
    std::vector<std::int64_t> a{1, 2, 3, 4, 5, 6};
    std::vector<std::int64_t> b{60, 50, 40, 30, 20, 10};
    std::vector<std::int64_t> g{1, 1, 2, 2, 3, 3};
    DataFrame df;
    df.names = {"a", "b", "g"};
    df.columns.push_back(Series::flat_i64(a.data(), 6));
    df.columns.push_back(Series::flat_i64(b.data(), 6));
    df.columns.push_back(Series::flat_i64(g.data(), 6));
    return df;
}

std::vector<std::int64_t> ints(const DataFrame& df, const std::string& name) {
    const Series& c = df.column(name);
    const std::int64_t* p = c.data<std::int64_t>();
    return std::vector<std::int64_t>(p, p + c.length());
}

enum class Kind {
    Filter,
    Projection,
    Aggregation,
    Sort,
    TopN,
    Limit,
    Tail,
    Join
};

struct Log {
    std::vector<Kind> offered;
    std::optional<AggregateSpec> last_agg;
    std::optional<SortSpec> last_sort;
    std::int64_t last_k = -1;
};

struct Policy {
    std::set<Kind> accept;
    ApplyStatus filter_status = ApplyStatus::Exact;
    bool return_same_handle = false;
    bool projection_inexact = false;
};

// An in-memory source that absorbs ops by computing them eagerly. An Inexact
// filter leaves the rows unchanged and remembers the predicate so a repeat
// offer is refused.
class SmartSource final : public Source,
                          public std::enable_shared_from_this<SmartSource> {
   public:
    SmartSource(DataFrame df, std::shared_ptr<Log> log,
                std::shared_ptr<const Policy> policy,
                std::set<std::uint64_t> seen = {})
        : inner_(share(df)),
          frame_(std::move(df)),
          log_(std::move(log)),
          policy_(std::move(policy)),
          seen_(std::move(seen)) {}

    Schema schema() const override { return inner_.schema(); }
    ScanResult scan(const ScanRequest& req) const override {
        return inner_.scan(req);
    }
    const DataFrame* as_frame() const override { return inner_.as_frame(); }

    std::optional<SourceApplication> apply_filter(
        const Expr& predicate) const override {
        log_->offered.push_back(Kind::Filter);
        const std::uint64_t fp = expr_fingerprint(predicate);
        if (!accepts(Kind::Filter) || seen_.count(fp)) return std::nullopt;
        std::set<std::uint64_t> seen = seen_;
        seen.insert(fp);
        if (policy_->filter_status == ApplyStatus::Inexact)
            return derive(share(frame_), ApplyStatus::Inexact, std::move(seen));
        std::vector<const Series*> in;
        for (const Series& c : frame_.columns) in.push_back(&c);
        return derive(frame_.filter(eval(predicate, in)), ApplyStatus::Exact,
                      std::move(seen));
    }

    std::optional<SourceApplication> apply_projection(
        const std::vector<NamedExpr>& exprs) const override {
        log_->offered.push_back(Kind::Projection);
        if (!accepts(Kind::Projection)) return std::nullopt;
        std::vector<std::string> names;
        for (const NamedExpr& e : exprs)
            names.push_back(
                frame_.names[static_cast<std::size_t>(expr_col_index(e.expr))]);
        return derive(frame_.select(names), policy_->projection_inexact
                                                ? ApplyStatus::Inexact
                                                : ApplyStatus::Exact);
    }

    std::optional<SourceApplication> apply_aggregation(
        const AggregateSpec& spec) const override {
        log_->offered.push_back(Kind::Aggregation);
        log_->last_agg = spec;
        if (!accepts(Kind::Aggregation)) return std::nullopt;
        std::vector<std::string> keys;
        for (const NamedExpr& k : spec.keys) keys.push_back(k.name);
        std::vector<GroupAgg> aggs;
        for (const auto& a : spec.aggs) {
            GroupAgg ga;
            ga.op = a.op;
            ga.out = a.out;
            ga.param = a.param;
            if (a.input.valid())
                ga.column = frame_.names[static_cast<std::size_t>(
                    expr_col_index(a.input))];
            aggs.push_back(ga);
        }
        return derive(frame_.group_by(keys, aggs).sort_by(keys.front()),
                      ApplyStatus::Exact);
    }

    std::optional<SourceApplication> apply_sort(
        const SortSpec& spec) const override {
        log_->offered.push_back(Kind::Sort);
        log_->last_sort = spec;
        if (!accepts(Kind::Sort)) return std::nullopt;
        return derive(frame_.sort_by_multi(spec.by, spec.descending),
                      ApplyStatus::Exact);
    }

    std::optional<SourceApplication> apply_topn(const SortSpec& spec,
                                                std::int64_t k) const override {
        log_->offered.push_back(Kind::TopN);
        log_->last_sort = spec;
        log_->last_k = k;
        if (!accepts(Kind::TopN)) return std::nullopt;
        return derive(frame_.sort_by_multi(spec.by, spec.descending).head(k),
                      ApplyStatus::Exact);
    }

    std::optional<SourceApplication> apply_limit(
        std::int64_t offset, std::int64_t n) const override {
        log_->offered.push_back(Kind::Limit);
        if (!accepts(Kind::Limit)) return std::nullopt;
        return derive(frame_.slice(offset, n), ApplyStatus::Exact);
    }

    std::optional<SourceApplication> apply_tail(std::int64_t n) const override {
        log_->offered.push_back(Kind::Tail);
        if (!accepts(Kind::Tail)) return std::nullopt;
        return derive(frame_.tail(n), ApplyStatus::Exact);
    }

    std::optional<SourceApplication> apply_join(
        const JoinSpec& spec) const override {
        log_->offered.push_back(Kind::Join);
        const auto* other = dynamic_cast<const SmartSource*>(spec.other.get());
        if (!accepts(Kind::Join) || !other) return std::nullopt;
        return derive(frame_.join(other->frame_, spec.left_on, spec.right_on,
                                  spec.how, spec.suffix),
                      ApplyStatus::Exact);
    }

   private:
    bool accepts(Kind k) const { return policy_->accept.count(k) != 0; }

    std::optional<SourceApplication> derive(
        DataFrame df, ApplyStatus status,
        std::optional<std::set<std::uint64_t>> seen = std::nullopt) const {
        if (policy_->return_same_handle)
            return SourceApplication{shared_from_this(), status};
        return SourceApplication{
            std::make_shared<SmartSource>(std::move(df), log_, policy_,
                                          seen ? std::move(*seen) : seen_),
            status};
    }

    InMemorySource inner_;
    DataFrame frame_;
    std::shared_ptr<Log> log_;
    std::shared_ptr<const Policy> policy_;
    std::set<std::uint64_t> seen_;
};

struct Smart {
    std::shared_ptr<Log> log;
    LazyFrame lf;
};

Smart smart(Policy policy) {
    auto log = std::make_shared<Log>();
    LazyFrame lf = LazyFrame::scan(std::make_shared<SmartSource>(
        make_df(), log, std::make_shared<Policy>(std::move(policy))));
    return Smart{std::move(log), std::move(lf)};
}

std::vector<double> nums(const DataFrame& df, const std::string& name) {
    const Series& c = df.column(name);
    std::vector<double> out;
    for (std::int64_t i = 0; i < c.length(); ++i)
        out.push_back(c.type() == TypeId::Float64
                          ? c.data<double>()[i]
                          : static_cast<double>(c.data<std::int64_t>()[i]));
    return out;
}

std::set<Kind> all_kinds() {
    return {Kind::Filter, Kind::Projection, Kind::Aggregation, Kind::Sort,
            Kind::TopN,   Kind::Limit,      Kind::Tail,        Kind::Join};
}

}  // namespace

TEST_SUITE("source_hooks") {
    TEST_CASE("a plain source keeps every op and its plan") {
        LazyFrame lf = make_df()
                           .lazy()
                           .filter(col(0) > std::int64_t{2})
                           .sort_by("b")
                           .head(2);
        CHECK(plan_fingerprint(apply_plan_rule(
                  lf, PlanRule::SourceAbsorption)) == plan_fingerprint(lf));
    }

    TEST_CASE("an exact filter, sort and limit leave no residual ops") {
        Smart s = smart({all_kinds()});
        LazyFrame lf =
            s.lf.filter(col(0) > std::int64_t{2}).sort_by("b").head(2);
        const std::string plan = lf.explain();
        CHECK(plan.find("filter") == std::string::npos);
        CHECK(plan.find("sort_by") == std::string::npos);
        CHECK(plan.find("slice") == std::string::npos);
        DataFrame r = run(lf.collect());
        CHECK(ints(r, "a") == std::vector<std::int64_t>{6, 5});
    }

    TEST_CASE("an inexact filter stays above the derived source") {
        Policy p{all_kinds()};
        p.filter_status = ApplyStatus::Inexact;
        Smart s = smart(p);
        LazyFrame lf = s.lf.filter(col(0) > std::int64_t{2})
                           .filter(col(1) > std::int64_t{20})
                           .sort_by("a");
        const std::string plan = lf.explain();
        CHECK(plan.find("filter") != std::string::npos);
        CHECK(plan.find("sort_by") != std::string::npos);
        DataFrame r = run(lf.collect());
        CHECK(ints(r, "a") == std::vector<std::int64_t>{3, 4});
    }

    TEST_CASE("after an inexact filter only filters are offered") {
        Policy p{all_kinds()};
        p.filter_status = ApplyStatus::Inexact;
        Smart s = smart(p);
        s.log->offered.clear();
        apply_plan_rule(s.lf.filter(col(0) > std::int64_t{2})
                            .filter(col(1) > std::int64_t{20})
                            .sort_by("a"),
                        PlanRule::SourceAbsorption);
        CHECK(s.log->offered == std::vector<Kind>{Kind::Filter, Kind::Filter});
    }

    TEST_CASE("a refused op ends the walk") {
        Smart s = smart({{Kind::Filter, Kind::Limit}});
        s.log->offered.clear();
        LazyFrame lf =
            s.lf.filter(col(0) > std::int64_t{1}).sort_by("b").head(2);
        LazyFrame out = apply_plan_rule(lf, PlanRule::SourceAbsorption);
        CHECK(s.log->offered == std::vector<Kind>{Kind::Filter, Kind::Sort});
        const std::string plan = out.explain();
        CHECK(plan.find("sort_by") != std::string::npos);
        CHECK(plan.find("slice") != std::string::npos);
        CHECK(ints(run(lf.collect()), "a") == std::vector<std::int64_t>{6, 5});
    }

    TEST_CASE("a projection changes the source schema for later ops") {
        Smart s = smart({all_kinds()});
        LazyFrame lf = s.lf.select({"b", "a"}).filter(col(1) > std::int64_t{4});
        const std::string plan = lf.explain();
        CHECK(plan.rfind("scan [b, a]", 0) == 0);
        CHECK(plan.find("filter") == std::string::npos);
        DataFrame r = run(lf.collect());
        CHECK(r.names == std::vector<std::string>{"b", "a"});
        CHECK(ints(r, "a") == std::vector<std::int64_t>{5, 6});
    }

    TEST_CASE("an aggregation arrives as positional expressions") {
        Smart s = smart({all_kinds()});
        LazyFrame lf =
            s.lf.group_by(std::vector<std::string>{"g"},
                          {{Agg::Count, "", "n"}, {Agg::Sum, "b", "sum_b"}});
        DataFrame r = run(lf.collect());
        REQUIRE(s.log->last_agg);
        const AggregateSpec& spec = *s.log->last_agg;
        // Projection pushdown runs first and narrows the source to [b, g],
        // so the offered expressions index into that.
        REQUIRE(spec.keys.size() == 1);
        CHECK(spec.keys[0].name == "g");
        CHECK(expr_col_index(spec.keys[0].expr) == 1);
        REQUIRE(spec.aggs.size() == 2);
        CHECK(spec.aggs[0].op == Agg::Count);
        CHECK_FALSE(spec.aggs[0].input.valid());
        CHECK(spec.aggs[1].op == Agg::Sum);
        CHECK(expr_col_index(spec.aggs[1].input) == 0);
        CHECK(spec.aggs[1].out == "sum_b");
        CHECK(lf.explain().find("group_by") == std::string::npos);
        CHECK(r.names == std::vector<std::string>{"g", "n", "sum_b"});
        CHECK(nums(r, "sum_b") == std::vector<double>{110, 70, 30});
    }

    TEST_CASE("topk arrives as a descending top-n") {
        Smart s = smart({all_kinds()});
        DataFrame r = run(s.lf.topk("b", 2, true).collect());
        REQUIRE(s.log->last_sort);
        CHECK(s.log->last_sort->by == std::vector<std::string>{"b"});
        CHECK(s.log->last_sort->descending == std::vector<bool>{true});
        CHECK(s.log->last_k == 2);
        CHECK(ints(r, "b") == std::vector<std::int64_t>{60, 50});
    }

    TEST_CASE("the pipeline is idempotent with a smart source") {
        Policy p{all_kinds()};
        p.filter_status = ApplyStatus::Inexact;
        Smart s = smart(p);
        LazyFrame once =
            optimize_plan(s.lf.filter(col(0) > std::int64_t{2}).select({"a"}));
        CHECK(plan_fingerprint(optimize_plan(once)) == plan_fingerprint(once));
    }

    TEST_CASE("tail reaches the source") {
        Smart s = smart({all_kinds()});
        LazyFrame lf = s.lf.tail(2);
        CHECK(lf.explain().find("tail") == std::string::npos);
        CHECK(ints(run(lf.collect()), "a") == std::vector<std::int64_t>{5, 6});
    }

    TEST_CASE("a join with a fully absorbed right side is absorbed") {
        Smart left = smart({all_kinds()});
        Smart right = smart({all_kinds()});
        LazyFrame lf =
            left.lf.join(right.lf.filter(col(0) > std::int64_t{4}), {"a"});
        CHECK(lf.explain().find("join") == std::string::npos);
        DataFrame got = run(lf.collect());
        DataFrame want = run(
            make_df()
                .lazy()
                .join(make_df().lazy().filter(col(0) > std::int64_t{4}), {"a"})
                .collect());
        CHECK(got.names == want.names);
        CHECK(ints(got, "a") == ints(want, "a"));
    }

    TEST_CASE("a join whose right side keeps ops is not offered") {
        Smart left = smart({all_kinds()});
        Smart right = smart({{Kind::Filter}});
        left.log->offered.clear();
        LazyFrame lf = left.lf.join(right.lf.sort_by("b"), {"a"});
        CHECK(lf.explain().find("join") != std::string::npos);
        CHECK(left.log->offered.empty());
        CHECK(run(lf.collect()).num_rows() == 6);
    }

    TEST_CASE("a join with a different kind of source is refused") {
        Smart left = smart({all_kinds()});
        LazyFrame lf = left.lf.join(make_df().lazy(), {"a"});
        CHECK(lf.explain().find("join") != std::string::npos);
        CHECK(left.log->offered == std::vector<Kind>{Kind::Join});
    }

    TEST_CASE("a hook returning the same handle is a contract error") {
        Policy p{all_kinds()};
        p.return_same_handle = true;
        Smart s = smart(p);
        CHECK_THROWS_AS(apply_plan_rule(s.lf.filter(col(0) > std::int64_t{1}),
                                        PlanRule::SourceAbsorption),
                        std::logic_error);
    }

    TEST_CASE("an inexact schema change is a contract error") {
        Policy p{all_kinds()};
        p.projection_inexact = true;
        Smart s = smart(p);
        CHECK_THROWS_AS(
            apply_plan_rule(s.lf.select({"a"}), PlanRule::SourceAbsorption),
            std::logic_error);
    }
}

namespace {

// C fixture: 10 rows id 0..9, val = id*10. apply() answers per ApplyFixture.
struct ApplyFixture {
    std::int32_t answer = DFTU_APPLY_EXACT;
    std::int64_t offset = 0;
    std::int64_t limit = -1;
    int derived_alive = 0;
    int derived_destroyed = 0;
    std::vector<dftu_apply_request> requests;
    std::vector<std::string> key_names;
    std::vector<std::string> agg_ops;
    std::vector<bool> agg_has_input;
    bool join_is_ours = false;
    std::vector<std::string> join_on;
    std::int32_t join_how = -1;
    std::string join_suffix;
};

struct Derived {
    ApplyFixture* fx;
    std::int64_t offset;
    std::int64_t n;
};

struct CursorState {
    dftu_dataframe* frame;
};

dftu_dataframe* make_rows(std::int64_t offset, std::int64_t n) {
    std::vector<std::int64_t> ids, vals;
    for (std::int64_t i = 0; i < 10; ++i) {
        ids.push_back(i);
        vals.push_back(i * 10);
    }
    dftu_series* id_col =
        dftu_series_new_flat(DFTU_TYPE_INT64, ids.data(), 10, nullptr);
    dftu_series* val_col =
        dftu_series_new_flat(DFTU_TYPE_INT64, vals.data(), 10, nullptr);
    const char* names[] = {"id", "val"};
    dftu_series* cols[] = {id_col, val_col};
    dftu_dataframe* df = dftu_dataframe_new(names, cols, 2);
    if (n < 0) return df;
    dftu_dataframe* sliced = dftu_dataframe_slice(df, offset, n);
    dftu_dataframe_free(df);
    return sliced;
}

std::int32_t c_schema(void*, const char* const** out_names) {
    static const char* names[] = {"id", "val"};
    *out_names = names;
    return 2;
}

dftu_task* c_next(void* self, std::int64_t, dftu_result_frame* out) {
    auto* cs = static_cast<CursorState*>(self);
    out->ok = 1;
    out->u.value = cs->frame;
    cs->frame = nullptr;
    return nullptr;
}

void c_cursor_destroy(void* self) {
    auto* cs = static_cast<CursorState*>(self);
    if (cs->frame) dftu_dataframe_free(cs->frame);
    delete cs;
}

const dftu_cursor_vt C_CURSOR_VT = {c_next, c_cursor_destroy, nullptr, nullptr,
                                    nullptr};

void* open_cursor(dftu_dataframe* df, void** out_cursor_self,
                  const dftu_cursor_vt** out_vt) {
    auto* cs = new CursorState{df};
    *out_cursor_self = cs;
    *out_vt = &C_CURSOR_VT;
    return cs;
}

void* c_root_scan(void*, const dftu_scan_request*, std::int32_t*,
                  void** out_cursor_self, const dftu_cursor_vt** out_vt) {
    return open_cursor(make_rows(0, -1), out_cursor_self, out_vt);
}

void* c_derived_scan(void* self, const dftu_scan_request*, std::int32_t*,
                     void** out_cursor_self, const dftu_cursor_vt** out_vt) {
    auto* d = static_cast<Derived*>(self);
    return open_cursor(make_rows(d->offset, d->n), out_cursor_self, out_vt);
}

void c_derived_destroy(void* self) {
    auto* d = static_cast<Derived*>(self);
    --d->fx->derived_alive;
    ++d->fx->derived_destroyed;
    delete d;
}

void c_root_destroy(void*) {}

void c_apply(void* self, const dftu_apply_request* req, dftu_apply_result* out);

const dftu_source_vt DERIVED_VT = {c_schema, c_derived_scan, c_derived_destroy,
                                   nullptr, nullptr};

void c_apply(void* self, const dftu_apply_request* req,
             dftu_apply_result* out) {
    auto* fx = static_cast<ApplyFixture*>(self);
    fx->requests.push_back(*req);
    if (req->kind == DFTU_APPLY_AGGREGATION) {
        const dftu_apply_aggregation_args& a = req->u.aggregation;
        for (std::int32_t i = 0; i < a.n_keys; ++i)
            fx->key_names.push_back(a.keys[i].name);
        for (std::int32_t i = 0; i < a.n_aggs; ++i) {
            fx->agg_ops.push_back(a.aggs[i].op);
            fx->agg_has_input.push_back(a.aggs[i].input != nullptr);
        }
    }
    if (req->kind == DFTU_APPLY_JOIN) {
        const dftu_apply_join_args& j = req->u.join;
        fx->join_is_ours = j.other_vt && j.other_vt->schema == c_schema;
        for (std::int32_t i = 0; i < j.n_on; ++i)
            fx->join_on.push_back(std::string(j.left_on[i]) + "=" +
                                  j.right_on[i]);
        fx->join_how = j.how;
        fx->join_suffix = j.suffix;
    }
    if (fx->answer == DFTU_APPLY_NO_CHANGE) return;
    const bool limit = req->kind == DFTU_APPLY_LIMIT;
    auto* d = new Derived{fx, limit ? req->u.limit.offset : 0,
                          limit ? req->u.limit.n : -1};
    ++fx->derived_alive;
    out->status = fx->answer;
    out->self = d;
    out->vt = &DERIVED_VT;
}

const dftu_source_vt ROOT_VT = {c_schema, c_root_scan, c_root_destroy, nullptr,
                                c_apply};

LazyFrame c_frame(ApplyFixture& fx) {
    return LazyFrame::scan(make_provider_source(ROOT_VT, &fx));
}

}  // namespace

TEST_SUITE("source_hooks_c_abi") {
    TEST_CASE("an exact limit reaches the derived source and frees it once") {
        ApplyFixture fx;
        {
            LazyFrame lf = c_frame(fx).slice(2, 3);
            DataFrame r = run(lf.collect());
            CHECK(ints(r, "id") == std::vector<std::int64_t>{2, 3, 4});
            REQUIRE_FALSE(fx.requests.empty());
            CHECK(fx.requests[0].kind == DFTU_APPLY_LIMIT);
            CHECK(fx.requests[0].u.limit.offset == 2);
            CHECK(fx.requests[0].u.limit.n == 3);
        }
        CHECK(fx.derived_alive == 0);
        CHECK(fx.derived_destroyed >= 1);
    }

    TEST_CASE("an open cursor keeps its derived source alive") {
        ApplyFixture fx;
        std::unique_ptr<dftracer::utils::dataframe::Cursor> cur;
        {
            LazyFrame lf = c_frame(fx).slice(0, 4);
            cur = lf.open_cursor();
        }
        CHECK(fx.derived_alive == 1);
        cur.reset();
        CHECK(fx.derived_alive == 0);
    }

    TEST_CASE("an inexact answer to a projection is refused and freed") {
        ApplyFixture fx;
        fx.answer = DFTU_APPLY_INEXACT;
        LazyFrame lf = c_frame(fx).select({"val"});
        const std::string plan = lf.explain();
        CHECK(plan.find("select [val]") != std::string::npos);
        CHECK(fx.derived_alive == 0);
    }

    TEST_CASE("an unknown status reads as no change and is freed") {
        ApplyFixture fx;
        fx.answer = 42;
        LazyFrame lf = c_frame(fx).slice(0, 2);
        CHECK(lf.explain().find("slice") != std::string::npos);
        CHECK(fx.derived_alive == 0);
        CHECK(ints(run(lf.collect()), "id") == std::vector<std::int64_t>{0, 1});
    }

    TEST_CASE("an aggregation marshals named keys and optional inputs") {
        ApplyFixture fx;
        fx.answer = DFTU_APPLY_NO_CHANGE;
        LazyFrame lf = c_frame(fx).group_by(
            std::vector<std::string>{"id"},
            {{Agg::Count, "", "n"}, {Agg::Sum, "val", "s"}});
        (void)lf.explain();
        CHECK(fx.key_names == std::vector<std::string>{"id"});
        CHECK(fx.agg_ops == std::vector<std::string>{"count", "sum"});
        CHECK(fx.agg_has_input == std::vector<bool>{false, true});
    }

    TEST_CASE("tail marshals its row count") {
        ApplyFixture fx;
        fx.answer = DFTU_APPLY_NO_CHANGE;
        (void)c_frame(fx).tail(3).explain();
        REQUIRE(fx.requests.size() == 1);
        CHECK(fx.requests[0].kind == DFTU_APPLY_TAIL);
        CHECK(fx.requests[0].u.tail.n == 3);
    }

    TEST_CASE("a join marshals the other source and keeps it alive") {
        ApplyFixture fx;
        std::unique_ptr<dftracer::utils::dataframe::Cursor> cur;
        {
            LazyFrame right = c_frame(fx).slice(0, 5);
            LazyFrame lf = c_frame(fx).join(
                right, {"id"}, dftracer::utils::dataframe::JoinHow::Left, "_r");
            cur = lf.open_cursor();
        }
        CHECK(fx.join_is_ours);
        CHECK(fx.join_on == std::vector<std::string>{"id=id"});
        CHECK(fx.join_how == DFTU_JOIN_LEFT);
        CHECK(fx.join_suffix == "_r");
        // The limit-derived right side and the join-derived source.
        CHECK(fx.derived_alive == 2);
        cur.reset();
        CHECK(fx.derived_alive == 0);
    }

    TEST_CASE("a source without apply keeps the ScanRequest path") {
        ApplyFixture fx;
        const dftu_source_vt vt = {c_schema, c_root_scan, c_root_destroy,
                                   nullptr, nullptr};
        LazyFrame lf =
            LazyFrame::scan(make_provider_source(vt, &fx)).slice(1, 2);
        CHECK(lf.explain().find("slice") != std::string::npos);
        CHECK(ints(run(lf.collect()), "id") == std::vector<std::int64_t>{1, 2});
        CHECK(fx.requests.empty());
    }
}

namespace {

struct BatchLog {
    int opened = 0;
    int scans = 0;
    int batches = 0;
    std::size_t last_members = 0;
};

// An in-memory source that reports a batch key, and counts how it is read.
class KeyedSource final : public Source {
   public:
    KeyedSource(DataFrame df, std::string key, std::shared_ptr<BatchLog> log)
        : inner_(share(df)),
          frame_(std::move(df)),
          key_(std::move(key)),
          log_(std::move(log)) {}

    Schema schema() const override { return inner_.schema(); }
    ScanResult scan(const ScanRequest& req) const override {
        ++log_->scans;
        return inner_.scan(req);
    }
    std::optional<std::string> batch_key() const override { return key_; }
    std::optional<
        std::vector<std::unique_ptr<dftracer::utils::dataframe::Cursor>>>
    open_batch(std::vector<std::shared_ptr<const Source>> members,
               std::uint64_t) const override {
        ++log_->opened;
        std::vector<std::unique_ptr<dftracer::utils::dataframe::Cursor>> out;
        for (const auto& m : members)
            out.push_back(static_cast<const KeyedSource&>(*m)
                              .inner_.scan(ScanRequest{})
                              .cursor);
        return out;
    }
    CoroTask<std::vector<DataFrame>> collect_batch(
        std::vector<std::shared_ptr<const Source>> members) const override {
        ++log_->batches;
        log_->last_members = members.size();
        std::vector<DataFrame> out;
        for (const auto& m : members)
            out.push_back(share(static_cast<const KeyedSource&>(*m).frame_));
        co_return out;
    }

   private:
    InMemorySource inner_;
    DataFrame frame_;
    std::string key_;
    std::shared_ptr<BatchLog> log_;
};

LazyFrame keyed(const std::string& key, std::shared_ptr<BatchLog> log) {
    return LazyFrame::scan(
        std::make_shared<KeyedSource>(make_df(), key, std::move(log)));
}

}  // namespace

TEST_SUITE("batched leaves") {
    TEST_CASE("a join of two same-key sources reads through one batch") {
        auto log = std::make_shared<BatchLog>();
        LazyFrame lf =
            keyed("k", log).join(keyed("k", log).select({"a", "g"}), {"a"});
        DataFrame got = run(lf.collect());
        CHECK(log->batches == 1);
        CHECK(log->last_members == 2);
        CHECK(log->scans == 0);
        DataFrame want =
            run(make_df()
                    .lazy()
                    .join(make_df().lazy().select({"a", "g"}), {"a"})
                    .collect());
        CHECK(got.names == want.names);
        CHECK(ints(got, "a") == ints(want, "a"));
    }

    TEST_CASE("a concat shares the batch and keeps each side's ops") {
        auto log = std::make_shared<BatchLog>();
        LazyFrame lf =
            keyed("k", log)
                .filter(col(0) > std::int64_t{4})
                .concat(keyed("k", log).filter(col(0) < std::int64_t{2}));
        DataFrame got = run(lf.collect());
        CHECK(log->batches == 1);
        CHECK(ints(got, "a") == std::vector<std::int64_t>{5, 6, 1});
    }

    TEST_CASE("different keys do not batch") {
        auto log = std::make_shared<BatchLog>();
        DataFrame got =
            run(keyed("x", log).join(keyed("y", log), {"a"}).collect());
        CHECK(log->batches == 0);
        CHECK(log->scans == 2);
        CHECK(got.num_rows() == 6);
    }

    TEST_CASE("collect_all streams a group of top-level plans") {
        auto log = std::make_shared<BatchLog>();
        std::vector<DataFrame> out =
            run(dftracer::utils::dataframe::collect_all(
                {keyed("k", log).filter(col(0) > std::int64_t{3}),
                 keyed("k", log).head(2), keyed("k", log)}));
        CHECK(log->opened == 1);
        CHECK(log->batches == 0);
        CHECK(log->scans == 0);
        CHECK(ints(out[0], "a") == std::vector<std::int64_t>{4, 5, 6});
        CHECK(out[1].num_rows() == 2);
        CHECK(out[2].num_rows() == 6);
    }

    TEST_CASE("a leading projection keeps a group on the buffered read") {
        auto log = std::make_shared<BatchLog>();
        run(dftracer::utils::dataframe::collect_all(
            {keyed("k", log).select({"a"}), keyed("k", log)}));
        CHECK(log->opened == 0);
        CHECK(log->batches == 1);
    }

    TEST_CASE("collect_all batches leaves across roots and their children") {
        auto log = std::make_shared<BatchLog>();
        std::vector<DataFrame> out =
            run(dftracer::utils::dataframe::collect_all(
                {keyed("k", log).join(keyed("k", log), {"a"}),
                 keyed("k", log).head(2)}));
        CHECK(log->batches == 1);
        CHECK(log->last_members == 3);
        CHECK(log->scans == 0);
        CHECK(out[0].num_rows() == 6);
        CHECK(out[1].num_rows() == 2);
    }
}
