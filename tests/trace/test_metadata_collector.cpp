#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/archive_format.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/internal/index_root.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::trace;

// Helper to create a test trace file
static std::string create_test_trace_file(const std::string& dir,
                                          int num_events) {
    std::string file_path = dir + "/test.trace.gz";
    std::ostringstream ofs;

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
    return dftu_utils_test::write_gz_trace(file_path, ofs.str());
}

TEST_SUITE("MetadataCollector") {
    TEST_CASE("MetadataCollector - Collect from a gzip trace file") {
        // Create temp directory
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_metadata_collector")
                .string();
        fs::create_directories(test_dir);

        SUBCASE("Small trace file") {
            // Create a test trace file with 10 events
            std::string trace_file = create_test_trace_file(test_dir, 10);

            // Create input
            auto input = MetadataCollectorUtilityInput::from_file(trace_file)
                             .with_compute_hash(true);

            // Process
            MetadataCollectorUtility collector;
            auto output = collector(input).get();

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
                             .with_force_rebuild(true)
                             .with_compute_hash(true);

            // Process
            MetadataCollectorUtility collector;
            auto output = collector(input).get();

            // Verify
            CHECK(output.success == true);
            CHECK(output.valid_events == 1000);
            CHECK(output.size_mb > 0);
        }

        SUBCASE("Extended metadata fields") {
            const std::string own_dir =
                dftu_utils_test::make_unique_test_path("metadata_no_index")
                    .string();
            fs::create_directories(own_dir);
            std::string trace_file = create_test_trace_file(own_dir, 100);

            // Create input
            auto input = MetadataCollectorUtilityInput::from_file(trace_file)
                             .with_compute_hash(true);

            // Process
            MetadataCollectorUtility collector;
            auto output = collector(input).get();

            // Verify extended metadata fields
            CHECK(output.success == true);
            CHECK(output.file_path == trace_file);
            CHECK(output.valid_events == 100);

            // Collecting metadata for a gzip trace builds its index.
            CHECK(output.format == ArchiveFormat::Gzip);
            CHECK(output.has_index == true);
            CHECK(output.compressed_size > 0);
            CHECK(output.uncompressed_size > 0);
            CHECK(output.compressed_size < output.uncompressed_size);
            CHECK(output.num_lines == 100);
            CHECK(output.error_message.empty());
        }

        // Clean up
        fs::remove_all(test_dir);
    }

    TEST_CASE("MetadataCollector - Extended fields for compressed files") {
        // Create temp directory
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_metadata_compressed")
                .string();
        fs::create_directories(test_dir);

        SUBCASE("Compressed file with index - extended metadata") {
            std::string gz_file = create_test_trace_file(test_dir, 50);
            REQUIRE(fs::exists(gz_file));

            // Create input with index
            std::string index_path =
                internal::determine_index_path(gz_file, "");
            auto input = MetadataCollectorUtilityInput::from_file(gz_file)
                             .with_index(index_path)
                             .with_checkpoint_size(1024 * 1024)  // 1MB
                             .with_force_rebuild(true)
                             .with_compute_hash(true);

            // Process
            MetadataCollectorUtility collector;
            auto output = collector(input).get();

            // Verify basic fields
            CHECK(output.success == true);
            CHECK(output.file_path == gz_file);
            CHECK(output.valid_events > 0);

            // Verify extended fields for compressed files
            CHECK(output.format == ArchiveFormat::Gzip);
            CHECK(output.has_index == true);
            CHECK(output.index_valid == true);
            CHECK(output.index_path == index_path);
            CHECK(output.compressed_size > 0);
            CHECK(output.uncompressed_size > 0);
            CHECK(output.compressed_size <
                  output.uncompressed_size);  // Should be compressed
            CHECK(output.num_lines == 50);
            CHECK(output.checkpoint_size > 0);
            CHECK(output.error_message.empty());

            // Verify index file was created
            CHECK(fs::exists(index_path));
        }

        SUBCASE("Reuse existing index") {
            std::string gz_file = create_test_trace_file(test_dir, 25);

            std::string index_path =
                internal::determine_index_path(gz_file, "");

            // First run - build index
            {
                auto input = MetadataCollectorUtilityInput::from_file(gz_file)
                                 .with_index(index_path)
                                 .with_force_rebuild(true)
                                 .with_compute_hash(true);

                MetadataCollectorUtility collector;
                auto output = collector(input).get();

                CHECK(output.success == true);
                CHECK(output.has_index == true);
                CHECK(output.index_valid == true);
                CHECK(fs::exists(index_path));
            }

            // Second run - reuse index (no force rebuild)
            {
                auto input = MetadataCollectorUtilityInput::from_file(gz_file)
                                 .with_index(index_path)
                                 .with_force_rebuild(false)
                                 .with_compute_hash(true);

                MetadataCollectorUtility collector;
                auto output = collector(input).get();

                CHECK(output.success == true);
                CHECK(output.has_index == true);
                CHECK(output.index_valid == true);
                CHECK(output.format == ArchiveFormat::Gzip);
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
                output = collector(input).get();
            } catch (...) {
                // noop
            }

            CHECK(output.success == false);
            CHECK(output.valid_events == 0);
        }

        SUBCASE("Empty trace file") {
            std::string test_dir =
                dftu_utils_test::make_unique_test_path("test_metadata_empty")
                    .string();
            fs::create_directories(test_dir);

            std::string empty_file = test_dir + "/empty.trace";
            std::ofstream ofs(empty_file);
            ofs.close();

            auto input = MetadataCollectorUtilityInput::from_file(empty_file);

            MetadataCollectorUtility collector;
            auto output = collector(input).get();

            // Empty file should still succeed but with 0 events
            CHECK(output.file_path == empty_file);
            CHECK(output.valid_events == 0);
            CHECK(output.size_mb == 0);

            fs::remove_all(test_dir);
        }
    }
}

