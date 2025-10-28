#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector.h>
#include <doctest/doctest.h>
#include <unistd.h>  // for getpid()

#include <cstdint>
#include <fstream>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft;

// Helper to create a test trace file
static std::string create_test_trace_file(const std::string& dir,
                                          int num_events) {
    std::string file_path = dir + "/test.trace";
    std::ofstream ofs(file_path);

    const char* io_names[] = {"pread", "pwrite", "read",
                              "write", "fread",  "fwrite"};
    const int num_names = sizeof(io_names) / sizeof(io_names[0]);

    for (int i = 1; i <= num_events; ++i) {
        // Create valid JSON events similar to DFTracer format
        uint64_t timestamp_us =
            1000000000ULL + static_cast<uint64_t>(i * 100000);
        const char* op_name = io_names[i % num_names];
        int size = 1024 * i;

        ofs << R"({"id":)" << i << R"(,"pid":)" << (1000 + i) << R"(,"tid":)"
            << (2000 + i) << R"(,"name":")" << op_name << R"(")"
            << R"(,"cat":"IO")"
            << R"(,"ph":"C")"
            << R"(,"ts":)" << timestamp_us << R"(,"dur":)" << (100 + i * 10)
            << R"(,"args":{"ret":)" << size << R"(})"
            << R"(})" << "\n";
    }
    ofs.close();
    return file_path;
}

