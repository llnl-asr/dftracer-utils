// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/archive_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
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

using namespace dftracer::utils;
using namespace dftracer::utils::trace;

// Helper to create a test trace file
static dstd::String create_test_trace_file(const dstd::String& dir,
                                           int num_events) {
    dstd::String file_path = dir + "/test.trace.gz";
    dftu_utils_test::TextOut ofs;

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
        dstd::String test_dir = dstd::String(
            dftu_utils_test::make_unique_test_path("test_metadata_collector")
                .str());
        REQUIRE(dstd::fs::create_directories(test_dir).has_value());

        SUBCASE("Small trace file") {
            // Create a test trace file with 10 events
            dstd::String trace_file = create_test_trace_file(test_dir, 10);

            // Create input
            auto input = MetadataCollectorUtilityInput::from_file(
                             dstd::String(trace_file))
                             .with_compute_hash(true);

            // Process
            MetadataCollectorUtility collector;
            auto output = collector(input).get();

            // Verify output
            CHECK(output.success == true);
            CHECK(dstd::String(output.file_path) == trace_file);
            CHECK(output.valid_events == 10);
            CHECK(output.end_line >= 9);  // Should have at least 10 lines (0-9)
            CHECK(output.size_mb > 0);
            CHECK(output.size_per_line > 0);
        }

        SUBCASE("Large trace file with checkpoint") {
            // Create larger trace file
            dstd::String trace_file = create_test_trace_file(test_dir, 1000);

            // Create input with small checkpoint size
            auto input = MetadataCollectorUtilityInput::from_file(
                             dstd::String(trace_file))
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
            const dstd::String own_dir = dstd::String(
                dftu_utils_test::make_unique_test_path("metadata_no_index")
                    .str());
            REQUIRE(dstd::fs::create_directories(own_dir).has_value());
            dstd::String trace_file = create_test_trace_file(own_dir, 100);

            // Create input
            auto input = MetadataCollectorUtilityInput::from_file(
                             dstd::String(trace_file))
                             .with_compute_hash(true);

            // Process
            MetadataCollectorUtility collector;
            auto output = collector(input).get();

            // Verify extended metadata fields
            CHECK(output.success == true);
            CHECK(dstd::String(output.file_path) == trace_file);
            CHECK(output.valid_events == 100);

            // Collecting metadata for a gzip trace builds its index.
            CHECK(output.format == common::ArchiveFormat::Gzip);
            CHECK(output.has_index == true);
            CHECK(output.compressed_size > 0);
            CHECK(output.uncompressed_size > 0);
            CHECK(output.compressed_size < output.uncompressed_size);
            CHECK(output.num_lines == 100);
            CHECK(output.error_message.empty());
        }

        // Clean up
        dstd::fs::remove_all(test_dir).has_value();
    }

    TEST_CASE("MetadataCollector - Extended fields for compressed files") {
        // Create temp directory
        dstd::String test_dir = dstd::String(
            dftu_utils_test::make_unique_test_path("test_metadata_compressed")
                .str());
        REQUIRE(dstd::fs::create_directories(test_dir).has_value());

        SUBCASE("Compressed file with index - extended metadata") {
            dstd::String gz_file = create_test_trace_file(test_dir, 50);
            REQUIRE(dstd::fs::exists(gz_file).value());

            // Create input with index
            dstd::String index_path =
                dstd::String(internal::determine_index_path(gz_file, ""));
            auto input =
                MetadataCollectorUtilityInput::from_file(dstd::String(gz_file))
                    .with_index(dstd::String(index_path))
                    .with_checkpoint_size(1024 * 1024)  // 1MB
                    .with_force_rebuild(true)
                    .with_compute_hash(true);

            // Process
            MetadataCollectorUtility collector;
            auto output = collector(input).get();

            // Verify basic fields
            CHECK(output.success == true);
            CHECK(dstd::String(output.file_path) == gz_file);
            CHECK(output.valid_events > 0);

            // Verify extended fields for compressed files
            CHECK(output.format == common::ArchiveFormat::Gzip);
            CHECK(output.has_index == true);
            CHECK(output.index_valid == true);
            CHECK(dstd::String(output.index_path) == index_path);
            CHECK(output.compressed_size > 0);
            CHECK(output.uncompressed_size > 0);
            CHECK(output.compressed_size <
                  output.uncompressed_size);  // Should be compressed
            CHECK(output.num_lines == 50);
            CHECK(output.checkpoint_size > 0);
            CHECK(output.error_message.empty());

            // Verify index file was created
            CHECK(dstd::fs::exists(index_path).value());
        }

        SUBCASE("Reuse existing index") {
            dstd::String gz_file = create_test_trace_file(test_dir, 25);

            dstd::String index_path =
                dstd::String(internal::determine_index_path(gz_file, ""));

            // First run - build index
            {
                auto input = MetadataCollectorUtilityInput::from_file(
                                 dstd::String(gz_file))
                                 .with_index(dstd::String(index_path))
                                 .with_force_rebuild(true)
                                 .with_compute_hash(true);

                MetadataCollectorUtility collector;
                auto output = collector(input).get();

                CHECK(output.success == true);
                CHECK(output.has_index == true);
                CHECK(output.index_valid == true);
                CHECK(dstd::fs::exists(index_path).value());
            }

            // Second run - reuse index (no force rebuild)
            {
                auto input = MetadataCollectorUtilityInput::from_file(
                                 dstd::String(gz_file))
                                 .with_index(dstd::String(index_path))
                                 .with_force_rebuild(false)
                                 .with_compute_hash(true);

                MetadataCollectorUtility collector;
                auto output = collector(input).get();

                CHECK(output.success == true);
                CHECK(output.has_index == true);
                CHECK(output.index_valid == true);
                CHECK(output.format == common::ArchiveFormat::Gzip);
                CHECK(output.num_lines == 25);
            }
        }

        // Clean up
        dstd::fs::remove_all(test_dir).has_value();
    }

    TEST_CASE(
        "MetadataCollector - force rebuild keeps the other index entries") {
        const dstd::String dir = dstd::String(
            dftu_utils_test::make_unique_test_path("metadata_keep_entries")
                .str());
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String gz_file = create_test_trace_file(dir, 25);
        const dstd::String root =
            dstd::String(internal::determine_index_path(gz_file, ""));

        auto first =
            MetadataCollectorUtilityInput::from_file(dstd::String(gz_file))
                .with_index(dstd::String(root))
                .with_force_rebuild(true);
        MetadataCollectorUtility collector;
        REQUIRE(collector(first).get().success);

        REQUIRE(dstd::fs::create_directories(dstd::Path(root) / "rollups")
                    .has_value());
        REQUIRE(dstd::fs::create_directories(dstd::Path(root) / "views" / "x")
                    .has_value());
        dftu_utils_test::FileOut(dstd::Path(root) / "rollups" / "marker")
            << "r";
        dftu_utils_test::FileOut(dstd::Path(root) / "views" / "x" / "marker")
            << "v";
        dftu_utils_test::FileOut(dstd::Path(root) / "store" / "stale_marker")
            << "s";

        auto second =
            MetadataCollectorUtilityInput::from_file(dstd::String(gz_file))
                .with_index(dstd::String(root))
                .with_force_rebuild(true);
        const auto output = collector(second).get();
        CHECK(output.success);
        CHECK(output.num_lines == 25);
        CHECK(
            dstd::fs::exists(dstd::Path(root) / "rollups" / "marker").value());
        CHECK(dstd::fs::exists(dstd::Path(root) / "views" / "x" / "marker")
                  .value());
        CHECK_FALSE(
            dstd::fs::exists(dstd::Path(root) / "store" / "stale_marker")
                .value());
        (void)dstd::fs::remove_all(dir);
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
            dstd::String test_dir = dstd::String(
                dftu_utils_test::make_unique_test_path("test_metadata_empty")
                    .str());
            REQUIRE(dstd::fs::create_directories(test_dir).has_value());

            dstd::String empty_file = test_dir + "/empty.trace";
            dftu_utils_test::FileOut ofs(empty_file);
            ofs.close();

            auto input = MetadataCollectorUtilityInput::from_file(
                dstd::String(empty_file));

            MetadataCollectorUtility collector;
            auto output = collector(input).get();

            // Empty file should still succeed but with 0 events
            CHECK(dstd::String(output.file_path) == empty_file);
            CHECK(output.valid_events == 0);
            CHECK(output.size_mb == 0);

            (void)dstd::fs::remove_all(test_dir);
        }
    }
}

