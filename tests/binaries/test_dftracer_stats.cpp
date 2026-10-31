#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

std::string create_pfw_gz(dftu_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

std::string find_stats_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_STATS_PATH",
                                                "dftracer_stats");
}

int run_stats(const std::string& binary, const std::vector<std::string>& args) {
    return dftu_utils_test::run_process(binary, args);
}

std::string run_stats_capture(const std::string& binary,
                              const std::vector<std::string>& args,
                              int* exit_code = nullptr) {
    return dftu_utils_test::run_process_capture(binary, args, true, exit_code);
}

struct Fixture {
    std::map<std::string, std::uint64_t> categories;
    std::map<std::string, std::uint64_t> names;
    std::map<std::string, std::uint64_t> pid_tids;
    std::uint64_t events = 0;
    std::uint64_t dur_count = 0;
    std::uint64_t dur_sum = 0;
    std::uint64_t min_ts = UINT64_MAX;
    std::uint64_t max_end = 0;
};

// Writes files of 60, 120 and 180 events. Counts per category (3:2:1), name
// (4:3:2:1) and the presence of "dur" (three of four events) are fixed, so the
// expected report values follow from the loop below.
Fixture write_fixture(const std::string& dir) {
    static const char* const CATS[] = {"POSIX", "STDIO", "app"};
    static const int CAT_AT[] = {0, 0, 0, 1, 1, 2};
    static const char* const NAMES[] = {"read", "write", "open", "close"};
    static const int NAME_AT[] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 3};
    Fixture fx;
    for (int f = 0; f < 3; ++f) {
        const int count = 60 * (f + 1);
        std::string text = "[\n";
        for (int i = 0; i < count; ++i) {
            const char* cat = CATS[CAT_AT[i % 6]];
            const char* name = NAMES[NAME_AT[i % 10]];
            const int pid = 10 + (i % 2) + f * 5;
            const int tid = pid * 10 + (i % 3);
            const std::uint64_t ts = 1000000 + i * 100 + f * 50000;
            const bool has_dur = (i % 4) != 0;
            const std::uint64_t dur = has_dur ? 7 + (i % 5) * 3 : 0;
            text += R"({"id":)" + std::to_string(i) + R"(,"pid":)" +
                    std::to_string(pid) + R"(,"tid":)" + std::to_string(tid) +
                    R"(,"name":")" + name + R"(","cat":")" + cat +
                    R"(","ph":"X","ts":)" + std::to_string(ts);
            if (has_dur) text += R"(,"dur":)" + std::to_string(dur);
            text += "}\n";

            std::string lower = cat;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            ++fx.categories[lower];
            ++fx.names[name];
            ++fx.pid_tids[std::to_string(pid) + ":" + std::to_string(tid)];
            ++fx.events;
            if (has_dur) {
                ++fx.dur_count;
                fx.dur_sum += dur;
            }
            fx.min_ts = std::min(fx.min_ts, ts);
            fx.max_end = std::max(fx.max_end, ts + dur);
        }
        text += "]\n";
        const std::string path =
            dir + "/fixture_" + std::to_string(f) + ".pfw.gz";
        gzFile out = gzopen(path.c_str(), "wb");
        gzwrite(out, text.data(), static_cast<unsigned>(text.size()));
        gzclose(out);
    }
    return fx;
}

// The "    key   count" lines of a text report.
std::map<std::string, std::uint64_t> parse_counts(const std::string& output) {
    std::map<std::string, std::uint64_t> counts;
    std::istringstream in(output);
    std::string line;
    const std::regex row(R"(^    (\S+)\s+(\d+)$)");
    std::smatch m;
    while (std::getline(in, line))
        if (std::regex_match(line, m, row)) counts[m[1]] = std::stoull(m[2]);
    return counts;
}

