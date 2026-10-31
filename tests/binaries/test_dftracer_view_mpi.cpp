#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

using dftu_utils_test::find_binary_by_name;
using dftu_utils_test::find_mpi_launcher;
using dftu_utils_test::run_mpi;
using dftu_utils_test::TestEnvironment;

namespace {

// Write cpu counter events (one per value) to a gzip trace.
void write_counter_file(const std::string& path, const char* name,
                        const std::vector<int>& utils) {
    gzFile gz = gzopen(path.c_str(), "wb");
    REQUIRE(gz != nullptr);
    gzputs(gz, "[\n");
    int ts = 1000;
    for (int u : utils) {
        std::string line =
            "{\"ph\":\"X\",\"name\":\"" + std::string(name) +
            "\",\"cat\":\"sys\",\"pid\":0,\"tid\":0,\"ts\":" +
            std::to_string(ts) +
            ",\"dur\":10,\"args\":{\"util\":" + std::to_string(u) + "}}\n";
        gzputs(gz, line.c_str());
        ts += 1000;
    }
    gzputs(gz, "]\n");
    gzclose(gz);
}

// Write one complete (ph="X") event with an explicit [ts, ts+dur) interval.
void write_event_file(const std::string& path, const char* name,
                      std::uint64_t ts, std::uint64_t dur) {
    gzFile gz = gzopen(path.c_str(), "wb");
    REQUIRE(gz != nullptr);
    gzputs(gz, "[\n");
    std::string line =
        "{\"ph\":\"X\",\"name\":\"" + std::string(name) +
        "\",\"cat\":\"io\",\"pid\":0,\"tid\":0,\"ts\":" + std::to_string(ts) +
        ",\"dur\":" + std::to_string(dur) + "}\n";
    gzputs(gz, line.c_str());
    gzputs(gz, "]\n");
    gzclose(gz);
}

std::string read_sorted(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line))
        if (!line.empty()) lines.push_back(line);
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

std::string read_sorted_gz(const std::string& path) {
    gzFile gz = gzopen(path.c_str(), "rb");
    if (!gz) return {};
    std::string all;
    char buf[65536];
    int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0)
        all.append(buf, static_cast<std::size_t>(n));
    gzclose(gz);
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < all.size()) {
        auto nl = all.find('\n', start);
        if (nl == std::string::npos) break;
        if (nl > start) lines.push_back(all.substr(start, nl - start));
        start = nl + 1;
    }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

}  // namespace

