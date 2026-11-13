// SPDX-License-Identifier: MIT
// A plugin callback that returns a negative status fails the fold with an
// error that names the plugin, the callback and the message the plugin set
// through dftu_fail. The same holds for a C++ Slice that returns an error,
// since the SDK trampolines turn it into a status and a message.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/runtime/runtime.h>

// After fold_adapter.h so nanoarrow is set up before dataframe/abi.h's
// arrow_abi.
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>
#include <task_result.h>

using namespace dftracer::utils;

namespace {

enum class At {
    MakeSlice,
    OnBatch,
    Merge,
    OnFinalize,
    StateInit,
    StateUpdate,
    StateMerge,
    StateFinalize
};

At g_at = At::OnBatch;

bool is(At at) { return g_at == at; }

dftu_status fail(dftu_ctx* ctx, const char* callback) {
    dstd::String msg = "refused in ";
    msg += callback;
    return dftu_fail(ctx, DFTU_E_PARSE, msg.c_str());
}

dftu_status make_slice(dftu_ctx* ctx, void*, void** out) {
    if (is(At::MakeSlice)) return fail(ctx, "make_slice");
    static int sentinel;
    *out = &sentinel;
    return DFTU_OK;
}
dftu_status on_batch(dftu_ctx* ctx, void*, const dftu_dataframe*,
                     const dftu_plugin_host*) {
    return is(At::OnBatch) ? fail(ctx, "on_batch") : DFTU_OK;
}
dftu_status merge(dftu_ctx* ctx, void*, void*) {
    return is(At::Merge) ? fail(ctx, "merge") : DFTU_OK;
}
dftu_status on_finalize(dftu_ctx* ctx, void*, const dftu_plugin_host*,
                        dftu_task**) {
    return is(At::OnFinalize) ? fail(ctx, "on_finalize") : DFTU_OK;
}

dftu_plugin make_c_plugin() {
    dftu_plugin p{};
    p.struct_size = sizeof(p);
    p.make_slice = make_slice;
    p.on_batch = on_batch;
    p.merge = merge;
    p.on_finalize = on_finalize;
    p.destroy_slice = [](void*) {};
    p.destroy = [](void*) {};
    return p;
}

dftu_status state_init(dftu_ctx* ctx, void*, void** out) {
    if (is(At::StateInit)) return fail(ctx, "state init");
    static int sentinel;
    *out = &sentinel;
    return DFTU_OK;
}
dftu_status state_update(dftu_ctx* ctx, void*, const dftu_dataframe*) {
    return is(At::StateUpdate) ? fail(ctx, "state update") : DFTU_OK;
}
dftu_status state_merge(dftu_ctx* ctx, void*, void*) {
    return is(At::StateMerge) ? fail(ctx, "state merge") : DFTU_OK;
}
dftu_status state_finalize(dftu_ctx* ctx, void*, dftu_result_value* out) {
    if (is(At::StateFinalize)) return fail(ctx, "state finalize");
    out->kind = DFTU_RESULT_KIND_BYTES;
    out->u.bytes.data = "ok";
    out->u.bytes.len = 2;
    return DFTU_OK;
}

dftu_state_desc make_state_desc() {
    dftu_state_desc d{};
    d.struct_size = sizeof(d);
    d.name = "cb_errors.state";
    d.init = state_init;
    d.update = state_update;
    d.merge = state_merge;
    d.finalize = state_finalize;
    d.destroy = [](void*) {};
    return d;
}

trace::views::detail::FoldEvent evt(dstd::StringIntern& intern) {
    trace::views::detail::FoldEvent fe;
    fe.cat_id = intern.get_or_insert("POSIX");
    fe.name_id = intern.get_or_insert("read");
    fe.phase = trace::RecordPhase::Complete;
    fe.ts = 1;
    fe.dur = 5;
    fe.has_dur = true;
    return fe;
}

// Runs one worker slice through step, merge and finalize and gives the first
// error it met ("" when none), whichever stage raised it.
dstd::String first_error(const dftu_plugin& p,
                         const plugins::host::StateRegistry* states) {
    dstd::StringIntern intern;
    plugins::NamedResultRegistry named;
    plugins::host::PluginFold master(&p, intern, nullptr, &named, "cb_errors",
                                     0, states);
    dstd::Vector<trace::views::detail::FoldEvent> evs{evt(intern)};
    auto slice = master.slice();
    auto* pf = static_cast<plugins::host::PluginFold*>(slice.get());
    trace::views::detail::ScanUnit unit{};
    auto stepped = pf->step(trace::views::detail::FoldBatch{
        dstd::Span<const trace::views::detail::FoldEvent>(evs), unit, {}});
    if (!stepped) return dstd::String(stepped.error().message());
    auto merged = master.merge(*pf);
    if (!merged) return dstd::String(merged.error().message());

    dstd::String message;
    Runtime rt(1);
    dftu_utils_test::ok(
        rt.submit(coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                      auto done = co_await master.finalize(
                          trace::views::detail::CoverageSet{});
                      if (!done.has_value())
                          message = dstd::String(done.error().message());
                  }),
                  "fin")
            .wait());
    rt.shutdown();
    return message;
}

