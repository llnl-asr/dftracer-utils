#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::indexing;

// Helper to create a test trace file (plain text), then compress to gzip.
// Returns the .gz path and the uncompressed size.
static std::pair<std::string, std::size_t> create_test_trace_gz(
    const std::string& dir, int num_events) {
    std::string plain_path = dir + "/test_index.trace";
    {
        std::ofstream ofs(plain_path);

        // Metadata events
        ofs << R"({"name":"HH","ph":"M","args":{"hhash":"abc123","name":"testhost","value":"abc123"}})"
            << "\n";
        ofs << R"({"name":"FH","ph":"M","args":{"hhash":"abc123","name":"./data/file.h5","value":"def456"}})"
            << "\n";
        ofs << R"({"name":"SH","ph":"M","args":{"hhash":"abc123","name":"my_app","value":"ghi789"}})"
            << "\n";

        // Regular events
        for (int i = 0; i < num_events; ++i) {
            std::uint64_t ts = 1000000 + static_cast<std::uint64_t>(i * 1000);
            int dur = 100 + i * 10;
            const char* names[] = {"read", "write", "open", "close"};
            const char* cats[] = {"POSIX", "storage"};

            ofs << R"({"name":")" << names[i % 4] << R"(","cat":")"
                << cats[i % 2] << R"(","pid":)" << (1000 + i % 3)
                << R"(,"tid":)" << (2000 + i % 2) << R"(,"ts":)" << ts
                << R"(,"dur":)" << dur << R"(,"ph":"X")"
                << R"(,"args":{"hhash":"abc123","fhash":"def456")"
                << R"(,"level":)" << (i % 5) << R"(,"io":{"size":)"
                << (1024 * (i + 1)) << R"(}})"
                << R"(})" << "\n";
        }

        ofs.close();
    }

    std::size_t uncompressed_size = fs::file_size(plain_path);
    std::string gz_path = plain_path + ".gz";
    dft_utils_test::compress_file_to_gzip(plain_path, gz_path);
    fs::remove(plain_path);

    return {gz_path, uncompressed_size};
}

TEST_SUITE("ChunkIndexerUtility") {
    TEST_CASE("ChunkIndexer - Process trace with metadata and events") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_chunk_indexer")
                .string();
        fs::create_directories(test_dir);

        auto [trace_file, uncompressed_size] =
            create_test_trace_gz(test_dir, 50);

        ChunkIndexerConfig config;
        config.expected_entries_per_chunk = 100;
        config.false_positive_rate = 0.01;

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
        CHECK(output.events_processed == 50);
        CHECK(output.checkpoint_idx == 0);

        // Verify bloom filters exist for default dimensions
        CHECK(output.bloom_filters.count("name") == 1);
        CHECK(output.bloom_filters.count("cat") == 1);
        CHECK(output.bloom_filters.count("pid") == 1);
        CHECK(output.bloom_filters.count("tid") == 1);
        CHECK(output.bloom_filters.count("hhash") == 1);
        CHECK(output.bloom_filters.count("fhash") == 1);

        // Verify bloom filter contents
        CHECK(output.bloom_filters.at("name").possibly_contains("read"));
        CHECK(output.bloom_filters.at("name").possibly_contains("write"));
        CHECK(output.bloom_filters.at("name").possibly_contains("open"));
        CHECK(output.bloom_filters.at("name").possibly_contains("close"));
        CHECK(output.bloom_filters.at("cat").possibly_contains("POSIX"));
        CHECK(output.bloom_filters.at("cat").possibly_contains("storage"));

        // Verify hash bloom contains hash values
        CHECK(output.bloom_filters.at("hhash").possibly_contains("abc123"));
        CHECK(output.bloom_filters.at("fhash").possibly_contains("def456"));

        // Verify statistics
        CHECK(output.statistics.total_events == 50);
        CHECK(output.statistics.category_counts.count("POSIX") == 1);
        CHECK(output.statistics.category_counts.count("storage") == 1);
        CHECK(output.statistics.name_counts.count("read") == 1);
        CHECK(output.statistics.duration_count == 50);

        // Verify hash resolutions from metadata events
        CHECK(output.hash_resolutions.count("hhash") == 1);
        CHECK(output.hash_resolutions.at("hhash").at("abc123") == "testhost");
        CHECK(output.hash_resolutions.count("fhash") == 1);
        CHECK(output.hash_resolutions.at("fhash").at("def456") ==
              "./data/file.h5");
        CHECK(output.hash_resolutions.count("shash") == 1);
        CHECK(output.hash_resolutions.at("shash").at("ghi789") == "my_app");

        fs::remove_all(test_dir);
    }

    TEST_CASE("ChunkIndexer - Extra dimensions with nested keys") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_chunk_indexer_extra")
                .string();
        fs::create_directories(test_dir);

        auto [trace_file, uncompressed_size] =
            create_test_trace_gz(test_dir, 20);

        ChunkIndexerConfig config;
        config.expected_entries_per_chunk = 100;
        config.false_positive_rate = 0.01;
        // Test arbitrary nested key access
        config.extra_dimensions = {"level", "io.size"};

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
        CHECK(output.events_processed == 20);

        // Extra dimensions should have bloom filters
        CHECK(output.bloom_filters.count("level") == 1);
        CHECK(output.bloom_filters.count("io.size") == 1);

        // Level values are 0-4
        CHECK(output.bloom_filters.at("level").possibly_contains("0"));
        CHECK(output.bloom_filters.at("level").possibly_contains("1"));
        CHECK(output.bloom_filters.at("level").possibly_contains("4"));

        // io.size values: 1024, 2048, ...
        CHECK(output.bloom_filters.at("io.size").possibly_contains("1024"));
        CHECK(output.bloom_filters.at("io.size").possibly_contains("2048"));

        fs::remove_all(test_dir);
    }

    TEST_CASE("ChunkIndexer - Events missing some fields") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_chunk_indexer_missing")
                .string();
        fs::create_directories(test_dir);

        std::string plain_path = test_dir + "/test_sparse.trace";
        {
            std::ofstream ofs(plain_path);
            // Event without fhash (not all events are I/O)
            ofs << R"({"name":"compute","cat":"APP","pid":1,"tid":1,"ts":1000,"dur":500,"ph":"X","args":{"hhash":"abc123"}})"
                << "\n";
            // Event with fhash
            ofs << R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":2000,"dur":100,"ph":"X","args":{"hhash":"abc123","fhash":"def456"}})"
                << "\n";
            ofs.close();
        }

        std::size_t uncompressed_size = fs::file_size(plain_path);
        std::string gz_path = plain_path + ".gz";
        dft_utils_test::compress_file_to_gzip(plain_path, gz_path);
        fs::remove(plain_path);

        ChunkIndexerConfig config;
        config.expected_entries_per_chunk = 100;

        ChunkIndexerInput input;
        input.with_file_path(gz_path)
            .with_idx_path("")
            .with_checkpoint_size(uncompressed_size)
            .with_checkpoint_idx(0)
            .with_byte_range(0, uncompressed_size)
            .with_config(config)
            .with_batch_size(4 * 1024 * 1024);

        ChunkIndexerUtility indexer;
        auto output = indexer.process(input);

        CHECK(output.success == true);
        CHECK(output.events_processed == 2);
        CHECK(output.bloom_filters.at("fhash").possibly_contains("def456"));
        CHECK(output.statistics.total_events == 2);

        fs::remove_all(test_dir);
    }
}