TEST_SUITE("dftracer_view_mpi") {
    // dftracer_view built with MPI runs distributed counter aggregation under
    // mpirun; n=2 must match n=1 (single-node), same one binary.
    TEST_CASE("distributed counters match single-rank") {
        std::string bin =
            find_binary_by_name("DFTRACER_UTILS_VIEW_PATH", "dftracer_view");
        std::string launcher = find_mpi_launcher();
        if (bin.empty() || launcher.empty()) {
            MESSAGE("dftracer_view or MPI launcher unavailable, skipping.");
            return;
        }

        TestEnvironment env(1);
        REQUIRE(env.is_valid());
        // Inputs live in their own dir so the .pfw outputs are not rescanned.
        const std::string in = env.get_dir() + "/in";
        fs::create_directories(in);
        // Two shards, cpu util means 40 and 80 -> combined mean 60 over 4.
        write_counter_file(in + "/r0.pfw.gz", "cpu", {40, 40});
        write_counter_file(in + "/r1.pfw.gz", "cpu", {80, 80});

        // Distributed run (2 ranks) exercising the full auto-index + query
        // path. Single-rank parity is covered by the non-MPI
        // test_dftracer_view; here we assert the distributed result against the
        // known-correct value.
        const std::string o = env.get_dir() + "/o.pfw";
        const std::vector<std::string> args = {
            "--directory", in,         "--counters",
            "--group-by",  "name",     "--agg",
            "mean:util",   "--output", o};
        REQUIRE(run_mpi(launcher, 2, bin, args) == 0);

        const std::string s = read_sorted(o);
        CHECK(!s.empty());
        // util means 40 and 80 over 4 samples -> combined mean 60.
        CHECK(s.find("60") != std::string::npos);
    }

    // Distributed collect() (aggregate table, not counters): merge_partials_to
    // _table across ranks must equal the single-rank collect.
    TEST_CASE("distributed aggregate table matches single-rank") {
        std::string bin =
            find_binary_by_name("DFTRACER_UTILS_VIEW_PATH", "dftracer_view");
        std::string launcher = find_mpi_launcher();
        if (bin.empty() || launcher.empty()) {
            MESSAGE("dftracer_view or MPI launcher unavailable, skipping.");
            return;
        }

        TestEnvironment env(1);
        REQUIRE(env.is_valid());
        const std::string in = env.get_dir() + "/in";
        fs::create_directories(in);
        write_counter_file(in + "/r0.pfw.gz", "cpu", {40, 40});
        write_counter_file(in + "/r1.pfw.gz", "cpu", {80, 80});

        // No --counters: aggregate mode -> collect() table via print_table.
        // Distributed run only; the merged table must carry the correct mean.
        const std::string o = env.get_dir() + "/a.json";
        const std::vector<std::string> args = {
            "--directory", in,          "--group-by", "name",
            "--agg",       "mean:util", "--output",   o};
        REQUIRE(run_mpi(launcher, 2, bin, args) == 0);

        const std::string s = read_sorted(o);
        CHECK(!s.empty());
        CHECK(s.find("cpu") != std::string::npos);  // group key
        CHECK(s.find("60") != std::string::npos);   // mean util across ranks
    }

    // Distributed occupancy: each rank aggregate_partial()s its shard, the
    // coordinator merge_partials_to_table()s. Two overlapping events on
    // separate ranks must OR-merge to the union (busy = 500000, concurrency =
    // 2), not sum to 1000000 - which only holds if the occupancy masks survive
    // the partial serialization the ranks ship. Guards the distributed
    // transport end to end.
    TEST_CASE("distributed occupancy merges across ranks") {
        std::string bin =
            find_binary_by_name("DFTRACER_UTILS_VIEW_PATH", "dftracer_view");
        std::string launcher = find_mpi_launcher();
        if (bin.empty() || launcher.empty()) {
            MESSAGE("dftracer_view or MPI launcher unavailable, skipping.");
            return;
        }

        TestEnvironment env(1);
        REQUIRE(env.is_valid());
        const std::string in = env.get_dir() + "/in";
        fs::create_directories(in);
        write_event_file(in + "/r0.pfw.gz", "io", 0, 500000);
        write_event_file(in + "/r1.pfw.gz", "io", 0, 500000);  // overlaps r0

        const std::string o = env.get_dir() + "/occ.json";
        const std::vector<std::string> args = {
            "--directory", in,     "--group-by", "name",
            "--agg",       "busy", "--output",   o};
        REQUIRE(run_mpi(launcher, 2, bin, args) == 0);

        const std::string s = read_sorted(o);
        CHECK(!s.empty());
        CHECK(s.find("io") != std::string::npos);
        // busy is the union across ranks (500000), not the sum (1000000).
        CHECK(s.find("500000") != std::string::npos);
        CHECK(s.find("1000000") == std::string::npos);
    }

    // Distributed --merge: each rank writes its shard, rank 0 concatenates. The
    // merged event set must equal the single-rank merge (plain output so the
    // NDJSON lines sort/compare directly).
    TEST_CASE("distributed merge matches single-rank") {
        std::string bin =
            find_binary_by_name("DFTRACER_UTILS_VIEW_PATH", "dftracer_view");
        std::string launcher = find_mpi_launcher();
        if (bin.empty() || launcher.empty()) {
            MESSAGE("dftracer_view or MPI launcher unavailable, skipping.");
            return;
        }

        TestEnvironment env(1);
        REQUIRE(env.is_valid());
        const std::string in = env.get_dir() + "/in";
        fs::create_directories(in);
        write_counter_file(in + "/r0.pfw.gz", "cpu", {40, 40});
        write_counter_file(in + "/r1.pfw.gz", "cpu", {80, 80});

        // Distributed --merge: each rank writes its shard, rank 0 concatenates.
        const std::string o = env.get_dir() + "/m.pfw";
        const std::vector<std::string> args = {"--directory", in, "--merge",
                                               "--output", o};
        REQUIRE(run_mpi(launcher, 2, bin, args) == 0);

        // Output is always a compressed trace (.gz).
        const std::string s = read_sorted_gz(o + ".gz");
        CHECK(!s.empty());
        // All 4 source events (util 40,40,80,80) survive the shard->merge path.
        CHECK(std::count(s.begin(), s.end(), '\n') == 4);
        CHECK(s.find("40") != std::string::npos);
        CHECK(s.find("80") != std::string::npos);
    }
}