bool names(const dstd::String& error, const char* callback,
           const char* detail) {
    return error.find("cb_errors") != dstd::String::npos &&
           error.find(callback) != dstd::String::npos &&
           error.find(detail) != dstd::String::npos;
}

}  // namespace

TEST_CASE("a clean run reports no error") {
    g_at = At::OnBatch;
    dftu_plugin p = make_c_plugin();
    p.on_batch = [](dftu_ctx*, void*, const dftu_dataframe*,
                    const dftu_plugin_host*) -> dftu_status { return DFTU_OK; };
    CHECK(first_error(p, nullptr).empty());
}

TEST_CASE("a failing make_slice names the plugin and callback") {
    g_at = At::MakeSlice;
    dftu_plugin p = make_c_plugin();
    const dstd::String e = first_error(p, nullptr);
    CHECK(names(e, "make_slice", "refused in make_slice"));
}

TEST_CASE("a failing on_batch names the plugin and callback") {
    g_at = At::OnBatch;
    dftu_plugin p = make_c_plugin();
    const dstd::String e = first_error(p, nullptr);
    CHECK(names(e, "on_batch", "refused in on_batch"));
}

TEST_CASE("a failing merge names the plugin and callback") {
    g_at = At::Merge;
    dftu_plugin p = make_c_plugin();
    const dstd::String e = first_error(p, nullptr);
    CHECK(names(e, "merge", "refused in merge"));
}

TEST_CASE("a failing on_finalize names the plugin and callback") {
    g_at = At::OnFinalize;
    dftu_plugin p = make_c_plugin();
    const dstd::String e = first_error(p, nullptr);
    CHECK(names(e, "on_finalize", "refused in on_finalize"));
}

TEST_CASE("a failing registered-state callback names the state and callback") {
    plugins::host::StateRegistry registry{plugins::host::RegisteredState{
        "cb_errors.state", make_state_desc(), nullptr}};
    dftu_plugin p = make_c_plugin();
    const struct {
        At at;
        const char* callback;
        const char* detail;
    } cases[] = {{At::StateInit, "init", "refused in state init"},
                 {At::StateUpdate, "update", "refused in state update"},
                 {At::StateMerge, "merge", "refused in state merge"},
                 {At::StateFinalize, "finalize", "refused in state finalize"}};
    for (const auto& c : cases) {
        CAPTURE(c.callback);
        g_at = c.at;
        const dstd::String e = first_error(p, &registry);
        CHECK(names(e, c.callback, c.detail));
        CHECK(e.find("cb_errors.state") != dstd::String::npos);
    }
}

