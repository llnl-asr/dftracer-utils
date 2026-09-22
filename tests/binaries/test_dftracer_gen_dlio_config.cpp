#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
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
#include <string>
#include <vector>

namespace {

// (cat, name) labels for the three emitted components. Defaults are the DLIO
// benchmark event names; override to exercise the loader's event selectors.
struct EventLabels {
    std::string fetch_block_cat = "dataloader";
    std::string fetch_block_name = "fetch.block";
    std::string fetch_iter_cat = "dataloader";
    std::string fetch_iter_name = "fetch.iter";
    std::string preprocess_cat = "data";
    std::string preprocess_name = "preprocess";
};

// Produce a .pfw.gz file containing realistic DLIO trace events: a mix of
// fetch.block / fetch.iter and preprocess duration events spread across
// `num_ranks` pids. Each rank gets `events_per_rank` of each kind. Event
// categories and names come from `labels`.
std::string create_dlio_pfw_gz(dftu_utils_test::TestEnvironment& env, int id,
                               int num_ranks, int events_per_rank,
                               const EventLabels& labels = {}) {
    const std::string plain_path =
        env.get_dir() + "/dlio_trace_" + std::to_string(id) + ".pfw";
    {
        std::ofstream ofs(plain_path);
        if (!ofs.is_open()) return "";
        ofs << "[\n";
        std::uint64_t event_id = 1;
        const std::uint64_t base_ts = 1000000000ULL;
        for (int rank = 0; rank < num_ranks; ++rank) {
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
    std::string gz_path = plain_path + ".gz";
    {
        gzFile gz = gzopen(gz_path.c_str(), "wb");
        if (!gz) return "";
        std::ifstream ifs(plain_path, std::ios::binary);
        std::stringstream ss;
        ss << ifs.rdbuf();
        const std::string body = ss.str();
        gzwrite(gz, body.data(), static_cast<unsigned>(body.size()));
        gzclose(gz);
    }
    fs::remove(plain_path);
    return gz_path;
}

std::string find_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_GEN_DLIO_CONFIG_PATH",
                                                "dftracer_gen_dlio_config");
}

int run_binary(const std::string& binary,
               const std::vector<std::string>& args) {
    return dftu_utils_test::run_process(binary, args);
}

std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

}  // namespace