std::map<std::string, std::uintmax_t> snapshot(const std::string& dir) {
    std::map<std::string, std::uintmax_t> files;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) files[e.path().string()] = e.file_size();
    return files;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerStats") {
    TEST_CASE("binary exists") {
        auto binary = find_stats_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_stats binary not found, skipping. "
                "Set DFTRACER_UTILS_STATS_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("basic stats from directory") {
        auto binary = find_stats_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_stats binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        // dftracer_stats auto-builds the .bidx when missing.
        int rc = 0;
        auto output = run_stats_capture(binary, {"-d", env.get_dir()}, &rc);
        CHECK(rc == 0);
        // Text summary always prints "Events Scanned:".
        CHECK(output.find("Events Scanned") != std::string::npos);
    }

    TEST_CASE("json output") {
        auto binary = find_stats_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_stats binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = 0;
        auto output =
            run_stats_capture(binary, {"-d", env.get_dir(), "--json"}, &rc);
        CHECK(rc == 0);
        // JSON output is a JSON array; each element is an object.
        CHECK(output.find('{') != std::string::npos);
        // The JSON object always contains "total_events".
        CHECK(output.find("total_events") != std::string::npos);
    }

    TEST_CASE("multiple files") {
        auto binary = find_stats_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_stats binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        for (int i = 0; i < 3; ++i) {
            auto f = create_pfw_gz(env, 30, i);
            REQUIRE(!f.empty());
        }

        int rc = run_stats(binary, {"-d", env.get_dir()});
        CHECK(rc == 0);
    }

    TEST_CASE("rebuilds stale index on changed source") {
        auto binary = find_stats_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_stats binary not found, skipping.");
            return;
        }

        auto total_events = [](const std::string& json) -> long {
            auto p = json.find("total_events");
            if (p == std::string::npos) return -1;
            p = json.find_first_of("0123456789", p);
            return p == std::string::npos ? -1 : std::atol(json.c_str() + p);
        };

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        int rc = 0;
        long n1 = total_events(
            run_stats_capture(binary, {"-d", env.get_dir(), "--json"}, &rc));
        CHECK(rc == 0);
        CHECK(n1 > 0);

        // Rewrite the same file with more events; the index is now stale.
        auto raw = env.create_dft_test_gzip_file(300);
        REQUIRE(!raw.empty());
        fs::remove(f);
        fs::rename(raw, f);

        long n2 = total_events(
            run_stats_capture(binary, {"-d", env.get_dir(), "--json"}, &rc));
        CHECK(rc == 0);
        CHECK(n2 > n1);
    }

    TEST_CASE("unfiltered reports answer from the per-file index rows") {
        auto binary = find_stats_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_stats binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        const Fixture fx = write_fixture(env.get_dir());

        int rc = 0;
        run_stats_capture(binary, {"-d", env.get_dir()}, &rc);
        REQUIRE(rc == 0);
        const auto before = snapshot(env.get_dir());

        auto report = [&](const std::string& name,
                          std::vector<std::string> extra = {}) {
            std::vector<std::string> args{"-d", env.get_dir(), "--report",
                                          name};
            args.insert(args.end(), extra.begin(), extra.end());
            auto out = run_stats_capture(binary, args, &rc);
            CHECK(rc == 0);
            return out;
        };

        CHECK(parse_counts(report("categories")) == fx.categories);
        CHECK(parse_counts(report("names")) == fx.names);
        CHECK(parse_counts(report("pid_tids")) == fx.pid_tids);

        const auto top_names =
            parse_counts(report("top-names", {"--top-n", "2"}));
        CHECK(top_names == std::map<std::string, std::uint64_t>{
                               {"read", fx.names.at("read")},
                               {"write", fx.names.at("write")}});
        const auto top_cats =
            parse_counts(report("top-categories", {"--top-n", "2"}));
        CHECK(top_cats == std::map<std::string, std::uint64_t>{
                              {"posix", fx.categories.at("posix")},
                              {"stdio", fx.categories.at("stdio")}});

        const auto summary = report("summary", {"--top-n-pid-tid", "0"});
        CHECK(summary.find("Events Scanned: " + std::to_string(fx.events)) !=
              std::string::npos);
        char span[64];
        std::snprintf(span, sizeof(span), "Time Span: %.6f seconds",
                      static_cast<double>(fx.max_end - fx.min_ts) / 1e6);
        CHECK(summary.find(span) != std::string::npos);
        auto summary_counts = parse_counts(summary);
        for (const auto& [k, v] : fx.categories) CHECK(summary_counts[k] == v);
        for (const auto& [k, v] : fx.pid_tids) CHECK(summary_counts[k] == v);

        const auto duration = report("duration");
        char dur_line[96];
        std::snprintf(dur_line, sizeof(dur_line), "Count: %llu   Mean: %.1f us",
                      static_cast<unsigned long long>(fx.dur_count),
                      static_cast<double>(fx.dur_sum) /
                          static_cast<double>(fx.dur_count));
        CHECK(duration.find(dur_line) != std::string::npos);

        // A warm unfiltered run neither folds nor writes: the index and the
        // rollup cache beside it keep the same files and sizes.
        CHECK(snapshot(env.get_dir()) == before);
        for (const auto& [path, size] : before)
            CHECK(path.find("-cache") == std::string::npos);
    }

    TEST_CASE("per-file json counts match the fixture") {
        auto binary = find_stats_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_stats binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        write_fixture(env.get_dir());

        int rc = 0;
        auto out = run_stats_capture(
            binary, {"-d", env.get_dir(), "--json", "--report", "categories"},
            &rc);
        CHECK(rc == 0);
        for (const char* total :
             {"\"total_events\":60,", "\"total_events\":120,",
              "\"total_events\":180,"})
            CHECK(out.find(total) != std::string::npos);
        CHECK(out.find("\"name\":\"posix\",\"count\":30") != std::string::npos);
    }
}
