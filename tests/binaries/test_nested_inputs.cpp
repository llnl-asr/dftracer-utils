// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <sstream>

using namespace dftracer::utils;

namespace {

constexpr int EVENTS_PER_FILE = 20;

dstd::String trace(int file) {
    dftu_utils_test::TextOut out;
    out << "[\n";
    for (int i = 0; i < EVENTS_PER_FILE; ++i)
        out << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX","pid":)"
            << file + 1 << R"(,"tid":1,"ph":"X","ts":)" << 1000 + i * 10
            << R"(,"dur":3,"args":{"fname":"/data/f)" << file << R"("}})"
            << "\n";
    out << "]\n";
    return out.str();
}

dstd::Set<dstd::String> files_under(const dstd::String& dir) {
    dstd::Set<dstd::String> out;
    for (const auto& e : dftu_utils_test::walk_dir(dir))
        if ((e.kind == dstd::fs::FileKind::File))
            out.insert(dstd::String(e.path.str()));
    return out;
}

dstd::String tool(const char* env, const char* name) {
    const dstd::String path = dftu_utils_test::find_binary_by_name(env, name);
    REQUIRE_MESSAGE(!path.empty(), name << " not found");
    dftu_utils_test::set_test_library_path(path);
    return path;
}

int run(const dstd::String& binary, const dstd::Vector<dstd::String>& args,
        dstd::String* out = nullptr) {
    int rc = -1;
    const auto text =
        dftu_utils_test::run_process_capture(binary, args, true, &rc);
    if (out) *out = text;
    if (rc != 0) MESSAGE(binary << " said: " << text);
    return rc;
}

}  // namespace

TEST_SUITE("NestedInputs") {
    TEST_CASE(
        "every tool reads the traces in the subfolders of -d and "
        "writes nothing beside them") {
        dftu_utils_test::TestEnvironment env(10);
        const dstd::String in = env.dir() + "/in";
        const dstd::String work = env.dir() + "/work";
        const dstd::Vector<dstd::String> dirs = {in + "/a", in + "/a/b",
                                                 in + "/c/d/e"};
        for (std::size_t i = 0; i < dirs.size(); ++i) {
            REQUIRE(dstd::fs::create_directories(dirs[i]).has_value());
            dftu_utils_test::write_gz_trace(
                dirs[i] + "/t" + dstd::to_string(i) + ".pfw.gz",
                trace(static_cast<int>(i)));
        }
        REQUIRE(dstd::fs::create_directories(work).has_value());
        const auto inputs = files_under(in);
        REQUIRE(inputs.size() == dirs.size());
        const dstd::String total =
            dstd::to_string(EVENTS_PER_FILE * static_cast<int>(dirs.size()));

        dstd::String out;
        SUBCASE("dftracer_index") {
            CHECK(run(tool("DFTRACER_UTILS_INDEX_PATH", "dftracer_index"),
                      {in, "--index-dir", work + "/idx"}) == 0);
        }
        SUBCASE("dftracer_event_count") {
            CHECK(run(tool("DFTRACER_UTILS_EVENT_COUNT_PATH",
                           "dftracer_event_count"),
                      {in, "--index-dir", work + "/idx"}, &out) == 0);
            CHECK(out.find(total) != dstd::String::npos);
        }
        SUBCASE("dftracer_info") {
            CHECK(run(tool("DFTRACER_UTILS_INFO_PATH", "dftracer_info"),
                      {in, "--index-dir", work + "/idx"}, &out) == 0);
            CHECK(out.find("Total Files:        " +
                           dstd::to_string(dirs.size())) != dstd::String::npos);
            CHECK(out.find("Valid Events:       " + total) !=
                  dstd::String::npos);
        }
        SUBCASE("dftracer_stats") {
            CHECK(run(tool("DFTRACER_UTILS_STATS_PATH", "dftracer_stats"),
                      {in, "--index-dir", work + "/idx"}) == 0);
        }
        SUBCASE("dftracer_split") {
            CHECK(run(tool("DFTRACER_UTILS_SPLIT_PATH", "dftracer_split"),
                      {in, "-o", work + "/split", "--verify"}) == 0);
            CHECK_FALSE(files_under(work + "/split").empty());
        }
        SUBCASE("dftracer_comparator") {
            CHECK(run(tool("DFTRACER_UTILS_COMPARATOR_PATH",
                           "dftracer_comparator"),
                      {"--baseline", in, "--variant", in,
                       "--baseline-index-dir", work + "/idx",
                       "--variant-index-dir", work + "/idx"}) == 0);
        }
        SUBCASE("dftracer_replay") {
            CHECK(run(tool("DFTRACER_UTILS_REPLAY_PATH", "dftracer_replay"),
                      {"--dry-run", "--dftracer-mode", "--no-sleep", in},
                      &out) == 0);
        }
        CHECK(files_under(in) == inputs);
    }
}