TEST_SUITE("MetadataCollectorRoot") {
    TEST_CASE(
        "fresh files of one root are checked alongside a reader, and a "
        "build waits for readers") {
        const std::string dir =
            dftu_utils_test::make_unique_test_path("test_metadata_root")
                .string();
        fs::create_directories(dir);
        const std::string index_path = dir + "/root.dftindex";
        constexpr int FILES = 8;

        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i) {
            const std::string sub = dir + "/f" + std::to_string(i);
            fs::create_directories(sub);
            files.push_back(create_test_trace_file(sub, 20 + i));
        }
        auto collect = [&](const std::string& file) {
            MetadataCollectorUtility collector;
            return collector(MetadataCollectorUtilityInput::from_file(file)
                                 .with_index(index_path))
                .get();
        };
        for (const auto& file : files) REQUIRE(collect(file).success);

        // The collector keys the root by its normalized path.
        auto root = internal::index_root(
            index::store::internal::normalize_index_root(index_path));
        // Forget what this process saw, so each check reads the index.
        {
            std::lock_guard<std::mutex> lock(root->current_mutex);
            root->current.clear();
        }
        // Another reader of the root, such as a freshness check in flight.
        REQUIRE(root->gate.try_acquire(1));

        for (int i = 0; i < FILES; ++i) {
            auto future = std::async(std::launch::async, collect, files[i]);
            REQUIRE(future.wait_for(std::chrono::seconds(10)) ==
                    std::future_status::ready);
            const auto out = future.get();
            CHECK(out.success);
            CHECK(out.num_lines == static_cast<std::size_t>(20 + i));
        }

        const std::string sub = dir + "/new";
        fs::create_directories(sub);
        const std::string fresh = create_test_trace_file(sub, 7);
        auto build = std::async(std::launch::async, collect, fresh);
        CHECK(build.wait_for(std::chrono::milliseconds(300)) ==
              std::future_status::timeout);
        root->gate.release(1);
        REQUIRE(build.wait_for(std::chrono::seconds(10)) ==
                std::future_status::ready);
        const auto built = build.get();
        CHECK(built.success);
        CHECK(built.num_lines == 7);

        fs::remove_all(dir);
    }
}