TEST_SUITE("DFTracerGenDlioConfig") {
    TEST_CASE("binary exists") {
        const auto binary = find_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_gen_dlio_config not found. Set "
                "DFTRACER_GEN_DLIO_CONFIG_PATH to locate it.");
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
        // Pointing -d at a valid empty dir but no -o; argparse should fail.
        dftu_utils_test::TestEnvironment env(10);
        REQUIRE(env.is_valid());
        CHECK(run_binary(binary, {"-d", env.get_dir()}) != 0);
    }

    TEST_CASE("directory without DLIO events fails gracefully") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        dftu_utils_test::TestEnvironment env(50);
        REQUIRE(env.is_valid());

        // Generic POSIX trace (no fetch.block/preprocess events).
        auto trace_gz = env.create_dft_test_gzip_file(50);
        REQUIRE(!trace_gz.empty());

        const std::string out = env.get_dir() + "/dlio_config.yaml";
        const int rc = run_binary(binary, {"-d", env.get_dir(), "-o", out});
        // Non-zero exit and no YAML produced.
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out));
    }

    TEST_CASE(
        "happy path: DLIO traces produce a valid YAML with train + reader") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        dftu_utils_test::TestEnvironment env(200);
        REQUIRE(env.is_valid());

        for (int i = 0; i < 2; ++i) {
            auto f = create_dlio_pfw_gz(env, i, /*num_ranks=*/2,
                                        /*events_per_rank=*/200);
            REQUIRE(!f.empty());
        }

        const std::string out = env.get_dir() + "/dlio_config.yaml";
        const int rc = run_binary(binary, {"-d", env.get_dir(), "-o", out,
                                           "--simulation-iterations", "2"});
        CHECK(rc == 0);
        REQUIRE(fs::exists(out));

        const std::string contents = read_file(out);
        // Spot-check the YAML schema. Don't pin specific distribution choice
        // (fitter may pick any of single/GMM-2/GMM-3 depending on data).
        CHECK(contents.find("train:") != std::string::npos);
        CHECK(contents.find("computation_time:") != std::string::npos);
        CHECK(contents.find("reader:") != std::string::npos);
        CHECK(contents.find("preprocess_time:") != std::string::npos);
        CHECK(contents.find("type:") != std::string::npos);
        CHECK(contents.find("max_bound:") != std::string::npos);
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

        auto f = create_dlio_pfw_gz(env, 0, /*num_ranks=*/2,
                                    /*events_per_rank=*/200, labels);
        REQUIRE(!f.empty());

        // Without overrides the loader looks for DLIO defaults and finds
        // nothing.
        const std::string out_default = env.get_dir() + "/dlio_default.yaml";
        const int rc_default =
            run_binary(binary, {"-d", env.get_dir(), "-o", out_default,
                                "--simulation-iterations", "2"});
        CHECK(rc_default != 0);
        CHECK_FALSE(fs::exists(out_default));

        // A YAML event map remaps the components onto the custom events.
        const std::string map_path = env.get_dir() + "/event_map.yaml";
        {
            std::ofstream ofs(map_path);
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

        const std::string out = env.get_dir() + "/dlio_config.yaml";
        const int rc = run_binary(
            binary, {"-d", env.get_dir(), "-o", out, "--simulation-iterations",
                     "2", "--event-map", map_path});
        CHECK(rc == 0);
        REQUIRE(fs::exists(out));

        const std::string contents = read_file(out);
        CHECK(contents.find("computation_time:") != std::string::npos);
        CHECK(contents.find("preprocess_time:") != std::string::npos);
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

        auto f = create_dlio_pfw_gz(env, 0, /*num_ranks=*/2,
                                    /*events_per_rank=*/200, labels);
        REQUIRE(!f.empty());

        // JSON is a YAML subset, so the same loader accepts a .json map.
        const std::string map_path = env.get_dir() + "/event_map.json";
        {
            std::ofstream ofs(map_path);
            REQUIRE(ofs.is_open());
            ofs << R"({"fetch_block":{"cat":")" << labels.fetch_block_cat
                << R"(","name":")" << labels.fetch_block_name << R"("},)"
                << R"("preprocess":{"cat":")" << labels.preprocess_cat
                << R"(","name":")" << labels.preprocess_name << R"("}})";
        }

        const std::string out = env.get_dir() + "/dlio_config.yaml";
        const int rc = run_binary(
            binary, {"-d", env.get_dir(), "-o", out, "--simulation-iterations",
                     "2", "--event-map", map_path});
        CHECK(rc == 0);
        CHECK(fs::exists(out));
    }

    TEST_CASE("malformed event map is rejected") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        dftu_utils_test::TestEnvironment env(200);
        REQUIRE(env.is_valid());

        auto f = create_dlio_pfw_gz(env, 0, /*num_ranks=*/1,
                                    /*events_per_rank=*/150);
        REQUIRE(!f.empty());

        const std::string map_path = env.get_dir() + "/bad_map.yaml";
        {
            std::ofstream ofs(map_path);
            REQUIRE(ofs.is_open());
            ofs << "fetch_block: {cat: dataloader, name: fetch.block\n";
        }

        const std::string out = env.get_dir() + "/dlio_config.yaml";
        const int rc = run_binary(
            binary, {"-d", env.get_dir(), "-o", out, "--simulation-iterations",
                     "2", "--event-map", map_path});
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out));
    }

    TEST_CASE("respects --num-workers and --prefetch-factor") {
        const auto binary = find_binary();
        if (binary.empty()) return;
        dftu_utils_test::TestEnvironment env(200);
        REQUIRE(env.is_valid());

        auto f = create_dlio_pfw_gz(env, 0, /*num_ranks=*/1,
                                    /*events_per_rank=*/150);
        REQUIRE(!f.empty());

        const std::string out = env.get_dir() + "/dlio_config.yaml";
        const int rc = run_binary(
            binary, {"-d", env.get_dir(), "-o", out, "--num-workers", "4",
                     "--prefetch-factor", "1", "--simulation-iterations", "2"});
        CHECK(rc == 0);
        CHECK(fs::exists(out));
    }
}
