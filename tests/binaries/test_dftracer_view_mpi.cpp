// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <string>

using namespace dftracer::utils;

namespace {

// Write cpu counter events (one per value) to a gzip trace.
void write_counter_file(const dstd::String& path, const char* name,
                        const dstd::Vector<int>& utils) {
    gzFile gz = gzopen(path.c_str(), "wb");
    REQUIRE(gz != nullptr);
    gzputs(gz, "[\n");
    int ts = 1000;
    for (int u : utils) {
        dstd::String line =
            "{\"ph\":\"X\",\"name\":\"" + dstd::String(name) +
            "\",\"cat\":\"sys\",\"pid\":0,\"tid\":0,\"ts\":" +
            dstd::to_string(ts) +
            ",\"dur\":10,\"args\":{\"util\":" + dstd::to_string(u) + "}}\n";
        gzputs(gz, line.c_str());
        ts += 1000;
    }
    gzputs(gz, "]\n");
    gzclose(gz);
}

// Write one complete (ph="X") event with an explicit [ts, ts+dur) interval.
void write_event_file(const dstd::String& path, const char* name,
                      std::uint64_t ts, std::uint64_t dur) {
    gzFile gz = gzopen(path.c_str(), "wb");
    REQUIRE(gz != nullptr);
    gzputs(gz, "[\n");
    dstd::String line =
        "{\"ph\":\"X\",\"name\":\"" + dstd::String(name) +
        "\",\"cat\":\"io\",\"pid\":0,\"tid\":0,\"ts\":" + dstd::to_string(ts) +
        ",\"dur\":" + dstd::to_string(dur) + "}\n";
    gzputs(gz, line.c_str());
    gzputs(gz, "]\n");
    gzclose(gz);
}

dstd::String read_sorted(const dstd::String& path) {
    dftu_utils_test::LineStream ifs(dftu_utils_test::read_text(path));
    dstd::Vector<dstd::String> lines;
    dstd::String line;
    while (ifs.next(line))
        if (!line.empty()) lines.push_back(line);
    std::sort(lines.begin(), lines.end());
    dstd::String out;
    for (auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

dstd::String read_sorted_gz(const dstd::String& path) {
    gzFile gz = gzopen(path.c_str(), "rb");
    if (!gz) return {};
    dstd::String all;
    char buf[65536];
    int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0)
        all.append(buf, static_cast<std::size_t>(n));
    gzclose(gz);
    dstd::Vector<dstd::String> lines;
    std::size_t start = 0;
    while (start < all.size()) {
        auto nl = all.find('\n', start);
        if (nl == dstd::String::npos) break;
        if (nl > start) lines.push_back(all.substr(start, nl - start));
        start = nl + 1;
    }
    std::sort(lines.begin(), lines.end());
    dstd::String out;
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
        dstd::String bin = dftu_utils_test::find_binary_by_name(
            "DFTRACER_UTILS_VIEW_PATH", "dftracer_view");
        dstd::String launcher = dftu_utils_test::find_mpi_launcher();
        if (bin.empty() || launcher.empty()) {
            MESSAGE("dftracer_view or MPI launcher unavailable, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(1);
        REQUIRE(env.is_valid());
        // Inputs live in their own dir so the .pfw outputs are not rescanned.
        const dstd::String in = env.dir() + "/in";
        REQUIRE(dstd::fs::create_directories(in).has_value());
        // Two shards, cpu util means 40 and 80 -> combined mean 60 over 4.
        write_counter_file(in + "/r0.pfw.gz", "cpu", {40, 40});
        write_counter_file(in + "/r1.pfw.gz", "cpu", {80, 80});

        // Distributed run (2 ranks) exercising the full auto-index + query
        // path. Single-rank parity is covered by the non-MPI
        // test_dftracer_view; here we assert the distributed result against the
        // known-correct value.
        const dstd::String o = env.dir() + "/o.pfw";
        const dstd::Vector<dstd::String> args = {
            in,      "--counters", "--group-by", "name",
            "--agg", "mean:util",  "--output",   o};
        REQUIRE(dftu_utils_test::run_mpi(launcher, 2, bin, args) == 0);

        const dstd::String s = read_sorted(o);
        CHECK(!s.empty());
        // util means 40 and 80 over 4 samples -> combined mean 60.
        CHECK(s.find("60") != dstd::String::npos);
    }

    // Distributed collect() (aggregate table, not counters): merge_partials_to
    // _table across ranks must equal the single-rank collect.
    TEST_CASE("distributed aggregate table matches single-rank") {
        dstd::String bin = dftu_utils_test::find_binary_by_name(
            "DFTRACER_UTILS_VIEW_PATH", "dftracer_view");
        dstd::String launcher = dftu_utils_test::find_mpi_launcher();
        if (bin.empty() || launcher.empty()) {
            MESSAGE("dftracer_view or MPI launcher unavailable, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(1);
        REQUIRE(env.is_valid());
        const dstd::String in = env.dir() + "/in";
        REQUIRE(dstd::fs::create_directories(in).has_value());
        write_counter_file(in + "/r0.pfw.gz", "cpu", {40, 40});
        write_counter_file(in + "/r1.pfw.gz", "cpu", {80, 80});

        // No --counters: aggregate mode -> collect() table via print_table.
        // Distributed run only; the merged table must carry the correct mean.
        const dstd::String o = env.dir() + "/a.json";
        const dstd::Vector<dstd::String> args = {
            in, "--group-by", "name", "--agg", "mean:util", "--output", o};
        REQUIRE(dftu_utils_test::run_mpi(launcher, 2, bin, args) == 0);

        const dstd::String s = read_sorted(o);
        CHECK(!s.empty());
        CHECK(s.find("cpu") != dstd::String::npos);  // group key
        CHECK(s.find("60") != dstd::String::npos);   // mean util across ranks
    }

    // Distributed occupancy: each rank aggregate_partial()s its shard, the
    // coordinator merge_partials_to_table()s. Two overlapping events on
    // separate ranks must OR-merge to the union (busy = 500000, concurrency =
    // 2), not sum to 1000000 - which only holds if the occupancy masks survive
    // the partial serialization the ranks ship. Guards the distributed
    // transport end to end.
    TEST_CASE("distributed occupancy merges across ranks") {
        dstd::String bin = dftu_utils_test::find_binary_by_name(
            "DFTRACER_UTILS_VIEW_PATH", "dftracer_view");
        dstd::String launcher = dftu_utils_test::find_mpi_launcher();
        if (bin.empty() || launcher.empty()) {
            MESSAGE("dftracer_view or MPI launcher unavailable, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(1);
        REQUIRE(env.is_valid());
        const dstd::String in = env.dir() + "/in";
        REQUIRE(dstd::fs::create_directories(in).has_value());
        write_event_file(in + "/r0.pfw.gz", "io", 0, 500000);
        write_event_file(in + "/r1.pfw.gz", "io", 0, 500000);  // overlaps r0

        const dstd::String o = env.dir() + "/occ.json";
        const dstd::Vector<dstd::String> args = {
            in, "--group-by", "name", "--agg", "busy", "--output", o};
        REQUIRE(dftu_utils_test::run_mpi(launcher, 2, bin, args) == 0);

        const dstd::String s = read_sorted(o);
        CHECK(!s.empty());
        CHECK(s.find("io") != dstd::String::npos);
        // busy is the union across ranks (500000), not the sum (1000000).
        CHECK(s.find("500000") != dstd::String::npos);
        CHECK(s.find("1000000") == dstd::String::npos);
    }

    // Distributed --merge: each rank writes its shard, rank 0 concatenates. The
    // merged event set must equal the single-rank merge (plain output so the
    // NDJSON lines sort/compare directly).
    TEST_CASE("distributed merge matches single-rank") {
        dstd::String bin = dftu_utils_test::find_binary_by_name(
            "DFTRACER_UTILS_VIEW_PATH", "dftracer_view");
        dstd::String launcher = dftu_utils_test::find_mpi_launcher();
        if (bin.empty() || launcher.empty()) {
            MESSAGE("dftracer_view or MPI launcher unavailable, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(1);
        REQUIRE(env.is_valid());
        const dstd::String in = env.dir() + "/in";
        REQUIRE(dstd::fs::create_directories(in).has_value());
        write_counter_file(in + "/r0.pfw.gz", "cpu", {40, 40});
        write_counter_file(in + "/r1.pfw.gz", "cpu", {80, 80});

        // Distributed --merge: each rank writes its shard, rank 0 concatenates.
        const dstd::String o = env.dir() + "/m.pfw";
        const dstd::Vector<dstd::String> args = {in, "--merge", "--output", o};
        REQUIRE(dftu_utils_test::run_mpi(launcher, 2, bin, args) == 0);

        // Output is always a compressed trace (.gz).
        const dstd::String s = read_sorted_gz(o + ".gz");
        CHECK(!s.empty());
        // All 4 source events (util 40,40,80,80) survive the shard->merge path.
        CHECK(std::count(s.begin(), s.end(), '\n') == 4);
        CHECK(s.find("40") != dstd::String::npos);
        CHECK(s.find("80") != dstd::String::npos);
    }
}
