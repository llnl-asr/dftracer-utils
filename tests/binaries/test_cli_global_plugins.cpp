// SPDX-License-Identifier: MIT
// DFTRACER_UTILS_PLUGINS loads plugins once for a tool, and the file format a
// plugin registers then reads the files it claims. A path that does not load
// stops the tool with an error that names it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdlib>

using namespace dftracer::utils;

namespace {

constexpr const char* PLUGINS_ENV = "DFTRACER_UTILS_PLUGINS";

dstd::String index_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_INDEX_PATH",
                                                "dftracer_index");
}

dstd::String chunked_plugin() {
    const auto path = common::Env::get("DFTRACER_UTILS_CHUNKED_FORMAT_PLUGIN");
    REQUIRE(path.has_value());
    return dstd::String(*path);
}

// A folder with one `rows.pfw.gz` that only the chunked format claims: it is
// not gzip and its magic line is not JSON, so the built-in reader would not
// read it.
struct Folder {
    dstd::Path root = dftu_utils_test::make_unique_test_path("cli_plugins");
    dstd::String data;
    dstd::String index;
    dstd::String file;

    Folder() {
        data = dstd::String((root / "data").str());
        index = dstd::String((root / "index").str());
        REQUIRE(dstd::fs::create_directories(dstd::PathView(data)).has_value());
        REQUIRE(
            dstd::fs::create_directories(dstd::PathView(index)).has_value());
        file = data + "/rows.pfw.gz";
        dstd::String text = "CHK1\n";
        for (int i = 1; i <= 8; ++i)
            text += dstd::to_string(i) + " n" + dstd::to_string(i) + "\n";
        REQUIRE(dstd::fs::write_file(dstd::PathView(file), text).has_value());
    }
};

dstd::String run_index(const Folder& folder, int& exit_code) {
    const dstd::String binary = index_binary();
    REQUIRE(!binary.empty());
    dftu_utils_test::set_test_library_path(binary);
    const dstd::Vector<dstd::String> args = {folder.data, "--index-dir",
                                             folder.index};
    return dftu_utils_test::run_process_capture(binary, args, true, &exit_code);
}

}  // namespace

TEST_CASE("a tool reads a file with the format DFTRACER_UTILS_PLUGINS loads") {
    Folder folder;
    ::setenv(PLUGINS_ENV, chunked_plugin().c_str(), 1);
    int code = -1;
    const dstd::String output = run_index(folder, code);
    ::unsetenv(PLUGINS_ENV);
    INFO(output.c_str());
    CHECK(code == 0);

    auto snapshot = dftu_utils_test::ok(trace::index::plan::open_snapshot(
        trace::internal::determine_index_path(folder.file, folder.index)));
    const auto file = snapshot.file(fileio::get_logical_path(folder.file));
    REQUIRE(file.has_value());
    auto record =
        dftu_utils_test::ok(trace::index::format_record(snapshot, *file));
    REQUIRE(record.has_value());
    CHECK(record->format == dstd::String("chunked_format.rows"));
}

TEST_CASE("a plugin path that does not load stops the tool and is named") {
    Folder folder;
    const dstd::String missing =
        dstd::String(folder.root.str()) + "/no_such_plugin.so";
    ::setenv(PLUGINS_ENV, missing.c_str(), 1);
    int code = -1;
    const dstd::String output = run_index(folder, code);
    ::unsetenv(PLUGINS_ENV);
    CHECK(code == 1);
    CHECK(output.find(missing.c_str()) != dstd::String::npos);
}

TEST_CASE("the same path twice, and empty items, load once") {
    Folder folder;
    const dstd::String plugin = chunked_plugin();
    const dstd::String list = plugin + "::" + plugin;
    ::setenv(PLUGINS_ENV, list.c_str(), 1);
    int code = -1;
    const dstd::String output = run_index(folder, code);
    ::unsetenv(PLUGINS_ENV);
    INFO(output.c_str());
    CHECK(code == 0);
}

namespace {

dstd::String binary_named(dstd::CStringView env, dstd::StringView name) {
    const dstd::String binary = dftu_utils_test::find_binary_by_name(env, name);
    REQUIRE(!binary.empty());
    dftu_utils_test::set_test_library_path(binary);
    return binary;
}

int exit_of(dstd::CStringView binary, dstd::Vector<dstd::String> args,
            dstd::String* output = nullptr) {
    int code = -1;
    const dstd::String text =
        dftu_utils_test::run_process_capture(binary, args, true, &code);
    if (output) *output = text;
    return code;
}

// A gzip trace named `name` in `folder`.
dstd::String add_trace(dftu_utils_test::TestEnvironment& env,
                       dstd::StringView folder, dstd::StringView name,
                       int events) {
    auto made = env.create_dft_test_gzip_file(events);
    REQUIRE(!made.empty());
    const dstd::String path = dstd::String(folder) + "/" + dstd::String(name);
    REQUIRE(dstd::fs::rename(made, path).has_value());
    return path;
}

}  // namespace

TEST_CASE(
    "a bad DFTRACER_UTILS_PLUGINS does not stop help, version or a tool "
    "that picks no reader") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    Folder folder;
    const dstd::String trace = add_trace(env, folder.data, "t.pfw.gz", 8);
    ::setenv(PLUGINS_ENV, "/no/such/plugin.so", 1);
    const dstd::String index = index_binary();
    const dstd::String stats =
        binary_named("DFTRACER_UTILS_STATS_PATH", "dftracer_stats");
    CHECK(exit_of(index, {"--help"}) == 0);
    CHECK(exit_of(index, {"--version"}) == 0);
    CHECK(exit_of(stats, {"--version"}) == 0);
    CHECK(exit_of(stats, {trace}) == 0);

    dstd::String output;
    CHECK(exit_of(index, {folder.data, "--index-dir", folder.index}, &output) ==
          1);
    CHECK(output.find("/no/such/plugin.so") != dstd::String::npos);
    ::unsetenv(PLUGINS_ENV);
}

TEST_CASE("a plugin the environment holds can be named again by a tool") {
    Folder folder;
    const dstd::String plugin = chunked_plugin();
    ::setenv(PLUGINS_ENV, plugin.c_str(), 1);
    dstd::String output;
    const int code =
        exit_of(index_binary(),
                {folder.data, "--index-dir", folder.index, "--plugin", plugin},
                &output);
    INFO(output.c_str());
    CHECK(code == 0);
    const dstd::String checker =
        binary_named("DFTRACER_UTILS_PLUGIN_PATH", "dftracer_plugin");
    CHECK(exit_of(checker, {"check", plugin}, &output) == 0);
    ::unsetenv(PLUGINS_ENV);
}

TEST_CASE("the summary of a mixed folder counts plugin and trace files") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    Folder folder;
    add_trace(env, folder.data, "t.pfw.gz", 8);
    ::setenv(PLUGINS_ENV, chunked_plugin().c_str(), 1);
    dstd::String output;
    const int code = exit_of(
        index_binary(), {folder.data, "--index-dir", folder.index}, &output);
    ::unsetenv(PLUGINS_ENV);
    INFO(output.c_str());
    CHECK(code == 0);
    CHECK(output.find("Files processed: 2") != dstd::String::npos);
    CHECK(output.find("Files failed: 0") != dstd::String::npos);
}
