// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_PLUGIN_COMMON_H
#define DFTRACER_UTILS_TESTS_PLUGIN_COMMON_H
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/domains/plugins/plugins.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <variant>

namespace test_plugin_common {

// Indexes an already-written gzip trace, discarding the export output; the
// caller only needs the resulting ViewFile.
inline dftracer::utils::trace::views::ViewFile index_trace(
    const dftracer::utils::dstd::String& gz) {
    using View = dftracer::utils::trace::views::View;
    using ViewFile = dftracer::utils::trace::views::ViewFile;
    using ExportSink = dftracer::utils::trace::views::ExportSink;

    const dftracer::utils::dstd::String idx = dftracer::utils::dstd::String(
        dftracer::utils::trace::internal::determine_index_path(gz, ""));
    struct Sink : ExportSink {
        void write(dftracer::utils::dstd::StringView) override {}
    } sink;
    dftu_utils_test::ok(View::from_file(dftracer::utils::dstd::String(gz),
                                        dftracer::utils::dstd::String(idx))
                            .sink_json(sink)
                            .get());
    return ViewFile{dftracer::utils::dstd::String(gz),
                    dftracer::utils::dstd::String(idx)};
}

// Runs a built plugin set over `view` to completion and returns the result.
// `tag` is the runtime submit tag, surfaced in runtime logs.
inline dftracer::utils::domains::plugins::PluginRun run_set(
    const dftracer::utils::domains::plugins::Plugins& set,
    const dftracer::utils::trace::views::View& view,
    const char* tag = "plugin-run") {
    dftracer::utils::domains::plugins::PluginRun out;
    dftracer::utils::Runtime rt(2);
    auto task = dftracer::utils::coro::with_scope(
        [&](dftracer::utils::coro::Scope&)
            -> dftracer::utils::coro::CoroTask<void> {
            auto run = co_await set.run(view);
            REQUIRE(run.has_value());
            out = std::move(*run);
            co_return;
        });
    dftu_utils_test::ok(rt.submit(std::move(task), tag).wait());
    rt.shutdown();
    return out;
}

// Text bytes of a named result, or "(missing)" if `name` is not present.
inline dftracer::utils::dstd::String result_text(
    dftracer::utils::domains::plugins::PluginRun& run, const char* name) {
    auto it = run.results.results().find(name);
    if (it == run.results.results().end()) return "(missing)";
    const auto& bytes =
        std::get<dftracer::utils::dstd::Vector<std::byte>>(it->second);
    return dftracer::utils::dstd::String(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace test_plugin_common

#endif  // DFTRACER_UTILS_TESTS_PLUGIN_COMMON_H
