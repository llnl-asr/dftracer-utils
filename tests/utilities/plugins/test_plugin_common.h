#ifndef DFTRACER_TESTS_PLUGIN_COMMON_H
#define DFTRACER_TESTS_PLUGIN_COMMON_H
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>

#include <string>
#include <variant>
#include <vector>

namespace test_plugin_common {

// Indexes an already-written gzip trace, discarding the export output; the
// caller only needs the resulting ViewFile.
inline dftracer::utils::trace::views::ViewFile index_trace(
    const std::string& gz) {
    using dftracer::utils::trace::internal::determine_index_path;
    using View = dftracer::utils::trace::views::View;
    using ViewFile = dftracer::utils::trace::views::ViewFile;
    using ExportSink = dftracer::utils::trace::views::ExportSink;

    const std::string idx = determine_index_path(gz, "");
    struct Sink : ExportSink {
        void write(std::string_view) override {}
    } sink;
    View::from_file(gz, idx).metadata(false).export_json(sink).get();
    return ViewFile{gz, idx};
}

// Runs a built plugin set over `view` to completion and returns the result.
// `tag` is the runtime submit tag, surfaced in runtime logs.
inline dftracer::utils::plugins::PluginRun run_set(
    const dftracer::utils::plugins::Plugins& set,
    const dftracer::utils::trace::views::View& view,
    const char* tag = "plugin-run") {
    using dftracer::utils::CoroScope;
    using dftracer::utils::Runtime;
    namespace coro = dftracer::utils::coro;

    dftracer::utils::plugins::PluginRun out;
    Runtime rt(2);
    auto task = dftracer::utils::run_coro_scope(
        rt.executor(), [&](CoroScope&) -> coro::CoroTask<void> {
            auto run = co_await set.run(view);
            REQUIRE(run.has_value());
            out = std::move(*run);
            co_return;
        });
    rt.submit(std::move(task), tag).wait();
    rt.shutdown();
    return out;
}

// Text bytes of a named result, or "(missing)" if `name` is not present.
inline std::string result_text(dftracer::utils::plugins::PluginRun& run,
                               const char* name) {
    auto it = run.results.results().find(name);
    if (it == run.results.results().end()) return "(missing)";
    const auto& bytes = std::get<std::vector<std::byte>>(it->second);
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
}

}  // namespace test_plugin_common

#endif  // DFTRACER_TESTS_PLUGIN_COMMON_H
