// SPDX-License-Identifier: MIT
// The provider adapter mirrors the Source planning hooks over
// dftu_source_vt::apply. A hand-written vtable exercises the C marshaling and
// derived-source ownership.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/provider_source.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <type_traits>
#include <utility>

using namespace dftracer::utils;

namespace {

template <class T>
T run(coro::CoroTask<dstd::Result<T>> t) {
    T out = dftu_utils_test::ok(
        dftracer::utils::default_runtime().submit(std::move(t)).get());
    if constexpr (std::is_same_v<T, dataframe::DataFrame>)
        for (dataframe::Series& c : out.columns) c = c.materialize();
    return out;
}

dstd::Vector<std::int64_t> ints(const dataframe::DataFrame& df,
                                const dstd::String& name) {
    const dataframe::Series& c = df.column(name);
    const std::int64_t* p = c.data<std::int64_t>();
    return dstd::Vector<std::int64_t>(p, p + c.length());
}

}  // namespace

namespace {

// C fixture: 10 rows id 0..9, val = id*10. apply() answers per ApplyFixture.
struct ApplyFixture {
    std::int32_t answer = DFTU_APPLY_EXACT;
    std::int64_t offset = 0;
    std::int64_t limit = -1;
    int derived_alive = 0;
    int derived_destroyed = 0;
    dstd::Vector<dftu_apply_request> requests;
    dstd::Vector<dstd::String> key_names;
    dstd::Vector<dstd::String> agg_ops;
    dstd::Vector<bool> agg_has_input;
    bool join_is_ours = false;
    dstd::Vector<dstd::String> join_on;
    std::int32_t join_how = -1;
    dstd::String join_suffix;
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
    dstd::Vector<std::int64_t> ids, vals;
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

dftu_status c_schema(dftu_ctx*, void*, const char* const** out_names,
                     std::int32_t* out_count) {
    static const char* names[] = {"id", "val"};
    *out_names = names;
    *out_count = 2;
    return DFTU_OK;
}

dftu_status c_next(dftu_ctx*, void* self, std::int64_t, dftu_dataframe** out,
                   dftu_task** pending) {
    auto* cs = static_cast<CursorState*>(self);
    *pending = nullptr;
    *out = cs->frame;
    cs->frame = nullptr;
    return *out ? DFTU_OK : DFTU_END;
}

void c_cursor_destroy(void* self) {
    auto* cs = static_cast<CursorState*>(self);
    if (cs->frame) dftu_dataframe_free(cs->frame);
    delete cs;
}

const dftu_cursor_vt C_CURSOR_VT = {sizeof(dftu_cursor_vt), c_next,
                                    c_cursor_destroy, nullptr};

dftu_status open_cursor(dftu_dataframe* df, void** out_cursor_self,
                        const dftu_cursor_vt** out_vt) {
    auto* cs = new CursorState{df};
    *out_cursor_self = cs;
    *out_vt = &C_CURSOR_VT;
    return DFTU_OK;
}

dftu_status c_root_scan(dftu_ctx*, void*, const dftu_plugin_host*,
                        const dftu_scan_request*, std::int32_t*,
                        void** out_cursor_self, const dftu_cursor_vt** out_vt) {
    return open_cursor(make_rows(0, -1), out_cursor_self, out_vt);
}

dftu_status c_derived_scan(dftu_ctx*, void* self, const dftu_plugin_host*,
                           const dftu_scan_request*, std::int32_t*,
                           void** out_cursor_self,
                           const dftu_cursor_vt** out_vt) {
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

dftu_status c_apply(dftu_ctx*, void* self, const dftu_apply_request* req,
                    dftu_apply_result* out);

const dftu_source_vt DERIVED_VT = {
    sizeof(dftu_source_vt), c_schema, c_derived_scan,
    c_derived_destroy,      nullptr,  nullptr};

dftu_status c_apply(dftu_ctx*, void* self, const dftu_apply_request* req,
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
            fx->join_on.push_back(
                dstd::fmt<"{}={}">(j.left_on[i], j.right_on[i]));
        fx->join_how = j.how;
        fx->join_suffix = j.suffix;
    }
    if (fx->answer == DFTU_APPLY_NO_CHANGE) return DFTU_OK;
    const bool limit = req->kind == DFTU_APPLY_LIMIT;
    auto* d = new Derived{fx, limit ? req->u.limit.offset : 0,
                          limit ? req->u.limit.n : -1};
    ++fx->derived_alive;
    out->status = fx->answer;
    out->self = d;
    out->vt = &DERIVED_VT;
    return DFTU_OK;
}

const dftu_source_vt ROOT_VT = {sizeof(dftu_source_vt), c_schema, c_root_scan,
                                c_root_destroy,         nullptr,  c_apply};

dataframe::LazyFrame c_frame(ApplyFixture& fx) {
    return dataframe::LazyFrame::scan(
        plugins::host::make_provider_source(ROOT_VT, &fx));
}

}  // namespace

TEST_SUITE("source_hooks_c_abi") {
    TEST_CASE("an exact limit reaches the derived source and frees it once") {
        ApplyFixture fx;
        {
            dataframe::LazyFrame lf = c_frame(fx).slice(2, 3);
            dataframe::DataFrame r = run(lf.collect());
            CHECK(ints(r, "id") == dstd::Vector<std::int64_t>{2, 3, 4});
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
        dstd::UniquePtr<dataframe::Cursor> cur;
        {
            dataframe::LazyFrame lf = c_frame(fx).slice(0, 4);
            cur = dftu_utils_test::ok(lf.open_cursor());
        }
        CHECK(fx.derived_alive == 1);
        cur.reset();
        CHECK(fx.derived_alive == 0);
    }

    TEST_CASE("an inexact answer to a projection is refused and freed") {
        ApplyFixture fx;
        fx.answer = DFTU_APPLY_INEXACT;
        dataframe::LazyFrame lf = c_frame(fx).select({"val"});
        const dstd::String plan = lf.explain();
        CHECK(plan.find("select [val]") != dstd::String::npos);
        CHECK(fx.derived_alive == 0);
    }

    TEST_CASE("an unknown status reads as no change and is freed") {
        ApplyFixture fx;
        fx.answer = 42;
        dataframe::LazyFrame lf = c_frame(fx).slice(0, 2);
        CHECK(lf.explain().find("slice") != dstd::String::npos);
        CHECK(fx.derived_alive == 0);
        CHECK(ints(run(lf.collect()), "id") ==
              dstd::Vector<std::int64_t>{0, 1});
    }

    TEST_CASE("an aggregation marshals named keys and optional inputs") {
        ApplyFixture fx;
        fx.answer = DFTU_APPLY_NO_CHANGE;
        dataframe::LazyFrame lf =
            c_frame(fx).group_by(dstd::Vector<dstd::String>{"id"},
                                 {{dataframe::Agg::Count, "", "n"},
                                  {dataframe::Agg::Sum, "val", "s"}});
        lf.explain();
        CHECK(fx.key_names == dstd::Vector<dstd::String>{"id"});
        CHECK(fx.agg_ops == dstd::Vector<dstd::String>{"count", "sum"});
        CHECK(fx.agg_has_input == dstd::Vector<bool>{false, true});
    }

    TEST_CASE("tail marshals its row count") {
        ApplyFixture fx;
        fx.answer = DFTU_APPLY_NO_CHANGE;
        c_frame(fx).tail(3).explain();
        REQUIRE(fx.requests.size() == 1);
        CHECK(fx.requests[0].kind == DFTU_APPLY_TAIL);
        CHECK(fx.requests[0].u.tail.n == 3);
    }

    TEST_CASE("a join marshals the other source and keeps it alive") {
        ApplyFixture fx;
        dstd::UniquePtr<dataframe::Cursor> cur;
        {
            dataframe::LazyFrame right = c_frame(fx).slice(0, 5);
            dataframe::LazyFrame lf =
                c_frame(fx).join(right, {"id"}, dataframe::JoinHow::Left, "_r");
            cur = dftu_utils_test::ok(lf.open_cursor());
        }
        CHECK(fx.join_is_ours);
        CHECK(fx.join_on == dstd::Vector<dstd::String>{"id=id"});
        CHECK(fx.join_how == DFTU_JOIN_LEFT);
        CHECK(fx.join_suffix == "_r");
        // The limit-derived right side and the join-derived source.
        CHECK(fx.derived_alive == 2);
        cur.reset();
        CHECK(fx.derived_alive == 0);
    }

    TEST_CASE("a source without apply keeps the ScanRequest path") {
        ApplyFixture fx;
        const dftu_source_vt vt = {
            sizeof(dftu_source_vt), c_schema, c_root_scan,
            c_root_destroy,         nullptr,  nullptr};
        dataframe::LazyFrame lf =
            dataframe::LazyFrame::scan(
                plugins::host::make_provider_source(vt, &fx))
                .slice(1, 2);
        CHECK(lf.explain().find("slice") != dstd::String::npos);
        CHECK(ints(run(lf.collect()), "id") ==
              dstd::Vector<std::int64_t>{1, 2});
        CHECK(fx.requests.empty());
    }
}

namespace {

// A source that derives another from every op offered, to see the order its
// sources are destroyed in. Node 0 is the root, which the adapter never owns.
struct ChainFixture {
    dstd::Vector<int> destroyed;
    int next_id = 1;
};

struct ChainNode {
    ChainFixture* fx;
    int id;
};

dftu_status chain_scan(dftu_ctx* ctx, void*, const dftu_plugin_host*,
                       const dftu_scan_request*, std::int32_t*, void**,
                       const dftu_cursor_vt**) {
    return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "chain source does not scan");
}

void chain_destroy(void* self) {
    auto* node = static_cast<ChainNode*>(self);
    node->fx->destroyed.push_back(node->id);
    delete node;
}

dftu_status chain_apply(dftu_ctx*, void* self, const dftu_apply_request*,
                        dftu_apply_result* out);

const dftu_source_vt CHAIN_VT = {sizeof(dftu_source_vt), c_schema, chain_scan,
                                 chain_destroy,          nullptr,  chain_apply};

dftu_status chain_apply(dftu_ctx*, void* self, const dftu_apply_request*,
                        dftu_apply_result* out) {
    auto* node = static_cast<ChainNode*>(self);
    out->status = DFTU_APPLY_EXACT;
    out->self = new ChainNode{node->fx, node->fx->next_id++};
    out->vt = &CHAIN_VT;
    return DFTU_OK;
}

}  // namespace

TEST_SUITE("source_hooks_destroy_order") {
    TEST_CASE("a derived source is destroyed before the source it came from") {
        ChainFixture fx;
        ChainNode root{&fx, 0};
        {
            dataframe::LazyFrame lf =
                dataframe::LazyFrame::scan(
                    plugins::host::make_provider_source(CHAIN_VT, &root))
                    .slice(0, 5)
                    .tail(2);
            lf.explain();
        }
        // Node 2 was derived from node 1 (the limit, then the tail); the plan
        // drops both once the planner is done with them.
        CHECK(fx.destroyed == dstd::Vector<int>{2, 1});
    }
}
