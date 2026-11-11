// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace dftracer::utils;

namespace {

// (cat, name) labels for the three emitted components. Defaults are the DLIO
// benchmark event names; override to exercise the loader's event selectors.
struct EventLabels {
    dstd::String fetch_block_cat = "dataloader";
    dstd::String fetch_block_name = "fetch.block";
    dstd::String fetch_iter_cat = "dataloader";
    dstd::String fetch_iter_name = "fetch.iter";
    dstd::String preprocess_cat = "data";
    dstd::String preprocess_name = "preprocess";
};

// Produce a .pfw.gz file containing realistic DLIO trace events: a mix of
// fetch.block / fetch.iter and preprocess duration events spread across
// `rank_count` pids. Each rank gets `events_per_rank` of each kind. Event
// categories and names come from `labels`.
dstd::String create_dlio_pfw_gz(dftu_utils_test::TestEnvironment& env, int id,
                                int rank_count, int events_per_rank,
                                const EventLabels& labels = {}) {
    const dstd::String plain_path =
        env.dir() + "/dlio_trace_" + dstd::to_string(id) + ".pfw";
    {
        dftu_utils_test::FileOut ofs(plain_path);
        if (!ofs.is_open()) return "";
        ofs << "[\n";
        std::uint64_t event_id = 1;
        const std::uint64_t base_ts = 1000000000ULL;
        for (int rank = 0; rank < rank_count; ++rank) {
            const std::uint64_t pid = 1000 + static_cast<std::uint64_t>(rank);
            const std::uint64_t tid = 2000 + static_cast<std::uint64_t>(rank);
            std::uint64_t ts = base_ts + rank * 100000ULL;
            for (int i = 0; i < events_per_rank; ++i) {
                // fetch.block: lognormal-ish via varying durations.
                const std::uint64_t fb_dur = 100 + (i * 7) % 500;
                ofs << R"({"id":)" << event_id++ << R"(,"pid":)" << pid
                    << R"(,"tid":)" << tid << R"(,"name":")"
                    << labels.fetch_block_name << R"(","cat":")"
                    << labels.fetch_block_cat << R"(")"
                    << R"(,"ph":"X","ts":)" << ts << R"(,"dur":)" << fb_dur
                    << R"(,"args":{"hhash":"h1"}})"
                    << "\n";
                ts += fb_dur;

                // fetch.iter: shorter durations.
                const std::uint64_t fi_dur = 50 + (i * 3) % 100;
                ofs << R"({"id":)" << event_id++ << R"(,"pid":)" << pid
                    << R"(,"tid":)" << tid << R"(,"name":")"
                    << labels.fetch_iter_name << R"(","cat":")"
                    << labels.fetch_iter_cat << R"(")"
                    << R"(,"ph":"X","ts":)" << ts << R"(,"dur":)" << fi_dur
                    << R"(,"args":{"hhash":"h1"}})"
                    << "\n";
                ts += fi_dur;

                // preprocess: emitted by a worker pid (different from main).
                const std::uint64_t worker_pid = pid + 100000ULL;
                const std::uint64_t pre_dur = 80 + (i * 5) % 200;
                ofs << R"({"id":)" << event_id++ << R"(,"pid":)" << worker_pid
                    << R"(,"tid":)" << tid << R"(,"name":")"
                    << labels.preprocess_name << R"(","cat":")"
                    << labels.preprocess_cat << R"(")"
                    << R"(,"ph":"X","ts":)" << ts << R"(,"dur":)" << pre_dur
                    << R"(,"args":{"hhash":"h1"}})"
                    << "\n";
                ts += pre_dur;
            }
        }
        ofs << "]\n";
    }

    // Compress to .pfw.gz.
    dstd::String gz_path = plain_path + ".gz";
    {
        gzFile gz = gzopen(gz_path.c_str(), "wb");
        if (!gz) return "";
        const dstd::String body = dftu_utils_test::read_text(plain_path);
        gzwrite(gz, body.data(), static_cast<unsigned>(body.size()));
        gzclose(gz);
    }
    dstd::fs::remove(plain_path).discard();
    return gz_path;
}

