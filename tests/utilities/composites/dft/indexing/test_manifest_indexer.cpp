#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <fstream>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::indexing;

static std::pair<std::string, std::size_t> create_manifest_test_trace(
    const std::string& dir) {
    std::string plain_path = dir + "/test_manifest.trace";
    {
        std::ofstream ofs(plain_path);
        // Line 0: HH metadata
        ofs << R"({"name":"HH","ph":"M","args":{"value":"abc123","name":"testhost"}})"
            << "\n";
        // Line 1: FH metadata
        ofs << R"({"name":"FH","ph":"M","args":{"value":"def456","name":"./data/file.h5"}})"
            << "\n";
        // Line 2: POSIX read
        ofs << R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":100,"ph":"X","args":{"hhash":"abc123","fhash":"def456"}})"
            << "\n";
        // Line 3: POSIX write
        ofs << R"({"name":"write","cat":"POSIX","pid":1,"tid":1,"ts":2000,"dur":200,"ph":"X","args":{"hhash":"abc123","fhash":"def456"}})"
            << "\n";
        // Line 4: POSIX read
        ofs << R"({"name":"read","cat":"POSIX","pid":1,"tid":2,"ts":3000,"dur":150,"ph":"X","args":{"hhash":"abc123","fhash":"def456"}})"
            << "\n";
        // Line 5: APP compute
        ofs << R"({"name":"compute","cat":"APP","pid":1,"tid":1,"ts":4000,"dur":500,"ph":"X","args":{"hhash":"abc123"}})"
            << "\n";
        // Line 6: SH metadata
        ofs << R"({"name":"SH","ph":"M","args":{"value":"ghi789","name":"my_app"}})"
            << "\n";
        // Line 7: POSIX read
        ofs << R"({"name":"read","cat":"POSIX","pid":2,"tid":1,"ts":5000,"dur":120,"ph":"X","args":{"hhash":"abc123","fhash":"def456"}})"
            << "\n";
        ofs.close();
    }

    std::size_t uncompressed_size = fs::file_size(plain_path);
    std::string gz_path = plain_path + ".gz";
    dft_utils_test::compress_file_to_gzip(plain_path, gz_path);
    fs::remove(plain_path);

    return {gz_path, uncompressed_size};
}

TEST_SUITE("ManifestIndexer") {
    TEST_CASE("Manifest collection - event line groups") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_indexer")
                .string();
        fs::create_directories(test_dir);

        auto [trace_file, uncompressed_size] =
            create_manifest_test_trace(test_dir);

        ChunkIndexerConfig config;
        config.expected_entries_per_chunk = 100;
        config.false_positive_rate = 0.01;
        config.build_manifest = true;

        ChunkIndexerInput input;
        input.with_file_path(trace_file)
            .with_idx_path("")
            .with_checkpoint_size(uncompressed_size)
            .with_checkpoint_idx(0)
            .with_byte_range(0, uncompressed_size)
            .with_config(config)
            .with_batch_size(4 * 1024 * 1024);

        ChunkIndexerUtility indexer;
        auto output = indexer.process(input);

        CHECK(output.success == true);
        CHECK(output.events_processed == 5);

        // Verify event line groups
        // Expected groups:
        //   (POSIX, read) -> lines [2, 4, 7]
        //   (POSIX, write) -> lines [3]
        //   (APP, compute) -> lines [5]
        CHECK(output.event_line_groups.size() == 3);

        auto find_group =
            [&](const std::string& cat,
                const std::string& name) -> const EventLineGroup* {
            for (const auto& g : output.event_line_groups) {
                if (g.cat == cat && g.name == name) {
                    return &g;
                }
            }
            return nullptr;
        };

        auto* posix_read = find_group("POSIX", "read");
        REQUIRE(posix_read != nullptr);
        CHECK(posix_read->line_numbers.size() == 3);
        CHECK(posix_read->line_numbers[0] == 2);
        CHECK(posix_read->line_numbers[1] == 4);
        CHECK(posix_read->line_numbers[2] == 7);

        auto* posix_write = find_group("POSIX", "write");
        REQUIRE(posix_write != nullptr);
        CHECK(posix_write->line_numbers.size() == 1);
        CHECK(posix_write->line_numbers[0] == 3);

        auto* app_compute = find_group("APP", "compute");
        REQUIRE(app_compute != nullptr);
        CHECK(app_compute->line_numbers.size() == 1);
        CHECK(app_compute->line_numbers[0] == 5);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Manifest collection - metadata line groups") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_meta")
                .string();
        fs::create_directories(test_dir);

        auto [trace_file, uncompressed_size] =
            create_manifest_test_trace(test_dir);

        ChunkIndexerConfig config;
        config.build_manifest = true;

        ChunkIndexerInput input;
        input.with_file_path(trace_file)
            .with_idx_path("")
            .with_checkpoint_size(uncompressed_size)
            .with_checkpoint_idx(0)
            .with_byte_range(0, uncompressed_size)
            .with_config(config)
            .with_batch_size(4 * 1024 * 1024);

        ChunkIndexerUtility indexer;
        auto output = indexer.process(input);

        CHECK(output.success == true);

        // Verify metadata line groups
        // Expected: HH -> [0], FH -> [1], SH -> [6]
        CHECK(output.metadata_line_groups.size() == 3);

        auto find_meta =
            [&](const std::string& meta_type) -> const MetadataLineGroup* {
            for (const auto& g : output.metadata_line_groups) {
                if (g.meta_type == meta_type) {
                    return &g;
                }
            }
            return nullptr;
        };

        auto* hh = find_meta("HH");
        REQUIRE(hh != nullptr);
        CHECK(hh->line_numbers.size() == 1);
        CHECK(hh->line_numbers[0] == 0);

        auto* fh = find_meta("FH");
        REQUIRE(fh != nullptr);
        CHECK(fh->line_numbers.size() == 1);
        CHECK(fh->line_numbers[0] == 1);

        auto* sh = find_meta("SH");
        REQUIRE(sh != nullptr);
        CHECK(sh->line_numbers.size() == 1);
        CHECK(sh->line_numbers[0] == 6);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Manifest disabled by default") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_disabled")
                .string();
        fs::create_directories(test_dir);

        auto [trace_file, uncompressed_size] =
            create_manifest_test_trace(test_dir);

        ChunkIndexerConfig config;
        // build_manifest defaults to false

        ChunkIndexerInput input;
        input.with_file_path(trace_file)
            .with_idx_path("")
            .with_checkpoint_size(uncompressed_size)
            .with_checkpoint_idx(0)
            .with_byte_range(0, uncompressed_size)
            .with_config(config)
            .with_batch_size(4 * 1024 * 1024);

        ChunkIndexerUtility indexer;
        auto output = indexer.process(input);

        CHECK(output.success == true);
        CHECK(output.event_line_groups.empty());
        CHECK(output.metadata_line_groups.empty());

        fs::remove_all(test_dir);
    }
}