TEST_SUITE("MetadataCollectorRoot") {
    TEST_CASE(
        "fresh files of one root are checked alongside a reader, and a "
        "build waits for readers") {
        const dstd::String dir = dstd::String(
            dftu_utils_test::make_unique_test_path("test_metadata_root").str());
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String index_path = dir + "/root.dftindex";
        constexpr int FILES = 8;

        dstd::Vector<dstd::String> files;
        for (int i = 0; i < FILES; ++i) {
            const dstd::String sub = dir + "/f" + dstd::to_string(i);
            REQUIRE(dstd::fs::create_directories(sub).has_value());
            files.push_back(create_test_trace_file(sub, 20 + i));
        }
        auto collect = [&](const dstd::String& file) {
            MetadataCollectorUtility collector;
            return collector(MetadataCollectorUtilityInput::from_file(
                                 dstd::String(file))
                                 .with_index(dstd::String(index_path)))
                .get();
        };
        for (const auto& file : files) REQUIRE(collect(file).success);

        // The collector keys the root by its normalized path.
        auto root = internal::index_root(dstd::String(dstd::String(
            index::store::internal::normalize_index_root(index_path))));
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

        const dstd::String sub = dir + "/new";
        REQUIRE(dstd::fs::create_directories(sub).has_value());
        const dstd::String fresh = create_test_trace_file(sub, 7);
        auto build = std::async(std::launch::async, collect, fresh);
        CHECK(build.wait_for(std::chrono::milliseconds(300)) ==
              std::future_status::timeout);
        root->gate.release(1);
        REQUIRE(build.wait_for(std::chrono::seconds(10)) ==
                std::future_status::ready);
        const auto built = build.get();
        CHECK(built.success);
        CHECK(built.num_lines == 7);

        (void)dstd::fs::remove_all(dir);
    }
}