dstd::String find_binary() {
    return dftu_utils_test::find_binary_by_name(
        "DFTRACER_UTILS_GEN_DLIO_CONFIG_PATH", "dftracer_gen_dlio_config");
}

int run_binary(const dstd::String& binary,
               const dstd::Vector<dstd::String>& args) {
    return dftu_utils_test::run_process(binary, args);
}

dstd::String read_file(const dstd::String& path) {
    return dftu_utils_test::read_text(path);
}

}  // namespace

TEST_SUITE("DFTracerGenDlioConfig") {
    TEST_CASE("binary exists") {
        const auto binary = find_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_gen_dlio_config not found. Set "
                "DFTRACER_UTILS_GEN_DLIO_CONFIG_PATH to locate it.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("--help exits 0") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        CHECK(run_binary(binary, {"--help"}) == 0);
    }

    TEST_CASE("missing --output rejected") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        // Pointing at a valid empty dir but no -o; the parser should fail.
        dftu_utils_test::TestEnvironment env(10);
        REQUIRE(env.is_valid());
        CHECK(run_binary(binary, {env.dir()}) != 0);
    }

    TEST_CASE("directory without DLIO events fails gracefully") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        dftu_utils_test::TestEnvironment env(50);
        REQUIRE(env.is_valid());

        // Generic POSIX trace (no fetch.block/preprocess events).
        auto trace_gz = env.create_dft_test_gzip_file(50);
        REQUIRE(!trace_gz.empty());

        const dstd::String out = env.dir() + "/dlio_config.yaml";
        const int rc = run_binary(binary, {env.dir(), "-o", out});
        // Non-zero exit and no YAML produced.
        CHECK(rc != 0);
        CHECK_FALSE(dstd::fs::exists(out).value());
    }

    TEST_CASE(
        "happy path: DLIO traces produce a valid YAML with train + reader") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        dftu_utils_test::TestEnvironment env(200);
        REQUIRE(env.is_valid());

        for (int i = 0; i < 2; ++i) {
            auto f = create_dlio_pfw_gz(env, i, /*rank_count=*/2,
                                        /*events_per_rank=*/200);
            REQUIRE(!f.empty());
        }

        const dstd::String out = env.dir() + "/dlio_config.yaml";
        const int rc = run_binary(
            binary, {env.dir(), "-o", out, "--simulation-iterations", "2"});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(out).value());

        const dstd::String contents = read_file(out);
        // Spot-check the YAML schema. Don't pin specific distribution choice
        // (fitter may pick any of single/GMM-2/GMM-3 depending on data).
        CHECK(contents.find("train:") != dstd::String::npos);
        CHECK(contents.find("computation_time:") != dstd::String::npos);
        CHECK(contents.find("reader:") != dstd::String::npos);
        CHECK(contents.find("preprocess_time:") != dstd::String::npos);
        CHECK(contents.find("type:") != dstd::String::npos);
        CHECK(contents.find("max_bound:") != dstd::String::npos);
    }

    TEST_CASE("custom event cat/name overrides map onto DLIO components") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        dftu_utils_test::TestEnvironment env(200);
        REQUIRE(env.is_valid());

        EventLabels labels;
        labels.fetch_block_cat = "io";
        labels.fetch_block_name = "read";
        labels.fetch_iter_cat = "io";
        labels.fetch_iter_name = "iter";
        labels.preprocess_cat = "cpu";
        labels.preprocess_name = "transform";

        auto f = create_dlio_pfw_gz(env, 0, /*rank_count=*/2,
                                    /*events_per_rank=*/200, labels);
        REQUIRE(!f.empty());

        // Without overrides the loader looks for DLIO defaults and finds
        // nothing.
        const dstd::String out_default = env.dir() + "/dlio_default.yaml";
        const int rc_default = run_binary(
            binary,
            {env.dir(), "-o", out_default, "--simulation-iterations", "2"});
        CHECK(rc_default != 0);
        CHECK_FALSE(dstd::fs::exists(out_default).value());

        // A YAML event map remaps the components onto the custom events.
        const dstd::String map_path = env.dir() + "/event_map.yaml";
        {
            dftu_utils_test::FileOut ofs(map_path);
            REQUIRE(ofs.is_open());
            ofs << "fetch_block:\n"
                << "  cat: " << labels.fetch_block_cat << "\n"
                << "  name: " << labels.fetch_block_name << "\n"
                << "fetch_iter:\n"
                << "  cat: " << labels.fetch_iter_cat << "\n"
                << "  name: " << labels.fetch_iter_name << "\n"
                << "preprocess:\n"
                << "  cat: " << labels.preprocess_cat << "\n"
                << "  name: " << labels.preprocess_name << "\n";
        }

        const dstd::String out = env.dir() + "/dlio_config.yaml";
        const int rc =
            run_binary(binary, {env.dir(), "-o", out, "--simulation-iterations",
                                "2", "--event-map", map_path});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(out).value());

        const dstd::String contents = read_file(out);
        CHECK(contents.find("computation_time:") != dstd::String::npos);
        CHECK(contents.find("preprocess_time:") != dstd::String::npos);
    }

    TEST_CASE("JSON event map is accepted") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        dftu_utils_test::TestEnvironment env(200);
        REQUIRE(env.is_valid());

        EventLabels labels;
        labels.fetch_block_cat = "io";
        labels.fetch_block_name = "read";
        labels.fetch_iter_cat = "io";
        labels.fetch_iter_name = "iter";
        labels.preprocess_cat = "cpu";
        labels.preprocess_name = "transform";

        auto f = create_dlio_pfw_gz(env, 0, /*rank_count=*/2,
                                    /*events_per_rank=*/200, labels);
        REQUIRE(!f.empty());

        // JSON is a YAML subset, so the same loader accepts a .json map.
        const dstd::String map_path = env.dir() + "/event_map.json";
        {
            dftu_utils_test::FileOut ofs(map_path);
            REQUIRE(ofs.is_open());
            ofs << R"({"fetch_block":{"cat":")" << labels.fetch_block_cat
                << R"(","name":")" << labels.fetch_block_name << R"("},)"
                << R"("preprocess":{"cat":")" << labels.preprocess_cat
                << R"(","name":")" << labels.preprocess_name << R"("}})";
        }

        const dstd::String out = env.dir() + "/dlio_config.yaml";
        const int rc =
            run_binary(binary, {env.dir(), "-o", out, "--simulation-iterations",
                                "2", "--event-map", map_path});
        CHECK(rc == 0);
        CHECK(dstd::fs::exists(out).value());
    }

    TEST_CASE("malformed event map is rejected") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        dftu_utils_test::TestEnvironment env(200);
        REQUIRE(env.is_valid());

        auto f = create_dlio_pfw_gz(env, 0, /*rank_count=*/1,
                                    /*events_per_rank=*/150);
        REQUIRE(!f.empty());

        const dstd::String map_path = env.dir() + "/bad_map.yaml";
        {
            dftu_utils_test::FileOut ofs(map_path);
            REQUIRE(ofs.is_open());
            ofs << "fetch_block: {cat: dataloader, name: fetch.block\n";
        }

        const dstd::String out = env.dir() + "/dlio_config.yaml";
        const int rc =
            run_binary(binary, {env.dir(), "-o", out, "--simulation-iterations",
                                "2", "--event-map", map_path});
        CHECK(rc != 0);
        CHECK_FALSE(dstd::fs::exists(out).value());
    }

    TEST_CASE("respects --num-workers and --prefetch-factor") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        dftu_utils_test::TestEnvironment env(200);
        REQUIRE(env.is_valid());

        auto f = create_dlio_pfw_gz(env, 0, /*rank_count=*/1,
                                    /*events_per_rank=*/150);
        REQUIRE(!f.empty());

        const dstd::String out = env.dir() + "/dlio_config.yaml";
        const int rc = run_binary(
            binary, {env.dir(), "-o", out, "--num-workers", "4",
                     "--prefetch-factor", "1", "--simulation-iterations", "2"});
        CHECK(rc == 0);
        CHECK(dstd::fs::exists(out).value());
    }
}