namespace {

// A Slice that returns an error at the chosen stage.
struct FailingSlice {
    explicit FailingSlice(const plugins::Config&) {}
    static dstd::Result<FailingSlice> from_config(const plugins::Config& c) {
        if (is(At::MakeSlice))
            return plugins::make_error(DFTU_E_PARSE, "sdk failed in make");
        return FailingSlice(c);
    }
    dstd::Result<void> step(const dftu_dataframe*, plugins::Host) {
        if (is(At::OnBatch))
            return plugins::make_error(DFTU_E_PARSE, "sdk failed in step");
        return plugins::ok();
    }
    dstd::Result<void> merge(FailingSlice&) {
        if (is(At::Merge))
            return plugins::make_error(DFTU_E_PARSE, "sdk failed in merge");
        return plugins::ok();
    }
    dstd::Result<void> finalize(plugins::Host) {
        if (is(At::OnFinalize))
            return plugins::make_error(DFTU_E_PARSE, "sdk failed in finalize");
        return plugins::ok();
    }
};

struct SdkPlugin {
    dftu_plugin* p = plugins::make_plugin<FailingSlice>(nullptr);
    ~SdkPlugin() { p->destroy(p->self); }
};

}  // namespace

TEST_CASE("the SDK turns a returned error into an error with its text") {
    const struct {
        At at;
        const char* callback;
        const char* detail;
    } cases[] = {{At::MakeSlice, "make_slice", "sdk failed in make"},
                 {At::OnBatch, "on_batch", "sdk failed in step"},
                 {At::Merge, "merge", "sdk failed in merge"},
                 {At::OnFinalize, "on_finalize", "sdk failed in finalize"}};
    for (const auto& c : cases) {
        CAPTURE(c.callback);
        g_at = c.at;
        SdkPlugin sdk;
        const dstd::String e = first_error(*sdk.p, nullptr);
        CHECK(names(e, c.callback, c.detail));
    }
}

TEST_CASE("DFTRACER_UTILS_TRY in a step returns a failed service call") {
    struct TrySlice {
        explicit TrySlice(const plugins::Config&) {}
        dstd::Result<void> step(const dftu_dataframe*, plugins::Host h) {
            DFTRACER_UTILS_TRY(dftu_series * out,
                               h.run_op("dftu.test.nonexistent_op", {}));
            dftu_series_free(out);
            return plugins::ok();
        }
        void merge(TrySlice&) {}
        void finalize(plugins::Host) {}
    };
    dftu_plugin* p = plugins::make_plugin<TrySlice>(nullptr);
    const dstd::String e = first_error(*p, nullptr);
    CHECK(names(e, "on_batch", "dftu.test.nonexistent_op"));
    {
        dstd::StringIntern intern;
        plugins::NamedResultRegistry named;
        plugins::host::PluginFold master(p, intern, nullptr, &named,
                                         "cb_errors", 0, nullptr);
        dstd::Vector<trace::views::detail::FoldEvent> evs{evt(intern)};
        auto slice = master.slice();
        trace::views::detail::ScanUnit unit{};
        auto stepped = slice->step(trace::views::detail::FoldBatch{
            dstd::Span<const trace::views::detail::FoldEvent>(evs), unit, {}});
        REQUIRE_FALSE(stepped.has_value());
        CHECK(stepped.error().is(dstd::Condition::NotFound));
    }
    p->destroy(p->self);
}

TEST_CASE("an Error returned from a Slice keeps its status") {
    struct CodedSlice {
        explicit CodedSlice(const plugins::Config&) {}
        dstd::Result<void> step(const dftu_dataframe*, plugins::Host) {
            return plugins::make_error(DFTU_E_NOT_FOUND, "no such column");
        }
        void merge(CodedSlice&) {}
        void finalize(plugins::Host) {}
    };
    dftu_plugin* p = plugins::make_plugin<CodedSlice>(nullptr);
    const dstd::String e = first_error(*p, nullptr);
    CHECK(names(e, "on_batch", "no such column"));
    {
        dstd::StringIntern intern;
        plugins::NamedResultRegistry named;
        plugins::host::PluginFold master(p, intern, nullptr, &named,
                                         "cb_errors", 0, nullptr);
        dstd::Vector<trace::views::detail::FoldEvent> evs{evt(intern)};
        auto slice = master.slice();
        trace::views::detail::ScanUnit unit{};
        auto stepped = slice->step(trace::views::detail::FoldBatch{
            dstd::Span<const trace::views::detail::FoldEvent>(evs), unit, {}});
        REQUIRE_FALSE(stepped.has_value());
        CHECK(stepped.error().is(dstd::Condition::NotFound));
    }
    p->destroy(p->self);
}
