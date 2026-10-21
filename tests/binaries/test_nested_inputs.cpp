#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int EVENTS_PER_FILE = 20;

std::string trace(int file) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < EVENTS_PER_FILE; ++i)
        out << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX","pid":)"
            << file + 1 << R"(,"tid":1,"ph":"X","ts":)" << 1000 + i * 10
            << R"(,"dur":3,"args":{"fname":"/data/f)" << file << R"("}})"
            << "\n";
    out << "]\n";
    return out.str();
}

std::set<std::string> files_under(const std::string& dir) {
    std::set<std::string> out;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) out.insert(e.path().string());
    return out;
}

std::string tool(const char* env, const char* name) {
    const std::string path = dftu_utils_test::find_binary_by_name(env, name);
    REQUIRE_MESSAGE(!path.empty(), name << " not found");
    dftu_utils_test::set_test_library_path(path);
    return path;
}

int run(const std::string& binary, const std::vector<std::string>& args,
        std::string* out = nullptr) {
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
        const std::string in = env.get_dir() + "/in";
        const std::string work = env.get_dir() + "/work";
        const std::vector<std::string> dirs = {in + "/a", in + "/a/b",
                                               in + "/c/d/e"};
        for (std::size_t i = 0; i < dirs.size(); ++i) {
            fs::create_directories(dirs[i]);
            dftu_utils_test::write_gz_trace(
                dirs[i] + "/t" + std::to_string(i) + ".pfw.gz",
                trace(static_cast<int>(i)));
        }
        fs::create_directories(work);
        const auto inputs = files_under(in);
        REQUIRE(inputs.size() == dirs.size());
        const std::string total =
            std::to_string(EVENTS_PER_FILE * static_cast<int>(dirs.size()));

        std::string out;
        SUBCASE("dftracer_index") {
            CHECK(run(tool("DFTRACER_UTILS_INDEX_PATH", "dftracer_index"),
                      {"-d", in, "--index-dir", work + "/idx"}) == 0);
        }
        SUBCASE("dftracer_event_count") {
            CHECK(run(tool("DFTRACER_UTILS_EVENT_COUNT_PATH",
                           "dftracer_event_count"),
                      {"-d", in, "--index-dir", work + "/idx"}, &out) == 0);
            CHECK(out.find(total) != std::string::npos);
        }
        SUBCASE("dftracer_info") {
            CHECK(run(tool("DFTRACER_UTILS_INFO_PATH", "dftracer_info"),
                      {"-d", in, "--index-dir", work + "/idx"}, &out) == 0);
            CHECK(out.find("Total Files:        " +
                           std::to_string(dirs.size())) != std::string::npos);
            CHECK(out.find("Valid Events:       " + total) !=
                  std::string::npos);
        }
        SUBCASE("dftracer_stats") {
            CHECK(run(tool("DFTRACER_UTILS_STATS_PATH", "dftracer_stats"),
                      {"-d", in, "--index-dir", work + "/idx"}) == 0);
        }
        SUBCASE("dftracer_split") {
            CHECK(run(tool("DFTRACER_UTILS_SPLIT_PATH", "dftracer_split"),
                      {"-d", in, "-o", work + "/split", "--verify"}) == 0);
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