TEST_SUITE("MetadataCollector") {
    TEST_CASE("MetadataCollector - Collect from plain trace file") {
        // Create temp directory
        std::string test_dir =
            "/tmp/test_metadata_collector_" + std::to_string(getpid());
        fs::create_directories(test_dir);

        SUBCASE("Small trace file") {
            // Create a test trace file with 10 events
            std::string trace_file = create_test_trace_file(test_dir, 10);

            // Create input
            auto input = MetadataCollectorUtilityInput::from_file(trace_file);

            // Process
            MetadataCollectorUtility collector;
            auto output = collector.process(input);

            // Verify output
            CHECK(output.success == true);
            CHECK(output.file_path == trace_file);
            CHECK(output.valid_events == 10);
            CHECK(output.end_line >= 9);  // Should have at least 10 lines (0-9)
            CHECK(output.size_mb > 0);
            CHECK(output.size_per_line > 0);
        }

        SUBCASE("Large trace file with checkpoint") {
            // Create larger trace file
            std::string trace_file = create_test_trace_file(test_dir, 1000);

            // Create input with small checkpoint size
            auto input = MetadataCollectorUtilityInput::from_file(trace_file)
                             .with_checkpoint_size(100)
                             .with_force_rebuild(true);

            // Process
            MetadataCollectorUtility collector;
            auto output = collector.process(input);

            // Verify
            CHECK(output.success == true);
            CHECK(output.valid_events == 1000);
            CHECK(output.size_mb > 0);
        }

        SUBCASE("Extended metadata fields - plain file") {
            // Create a test trace file
            std::string trace_file = create_test_trace_file(test_dir, 100);

            // Create input
            auto input = MetadataCollectorUtilityInput::from_file(trace_file);

            // Process
            MetadataCollectorUtility collector;
            auto output = collector.process(input);

            // Verify extended metadata fields
            CHECK(output.success == true);
            CHECK(output.file_path == trace_file);
            CHECK(output.valid_events == 100);

            // Extended fields for plain files
            CHECK(output.format == ArchiveFormat::UNKNOWN);  // Plain text
            CHECK(output.has_index == false);
            CHECK(output.index_valid == false);
            CHECK(output.compressed_size > 0);
            CHECK(output.uncompressed_size > 0);
            CHECK(output.compressed_size ==
                  output.uncompressed_size);  // No compression
            CHECK(output.num_lines == 100);
            CHECK(output.checkpoint_size ==
                  0);                         // No checkpoints for plain files
            CHECK(output.num_checkpoints == 0);
            CHECK(output.error_message.empty());
        }

        // Clean up
        fs::remove_all(test_dir);
    }

    TEST_CASE("MetadataCollector - Extended fields for compressed files") {
        // Create temp directory
        std::string test_dir =
            "/tmp/test_metadata_compressed_" + std::to_string(getpid());
        fs::create_directories(test_dir);

        SUBCASE("Compressed file with index - extended metadata") {
            // Create a plain trace file first
            std::string trace_file = create_test_trace_file(test_dir, 50);

            // Compress it using gzip
            std::string gz_file = trace_file + ".gz";
            std::string cmd = "gzip -c " + trace_file + " > " + gz_file;
            int result = std::system(cmd.c_str());
            REQUIRE(result == 0);
            REQUIRE(fs::exists(gz_file));

            // Create input with index
            std::string idx_path = gz_file + ".idx";
            auto input = MetadataCollectorUtilityInput::from_file(gz_file)
                             .with_index(idx_path)
                             .with_checkpoint_size(1024 * 1024)  // 1MB
                             .with_force_rebuild(true);

            // Process
            MetadataCollectorUtility collector;
            auto output = collector.process(input);

            // Verify basic fields
            CHECK(output.success == true);
            CHECK(output.file_path == gz_file);
            CHECK(output.valid_events > 0);

            // Verify extended fields for compressed files
            CHECK(output.format == ArchiveFormat::GZIP);
            CHECK(output.has_index == true);
            CHECK(output.index_valid == true);
            CHECK(output.idx_path == idx_path);
            CHECK(output.compressed_size > 0);
            CHECK(output.uncompressed_size > 0);
            CHECK(output.compressed_size <
                  output.uncompressed_size);  // Should be compressed
            CHECK(output.num_lines == 50);
            CHECK(output.checkpoint_size > 0);
            CHECK(output.error_message.empty());

            // Verify index file was created
            CHECK(fs::exists(idx_path));
        }

        SUBCASE("Reuse existing index") {
            // Create and compress a trace file
            std::string trace_file = create_test_trace_file(test_dir, 25);
            std::string gz_file = trace_file + ".gz";
            std::string cmd = "gzip -c " + trace_file + " > " + gz_file;
            int result = std::system(cmd.c_str());
            REQUIRE(result == 0);

            std::string idx_path = gz_file + ".idx";

            // First run - build index
            {
                auto input = MetadataCollectorUtilityInput::from_file(gz_file)
                                 .with_index(idx_path)
                                 .with_force_rebuild(true);

                MetadataCollectorUtility collector;
                auto output = collector.process(input);

                CHECK(output.success == true);
                CHECK(output.has_index == true);
                CHECK(output.index_valid == true);
                CHECK(fs::exists(idx_path));
            }

            // Second run - reuse index (no force rebuild)
            {
                auto input = MetadataCollectorUtilityInput::from_file(gz_file)
                                 .with_index(idx_path)
                                 .with_force_rebuild(false);

                MetadataCollectorUtility collector;
                auto output = collector.process(input);

                CHECK(output.success == true);
                CHECK(output.has_index == true);
                CHECK(output.index_valid == true);
                CHECK(output.format == ArchiveFormat::GZIP);
                CHECK(output.num_lines == 25);
            }
        }

        // Clean up
        fs::remove_all(test_dir);
    }

    TEST_CASE("MetadataCollector - Invalid inputs") {
        SUBCASE("Non-existent file") {
            auto input = MetadataCollectorUtilityInput::from_file(
                "/non/existent/file.trace");

            MetadataCollectorUtility collector;
            MetadataCollectorUtilityOutput output;
            try {
                output = collector.process(input);
            } catch (...) {
                // noop
            }

            CHECK(output.success == false);
            CHECK(output.valid_events == 0);
        }

        SUBCASE("Empty trace file") {
            std::string test_dir =
                "/tmp/test_metadata_empty_" + std::to_string(getpid());
            fs::create_directories(test_dir);

            std::string empty_file = test_dir + "/empty.trace";
            std::ofstream ofs(empty_file);
            ofs.close();

            auto input = MetadataCollectorUtilityInput::from_file(empty_file);

            MetadataCollectorUtility collector;
            auto output = collector.process(input);

            // Empty file should still succeed but with 0 events
            CHECK(output.file_path == empty_file);
            CHECK(output.valid_events == 0);
            CHECK(output.size_mb == 0);

            fs::remove_all(test_dir);
        }
    }
}
