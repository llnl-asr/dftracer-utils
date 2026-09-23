// Provider pushdown: the C-ABI adapter (provider_registry.cpp) marshals
// ScanRequest/ScanResult across dftu_source_vt::scan as dftu_scan_request and
// an out_pushed array. These tests drive a hand-written dftu_source_vt
// directly through make_provider_source (an internal test-only bridge, not
// the registry), so each hazard the adapter must defend against - an
// untrusted out_pushed value, an ignored projection, a false Exact claim - is
// exercised without the plugin/dlopen machinery.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/provider_source.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using dftracer::utils::coro::CoroTask;
using dftracer::utils::dataframe::col;
using dftracer::utils::dataframe::Cursor;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::make_provider_source;
using dftracer::utils::dataframe::Morsel;
using dftracer::utils::dataframe::Pushed;
using dftracer::utils::dataframe::ScanRequest;
using dftracer::utils::dataframe::ScanResult;
using dftracer::utils::dataframe::Source;

namespace {

DataFrame run(CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

CoroTask<DataFrame> drain_cursor(std::unique_ptr<Cursor> cur) {
    DataFrame out;
    while (auto m = co_await cur->next(1 << 20)) {
        out.columns = std::move(m->columns);
        out.names.assign(out.columns.size(), std::string());
    }
    co_return out;
}

// How the fixture source handles the one filter a test pushes.
enum class FilterBehavior {
    Ignore,
    ApplyExact,
    ApplyInexact,
    OutOfRange,
    FalseExact
};

struct FixtureConfig {
    FilterBehavior filter_behavior = FilterBehavior::Ignore;
    bool honor_projection = true;
    bool apply_limit = false;
};

int32_t fixture_schema(void*, const char* const** out_names) {
    static const char* names[] = {"id", "val"};
    *out_names = names;
    return 2;
}

struct CursorState {
    dftu_dataframe* frame;
    bool done = false;
};

dftu_task* fixture_cursor_next(void* self, int64_t, dftu_result_frame* out) {
    auto* cs = static_cast<CursorState*>(self);
    out->ok = 1;
    if (cs->done) {
        out->u.value = nullptr;
    } else {
        out->u.value = cs->frame;
        cs->frame = nullptr;
        cs->done = true;
    }
    return nullptr;
}

void fixture_cursor_destroy(void* self) {
    auto* cs = static_cast<CursorState*>(self);
    if (cs->frame) dftu_dataframe_free(cs->frame);
    delete cs;
}

const dftu_cursor_vt FIXTURE_CURSOR_VT = {
    fixture_cursor_next, fixture_cursor_destroy, nullptr, nullptr, nullptr};

dftu_dataframe* make_base_frame() {
    std::int64_t ids[10];
    std::int64_t vals[10];
    for (int i = 0; i < 10; ++i) {
        ids[i] = i;
        vals[i] = i * 10;
    }
    dftu_series* id_col =
        dftu_series_new_flat(DFTU_TYPE_INT64, ids, 10, nullptr);
    dftu_series* val_col =
        dftu_series_new_flat(DFTU_TYPE_INT64, vals, 10, nullptr);
    const char* names[] = {"id", "val"};
    dftu_series* cols[] = {id_col, val_col};
    return dftu_dataframe_new(names, cols, 2);
}

// 10 rows: id 0..9, val = id*10. Honors/ignores projection and applies its
// one filter per cfg->filter_behavior, so a single vtable drives every test
// below by varying `self`.
void* fixture_scan(void* self, const dftu_scan_request* req,
                   int32_t* out_pushed, void** out_cursor_self,
                   const dftu_cursor_vt** out_vt) {
    auto* cfg = static_cast<FixtureConfig*>(self);
    dftu_dataframe* df = make_base_frame();

    if (req->n_projection > 0 && cfg->honor_projection) {
        std::vector<const char*> proj_names;
        std::vector<dftu_series*> proj_cols;
        for (int32_t i = 0; i < req->n_projection; ++i) {
            proj_names.push_back(req->projection[i]);
            proj_cols.push_back(dftu_dataframe_column(df, req->projection[i]));
        }
        dftu_dataframe* projected =
            dftu_dataframe_new(proj_names.data(), proj_cols.data(),
                               static_cast<int32_t>(proj_cols.size()));
        dftu_dataframe_free(df);
        df = projected;
    }

    if (req->n_filters > 0) {
        switch (cfg->filter_behavior) {
            case FilterBehavior::Ignore:
                break;  // leave out_pushed[0] at the host's pre-filled No.
            case FilterBehavior::ApplyExact: {
                const int32_t nc = dftu_dataframe_num_columns(df);
                std::vector<dftu_series*> owned;
                std::vector<const dftu_series*> inputs;
                for (int32_t c = 0; c < nc; ++c) {
                    dftu_series* col_c = dftu_dataframe_column(
                        df, dftu_dataframe_column_name(df, c));
                    owned.push_back(col_c);
                    inputs.push_back(col_c);
                }
                dftu_series* mask =
                    dftu_expr_eval(req->filters[0], inputs.data(), nc);
                for (dftu_series* c : owned) dftu_series_free(c);
                dftu_dataframe* filtered = dftu_dataframe_filter(df, mask);
                dftu_series_free(mask);
                dftu_dataframe_free(df);
                df = filtered;
                out_pushed[0] = DFTU_PUSHED_EXACT;
                break;
            }
            case FilterBehavior::ApplyInexact: {
                // A coarse prune: drops rows 0-2, a strict superset of the
                // id>5 predicate the tests push, so the engine must still
                // narrow it.
                dftu_dataframe* sliced = dftu_dataframe_slice(df, 3, 7);
                dftu_dataframe_free(df);
                df = sliced;
                out_pushed[0] = DFTU_PUSHED_INEXACT;
                break;
            }
            case FilterBehavior::OutOfRange:
                out_pushed[0] = 99;
                break;
            case FilterBehavior::FalseExact:
                // Claims Exact without touching a single row: the hazard
                // safety rule (c) exists because this claim cannot be
                // verified cheaply at runtime.
                out_pushed[0] = DFTU_PUSHED_EXACT;
                break;
        }
    }

    if (cfg->apply_limit && req->limit >= 0) {
        dftu_dataframe* limited = dftu_dataframe_head(df, req->limit);
        dftu_dataframe_free(df);
        df = limited;
    }

    auto* cs = new CursorState{df};
    *out_cursor_self = cs;
    *out_vt = &FIXTURE_CURSOR_VT;
    return cs;
}

void fixture_destroy(void*) {}

std::shared_ptr<Source> make_fixture_source(FixtureConfig& cfg) {
    static const dftu_source_vt vt = {fixture_schema, fixture_scan,
                                      fixture_destroy, nullptr};
    return make_provider_source(vt, &cfg);
}

}  // namespace

TEST_SUITE("provider pushdown") {
    TEST_CASE("Exact and No give the same rows (engine re-applies No)") {
        FixtureConfig exact_cfg;
        exact_cfg.filter_behavior = FilterBehavior::ApplyExact;
        FixtureConfig no_cfg;
        no_cfg.filter_behavior = FilterBehavior::Ignore;

        auto pred = col(0) > std::int64_t{5};
        DataFrame exact_result =
            run(LazyFrame::scan(make_fixture_source(exact_cfg))
                    .filter(pred)
                    .collect());
        DataFrame no_result = run(LazyFrame::scan(make_fixture_source(no_cfg))
                                      .filter(pred)
                                      .collect());

        REQUIRE(exact_result.num_rows() == 4);
        REQUIRE(no_result.num_rows() == 4);
        const std::int64_t* e_ids =
            exact_result.column("id").data<std::int64_t>();
        const std::int64_t* n_ids = no_result.column("id").data<std::int64_t>();
        for (int i = 0; i < 4; ++i) CHECK(e_ids[i] == n_ids[i]);
    }

    TEST_CASE("Inexact is re-applied by the engine, narrowing the survivors") {
        FixtureConfig cfg;
        cfg.filter_behavior = FilterBehavior::ApplyInexact;

        DataFrame result = run(LazyFrame::scan(make_fixture_source(cfg))
                                   .filter(col(0) > std::int64_t{5})
                                   .collect());
        REQUIRE(result.num_rows() == 4);
        const std::int64_t* ids = result.column("id").data<std::int64_t>();
        for (int i = 0; i < 4; ++i) CHECK(ids[i] == 6 + i);
    }

    TEST_CASE(
        "a provider writing nothing into out_pushed gets No for every "
        "filter") {
        FixtureConfig cfg;
        cfg.filter_behavior = FilterBehavior::Ignore;
        auto source = make_fixture_source(cfg);

        ScanRequest req;
        req.filters.push_back(col(0) > std::int64_t{5});
        ScanResult r = source->scan(req);
        REQUIRE(r.filters.size() == 1);
        CHECK(r.filters[0] == Pushed::No);
        DataFrame raw = run(drain_cursor(std::move(r.cursor)));
        CHECK(raw.num_rows() == 10);  // unfiltered; the engine would re-apply

        DataFrame result = run(
            LazyFrame::scan(source).filter(col(0) > std::int64_t{5}).collect());
        CHECK(result.num_rows() == 4);
    }

    TEST_CASE("an out-of-range pushed value is treated as No, not trusted") {
        FixtureConfig cfg;
        cfg.filter_behavior = FilterBehavior::OutOfRange;
        auto source = make_fixture_source(cfg);

        ScanRequest req;
        req.filters.push_back(col(0) > std::int64_t{5});
        ScanResult r = source->scan(req);
        REQUIRE(r.filters.size() == 1);
        CHECK(r.filters[0] == Pushed::No);

        DataFrame result = run(
            LazyFrame::scan(source).filter(col(0) > std::int64_t{5}).collect());
        CHECK(result.num_rows() == 4);
    }

    TEST_CASE(
        "a provider honoring the projection returns exactly those "
        "columns in order") {
        FixtureConfig cfg;
        auto result = run(LazyFrame::scan(make_fixture_source(cfg))
                              .select({"val", "id"})
                              .collect());
        REQUIRE(result.names == std::vector<std::string>{"val", "id"});
        CHECK(result.column("id").data<std::int64_t>()[0] == 0);
    }

    TEST_CASE(
        "a provider ignoring the projection is caught, not passed "
        "downstream") {
        FixtureConfig cfg;
        cfg.honor_projection = false;
        CHECK_THROWS_AS(run(LazyFrame::scan(make_fixture_source(cfg))
                                .select({"val", "id"})
                                .collect()),
                        std::runtime_error);
    }

    TEST_CASE(
        "a false Exact claim (no actual filtering) produces wrong "
        "rows") {
        FixtureConfig cfg;
        cfg.filter_behavior = FilterBehavior::FalseExact;
        DataFrame result = run(LazyFrame::scan(make_fixture_source(cfg))
                                   .filter(col(0) > std::int64_t{5})
                                   .collect());
        // The engine trusted the claim and dropped the filter: all 10 rows
        // come back instead of the 4 matching id>5. There is no cheap runtime
        // check for a false Exact claim (safety rule c), so this is caught
        // only by a test asserting the provider and engine agree - see the
        // Exact-vs-No test above for the honest counterpart.
        CHECK(result.num_rows() == 10);
    }

    TEST_CASE(
        "limit: a provider honoring req.limit truncates; one ignoring "
        "it stays correct") {
        FixtureConfig honored;
        honored.apply_limit = true;
        ScanRequest req1;
        req1.limit = 3;
        DataFrame got1 = run(drain_cursor(
            std::move(make_fixture_source(honored)->scan(req1).cursor)));
        CHECK(got1.num_rows() == 3);

        FixtureConfig ignored;
        ignored.apply_limit = false;
        ScanRequest req2;
        req2.limit = 3;
        DataFrame got2 = run(drain_cursor(
            std::move(make_fixture_source(ignored)->scan(req2).cursor)));
        CHECK(got2.num_rows() == 10);
    }
}

namespace {

// Records the limit the optimizer put in the request, and yields a fixed frame.
class LimitRecordingSource : public dftracer::utils::dataframe::Source {
   public:
    mutable std::int64_t seen_limit = -2;  // -2 = scan() never ran

