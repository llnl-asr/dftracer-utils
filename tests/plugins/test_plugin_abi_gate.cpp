// SPDX-License-Identifier: MIT
// A plugin built against an incompatible plugin ABI, with an info or plugin
// descriptor that is too small, or with no info symbol must be refused with a
// clear error at load, never crash. A plugin whose descriptor stops before the
// last optional callback still loads and runs; the host reads the missing
// callback as NULL.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/plugin_abi.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <plugins/test_plugin_common.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;

using View = dftracer::utils::trace::views::View;
using ViewFile = dftracer::utils::trace::views::ViewFile;

#ifndef BAD_ABI_VERSION_PLUGIN_PATH
#error "BAD_ABI_VERSION_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef NO_INFO_PLUGIN_PATH
#error "NO_INFO_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef SHORT_PLUGIN_PATH
#error "SHORT_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef TOO_SMALL_PLUGIN_PATH
#error "TOO_SMALL_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef SHORT_INFO_PLUGIN_PATH
#error "SHORT_INFO_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef OVERSIZE_PLUGIN_PATH
#error "OVERSIZE_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef OVERSIZE_UDF_PLUGIN_PATH
#error "OVERSIZE_UDF_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

constexpr int EVENTS = 64;

bool contains(dstd::StringView text, dstd::StringView part) {
    return text.find(part) != dstd::String::npos;
}

std::uint32_t pack(std::uint32_t major, std::uint32_t minor,
                   std::uint32_t patch) {
    return (major << 16) | (minor << 8) | patch;
}

ViewFile make_trace(dftu_utils_test::TestEnvironment& env,
                    dstd::CStringView tag) {
    const dstd::String dir = env.dir() + "/" + tag.c_str();
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw = dir + "/trace.pfw";
    dstd::String ofs;
    for (int i = 0; i < EVENTS; ++i)
        dstd::fmt_append<
            "{\"ph\":1,\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":1,"
            "\"tid\":1,\"ts\":{},\"dur\":{}{}\n">(ofs, (1000 + i * 100),
                                                  (10 + i), R"(,"args":{}})");
    dstd::fs::write_file(dstd::PathView(pfw), ofs).discard();
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    return test_plugin_common::index_trace(gz);
}

}  // namespace

TEST_CASE("abi_compatible follows the major.minor rule") {
    using plugins::host::abi_compatible;

    CHECK(abi_compatible(pack(0, 3, 0), pack(0, 3, 0)));
    CHECK(abi_compatible(pack(0, 3, 0), pack(0, 1, 0)));
    CHECK_FALSE(abi_compatible(pack(0, 1, 0), pack(0, 3, 0)));
    CHECK_FALSE(abi_compatible(pack(1, 0, 0), pack(0, 9, 0)));
    CHECK(abi_compatible(pack(1, 2, 3), pack(1, 2, 9)));
}

TEST_CASE("a plugin stamped with a mismatched ABI version is refused") {
    auto set = plugins::host::Plugins::builder()
                   .add(BAD_ABI_VERSION_PLUGIN_PATH)
                   .build();

    REQUIRE_FALSE(set.has_value());
    const dstd::String message(set.error().message());
    CHECK(contains(message, "plugin ABI version"));
    CHECK(contains(message, BAD_ABI_VERSION_PLUGIN_PATH));
    CHECK(contains(message, "major must match"));
    CHECK_FALSE(contains(message, "the loader must not get here"));
}

TEST_CASE("a plugin with no info symbol is refused") {
    auto set =
        plugins::host::Plugins::builder().add(NO_INFO_PLUGIN_PATH).build();

    REQUIRE_FALSE(set.has_value());
    const dstd::String message(set.error().message());
    CHECK(contains(message, "dftracer_plugin_info"));
    CHECK(contains(message, NO_INFO_PLUGIN_PATH));
    CHECK_FALSE(contains(message, "the loader must not get here"));
}

TEST_CASE("a plugin descriptor below the first published size is refused") {
    auto set =
        plugins::host::Plugins::builder().add(TOO_SMALL_PLUGIN_PATH).build();

    REQUIRE_FALSE(set.has_value());
    const dstd::String message(set.error().message());
    CHECK(contains(message, "dftu_plugin struct_size"));
    CHECK(contains(message, "outside"));
    CHECK(contains(message, TOO_SMALL_PLUGIN_PATH));
}

TEST_CASE("a plugin whose descriptor omits the last optional callback runs") {
    auto set = plugins::host::Plugins::builder().add(SHORT_PLUGIN_PATH).build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    View view = View::from_files({make_trace(env, "short")});
    plugins::host::PluginRun run =
        test_plugin_common::run_set(*set, view, "short-plugin");

    auto it = run.results.results().find("seen_rows");
    REQUIRE(it != run.results.results().end());
    const auto& bytes = std::get<dstd::Vector<std::byte>>(it->second);
    REQUIRE(bytes.size() == sizeof(std::int64_t));
    std::int64_t rows = 0;
    std::memcpy(&rows, bytes.data(), sizeof(rows));
    CHECK(rows == EVENTS);
}

TEST_CASE("a plugin whose info record omits the version field loads") {
    auto set =
        plugins::host::Plugins::builder().add(SHORT_INFO_PLUGIN_PATH).build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    CHECK(set.has_value());
}

TEST_CASE("a plugin descriptor larger than the host's is refused") {
    auto set =
        plugins::host::Plugins::builder().add(OVERSIZE_PLUGIN_PATH).build();

    REQUIRE_FALSE(set.has_value());
    const dstd::String message(set.error().message());
    CHECK(contains(message, "dftu_plugin struct_size"));
    CHECK(contains(message, "outside"));
    CHECK(contains(message, OVERSIZE_PLUGIN_PATH));
}

TEST_CASE("a UDF descriptor larger than the host's is refused") {
    auto set =
        plugins::host::Plugins::builder().add(OVERSIZE_UDF_PLUGIN_PATH).build();

    REQUIRE_FALSE(set.has_value());
    const dstd::String message(set.error().message());
    CHECK(contains(message, "oversize_udf"));
    CHECK(contains(message, "struct_size"));
    CHECK(contains(message, "outside"));
    CHECK(contains(message, OVERSIZE_UDF_PLUGIN_PATH));
}