    dftracer::utils::dataframe::Schema schema() const override {
        return {{dftracer::utils::dataframe::Field{
            "id",
            dftracer::utils::dataframe::scalar(
                dftracer::utils::dataframe::TypeId::Unknown),
            true}}};
    }

    dftracer::utils::dataframe::ScanResult scan(
        const ScanRequest& req) const override {
        seen_limit = req.limit;
        dftracer::utils::dataframe::ScanResult r;
        r.cursor = std::make_unique<OneShot>();
        r.filters.assign(req.filters.size(),
                         dftracer::utils::dataframe::Pushed::No);
        return r;
    }

   private:
    struct OneShot : Cursor {
        bool done = false;
        CoroTask<std::optional<Morsel>> next(std::int64_t) override {
            if (done) co_return std::nullopt;
            done = true;
            std::vector<std::int64_t> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
            dftracer::utils::dataframe::Morsel m;
            m.rows = static_cast<std::int64_t>(v.size());
            m.columns.push_back(
                dftracer::utils::dataframe::Series{dftu_series_new_flat(
                    DFTU_TYPE_INT64, v.data(),
                    static_cast<std::int64_t>(v.size()), nullptr)});
            co_return m;
        }
    };
};

}  // namespace

TEST_SUITE("provider_pushdown") {
    TEST_CASE("the optimizer pushes a reachable slice as req.limit") {
        auto src = std::make_shared<LimitRecordingSource>();
        DataFrame got = run(LazyFrame::scan(src).head(3).collect());
        CHECK(got.num_rows() == 3);
        // head(3) is slice(0,3): the source only ever needed 3 rows.
        CHECK(src->seen_limit == 3);
    }

    TEST_CASE("a filter before the slice blocks the limit") {
        auto src = std::make_shared<LimitRecordingSource>();
        DataFrame got = run(LazyFrame::scan(src)
                                .filter(col(0) > std::int64_t{5})
                                .head(3)
                                .collect());
        // Whether the source honored the filter is only known from the result
        // it has not returned yet, so truncating it here could starve the
        // engine of rows the filter would have kept.
        CHECK(src->seen_limit == -1);
        CHECK(got.num_rows() == 3);  // 6,7,8 survive filter then head
    }

    TEST_CASE("a row-reordering op before the slice blocks the limit") {
        auto src = std::make_shared<LimitRecordingSource>();
        DataFrame got =
            run(LazyFrame::scan(src).sort_by("id", true).head(3).collect());
        CHECK(src->seen_limit == -1);  // sorting needs every row
        CHECK(got.num_rows() == 3);
    }
}

TEST_SUITE("provider_pushdown") {
    TEST_CASE("a slice window too large to sum leaves the limit unset") {
        auto src = std::make_shared<LimitRecordingSource>();
        // offset + len would overflow int64; unset means "produce everything",
        // which is what a window that large asked for anyway.
        DataFrame got =
            run(LazyFrame::scan(src)
                    .slice(std::numeric_limits<std::int64_t>::max(), 10)
                    .collect());
        CHECK(src->seen_limit == -1);
        CHECK(got.num_rows() == 0);  // the window starts past the last row
    }
}
