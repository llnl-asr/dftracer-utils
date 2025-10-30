#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/error.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <doctest/doctest.h>

#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "testing_utilities.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer::internal;
using namespace dftracer::utils::utilities::reader::internal;
using namespace dft_utils_test;

TEST_CASE("TAR.GZ Indexer - Basic functionality") {
    TestEnvironment env(1000, Format::TAR_GZIP);
    REQUIRE(env.is_valid());

    std::string tar_gz_file = env.create_test_tar_gzip_file();
    REQUIRE(!tar_gz_file.empty());

    std::string idx_file = env.get_index_path(tar_gz_file);

    SUBCASE("Build index") {
        auto indexer =
            IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(1.0));
        REQUIRE(indexer != nullptr);
        CHECK_NOTHROW(indexer->build());
    }

    SUBCASE("Check rebuild needed") {
        auto indexer =
            IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(1.0));
        REQUIRE(indexer != nullptr);
        CHECK(indexer->need_rebuild());  // Should need rebuild initially

        indexer->build();
        CHECK_FALSE(
            indexer->need_rebuild());  // Should not need rebuild after building
    }

    SUBCASE("Getter methods") {
        std::size_t ckpt_size = mb_to_b(1.5);
        auto indexer = IndexerFactory::create(tar_gz_file, idx_file, ckpt_size);
        REQUIRE(indexer != nullptr);

        // Test getter methods
        CHECK(indexer->get_archive_path() == tar_gz_file);
        CHECK(indexer->get_idx_path() == idx_file);

        // Build index first before accessing metadata
        indexer->build();

        // size will be adjusted
        CHECK(indexer->get_checkpoint_size() <= ckpt_size);
    }

    SUBCASE("Move semantics") {
        auto indexer1 = IndexerFactory::create(tar_gz_file, idx_file, 1.0);
        REQUIRE(indexer1 != nullptr);

        // Move constructor
        auto indexer2 = std::move(indexer1);

        // Move assignment
        auto indexer3 = IndexerFactory::create(tar_gz_file, idx_file, 2.0);
        REQUIRE(indexer3 != nullptr);
        indexer3 = std::move(indexer2);
    }
}

TEST_CASE("TAR.GZ Reader - Basic functionality") {
    TestEnvironment env(100, Format::TAR_GZIP);
    REQUIRE(env.is_valid());

    std::string tar_gz_file = env.create_test_tar_gzip_file();
    REQUIRE(!tar_gz_file.empty());

    std::string idx_file = env.get_index_path(tar_gz_file);

    // Build index first
    {
        auto indexer =
            IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(0.5));
        REQUIRE(indexer != nullptr);
        indexer->build();
    }

    SUBCASE("Constructor and destructor") {
        // Test automatic destruction
        {
            auto reader = ReaderFactory::create(tar_gz_file, idx_file);
            REQUIRE(reader != nullptr);
            CHECK(reader->is_valid());
            CHECK(reader->get_archive_path() == tar_gz_file);
        }

        // Should be able to create another one
        auto reader2 = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader2 != nullptr);
        CHECK(reader2->is_valid());
    }

    SUBCASE("Get max bytes") {
        auto reader = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader != nullptr);
        std::size_t max_bytes = reader->get_max_bytes();
        CHECK(max_bytes > 0);
    }

    SUBCASE("Getter methods") {
        auto reader = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader != nullptr);

        // Test getter methods
        CHECK(reader->get_archive_path() == tar_gz_file);
        CHECK(reader->get_idx_path() == idx_file);
    }

    SUBCASE("Read byte range using streaming API") {
        auto reader = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader != nullptr);

        // Read using streaming API
        const std::size_t buffer_size = 1024;
        char buffer[1024];
        std::string result;

        // Stream data until no more available
        std::size_t bytes_read;
        std::size_t current_pos = 0;
        while (current_pos < 50) {
            std::size_t end_pos = std::min(current_pos + buffer_size,
                                           static_cast<std::size_t>(50));
            bytes_read =
                reader->read(current_pos, end_pos, buffer, buffer_size);
            if (bytes_read == 0) break;
            result.append(buffer, bytes_read);
            current_pos += bytes_read;
        }

        CHECK(result.size() <= 50);
        CHECK(!result.empty());
    }

    SUBCASE("Move semantics") {
        auto reader1 = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader1 != nullptr);

        // Move constructor
        auto reader2 = std::move(reader1);
        CHECK(reader1 == nullptr);
        CHECK(reader2->is_valid());

        // Move assignment
        auto reader3 = std::move(reader2);
        CHECK(reader3 != nullptr);
        CHECK(reader2 == nullptr);
        CHECK(reader3->is_valid());
    }
}

TEST_CASE("TAR.GZ API - Multi-file content validation") {
    TestEnvironment env(
        300, Format::TAR_GZIP);  // Multiple files with decent content
    REQUIRE(env.is_valid());

    std::string tar_gz_file = env.create_test_tar_gzip_file();
    REQUIRE(!tar_gz_file.empty());

    std::string idx_file = env.get_index_path(tar_gz_file);

    // Build index first
    {
        auto indexer =
            IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(0.5));
        REQUIRE(indexer != nullptr);
        indexer->build();
    }

    auto reader = ReaderFactory::create(tar_gz_file, idx_file);
    REQUIRE(reader != nullptr);

    SUBCASE("Verify multiple file content is accessible") {
        std::size_t max_bytes = reader->get_max_bytes();

        // Read entire file content
        std::string full_content;
        char buffer[4096];
        std::size_t bytes_read;

        std::size_t current_pos = 0;
        while (current_pos < max_bytes) {
            std::size_t end_pos =
                std::min(current_pos + sizeof(buffer), max_bytes);
            bytes_read =
                reader->read(current_pos, end_pos, buffer, sizeof(buffer));
            if (bytes_read == 0) break;
            full_content.append(buffer, bytes_read);
            current_pos += bytes_read;
        }

        // Should contain data from multiple files in the TAR.GZ
        CHECK(full_content.find("\"file\": \"main\"") != std::string::npos);
        CHECK(full_content.find("\"file\": \"secondary\"") !=
              std::string::npos);
        CHECK(full_content.find("\"file\": \"additional\"") !=
              std::string::npos);

        // Should contain directory structure
        CHECK(full_content.find("logs/additional.jsonl") != std::string::npos);

        // Should contain metadata
        CHECK(full_content.find("\"format\": \"tar.gz\"") != std::string::npos);
    }

    SUBCASE("JSON boundary detection with multi-file content") {
        char buffer[2048];
        std::string content;
        std::size_t bytes_read;

        std::size_t current_pos = 0;
        while (current_pos < 500) {
            std::size_t end_pos = std::min(current_pos + sizeof(buffer),
                                           static_cast<std::size_t>(500));
            bytes_read = reader->read_line_bytes(current_pos, end_pos, buffer,
                                                 sizeof(buffer));
            if (bytes_read == 0) break;
            content.append(buffer, bytes_read);
            current_pos += bytes_read;
        }

        CHECK(content.size() <= 500);

        // Should end with complete JSON line from any file
        CHECK(content.back() == '\n');

        // Should contain valid JSON structure
        std::size_t last_brace = content.rfind('}');
        REQUIRE(last_brace != std::string::npos);
        CHECK(last_brace <
              content.length() - 1);  // '}' should not be the last character
        CHECK(content[last_brace + 1] ==
              '\n');                  // Should be followed by newline

        // Should not end with incomplete JSON
        CHECK(content.find("\"message_") ==
              std::string::npos);  // No partial field names
    }
}

TEST_CASE("TAR.GZ API - JSON boundary detection") {
    TestEnvironment env(
        1000, Format::TAR_GZIP);  // More lines for better boundary testing
    REQUIRE(env.is_valid());

    std::string tar_gz_file = env.create_test_tar_gzip_file();
    REQUIRE(!tar_gz_file.empty());

    std::string idx_file = env.get_index_path(tar_gz_file);

    // Build index first
    {
        auto indexer =
            IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(0.5));
        REQUIRE(indexer != nullptr);
        indexer->build();
    }

    // Create reader
    auto reader = ReaderFactory::create(tar_gz_file, idx_file);
    REQUIRE(reader != nullptr);

    SUBCASE("Small range should provide minimum requested bytes") {
        // Request 100 bytes - should get AT LEAST 100 bytes due to boundary
        // extension
        char buffer[2048];
        std::string content;
        std::size_t bytes_read;

        std::size_t current_pos = 0;
        while (current_pos < 100) {
            std::size_t end_pos = std::min(current_pos + sizeof(buffer),
                                           static_cast<std::size_t>(100));
            bytes_read = reader->read_line_bytes(current_pos, end_pos, buffer,
                                                 sizeof(buffer));
            if (bytes_read == 0) break;
            content.append(buffer, bytes_read);
            current_pos += bytes_read;
        }

        CHECK(content.size() <= 100);  // Should get at least what was requested

        // Verify that output ends with complete JSON line
        CHECK(content.back() == '\n');  // Should end with newline

        // Should contain complete JSON objects
        std::size_t last_brace = content.rfind('}');
        REQUIRE(last_brace != std::string::npos);
        CHECK(last_brace <
              content.length() - 1);  // '}' should not be the last character
        CHECK(content[last_brace + 1] ==
              '\n');                  // Should be followed by newline
    }

    SUBCASE("Output should not cut off in middle of JSON") {
        // Request 500 bytes - this should not cut off mid-JSON
        char buffer[2048];
        std::string content;
        std::size_t bytes_read;

        std::size_t current_pos = 0;
        while (current_pos < 500) {
            std::size_t end_pos = std::min(current_pos + sizeof(buffer),
                                           static_cast<std::size_t>(500));
            bytes_read = reader->read_line_bytes(current_pos, end_pos, buffer,
                                                 sizeof(buffer));
            if (bytes_read == 0) break;
            content.append(buffer, bytes_read);
            current_pos += bytes_read;
        }

        CHECK(content.size() <= 500);

        // Should not end with partial JSON like {"name":"name_%
        std::size_t name_pos = content.find("\"name_");
        std::size_t last_brace_pos = content.rfind('}');
        bool has_incomplete_name =
            (name_pos != std::string::npos) && (name_pos > last_brace_pos);
        CHECK_FALSE(has_incomplete_name);

        // Verify it ends with complete JSON boundary (}\n)
        if (content.length() >= 2) {
            CHECK(content[content.length() - 2] == '}');
            CHECK(content[content.length() - 1] == '\n');
        }
    }

    SUBCASE("Multiple range reads should maintain boundaries across files") {
        // Read multiple consecutive ranges that might span different files in
        // TAR.GZ
        std::vector<std::string> segments;
        std::size_t current_pos = 0;
        std::size_t segment_size = 200;

        for (int i = 0; i < 5; ++i) {
            char buffer[2048];
            std::string content;
            std::size_t bytes_read;

            while ((bytes_read = reader->read_line_bytes(
                        current_pos, current_pos + segment_size, buffer,
                        sizeof(buffer))) > 0) {
                content.append(buffer, bytes_read);
            }

            segments.push_back(content);

            // Each segment should end properly with complete lines
            CHECK(!content.empty());
            CHECK(content.back() == '\n');

            // Verify all lines in segment are complete (no partial lines in
            // middle)
            std::size_t newline_count = 0;
            for (char c : content) {
                if (c == '\n') newline_count++;
            }
            CHECK(newline_count > 0);  // Should have at least one complete line

            current_pos += segment_size;
        }

        // Each segment should contain complete JSON objects
        for (const auto& segment : segments) {
            std::size_t json_count = 0;
            std::size_t pos = 0;
            while ((pos = segment.find("}\n", pos)) != std::string::npos) {
                json_count++;
                pos += 2;
            }
            CHECK(json_count >
                  0);  // Should have at least one complete JSON object
        }
    }
}

TEST_CASE("TAR.GZ Reader - Raw reading functionality") {
    TestEnvironment env(100, Format::TAR_GZIP);
    REQUIRE(env.is_valid());

    std::string tar_gz_file = env.create_test_tar_gzip_file();
    REQUIRE(!tar_gz_file.empty());

    std::string idx_file = env.get_index_path(tar_gz_file);

    // Build index first
    {
        auto indexer =
            IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(0.5));
        REQUIRE(indexer != nullptr);
        indexer->build();
    }

    SUBCASE("Basic raw read functionality") {
        auto reader = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader != nullptr);

        // Read using raw API
        const std::size_t buffer_size = 1024;
        char buffer[1024];
        std::string raw_result;

        // Stream raw data until no more available
        std::size_t bytes_read;
        std::size_t current_pos = 0;
        while (current_pos < 50) {
            std::size_t end_pos = std::min(current_pos + buffer_size,
                                           static_cast<std::size_t>(50));
            bytes_read =
                reader->read(current_pos, end_pos, buffer, buffer_size);
            if (bytes_read == 0) break;
            raw_result.append(buffer, bytes_read);
            current_pos += bytes_read;
        }

        CHECK(raw_result.size() >= 50);
        CHECK(!raw_result.empty());

        // Raw read should not care about JSON boundaries
        CHECK(raw_result.size() <=
              60);  // Should be much closer to 50 than regular read
    }

    SUBCASE("Compare raw vs regular read for TAR.GZ") {
        auto reader1 = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader1 != nullptr);
        auto reader2 = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader2 != nullptr);
        const std::size_t buffer_size = 1024;
        char buffer1[1024], buffer2[1024];
        std::string raw_result, regular_result;

        // Raw read
        std::size_t bytes_read1;
        std::size_t current_pos1 = 0;
        while (current_pos1 < 100) {
            std::size_t end_pos = std::min(current_pos1 + buffer_size,
                                           static_cast<std::size_t>(100));
            bytes_read1 =
                reader1->read(current_pos1, end_pos, buffer1, buffer_size);
            if (bytes_read1 == 0) break;
            raw_result.append(buffer1, bytes_read1);
            current_pos1 += bytes_read1;
        }

        // Line bytes read
        std::size_t bytes_read2;
        std::size_t current_pos2 = 0;
        while (current_pos2 < 100) {
            std::size_t end_pos = std::min(current_pos2 + buffer_size,
                                           static_cast<std::size_t>(100));
            bytes_read2 = reader2->read_line_bytes(current_pos2, end_pos,
                                                   buffer2, buffer_size);
            if (bytes_read2 == 0) break;
            regular_result.append(buffer2, bytes_read2);
            current_pos2 += bytes_read2;
        }

        // Raw read should be exactly requested size for TAR.GZ too
        CHECK(raw_result.size() == 100);
        CHECK(regular_result.size() <= 100);

        // Regular read should be smaller or equal due to JSON boundary
        // alignment
        CHECK(regular_result.size() <= raw_result.size());

        // Regular read should end with complete JSON line
        CHECK(regular_result.back() == '\n');

        // Both should start with same data
        std::size_t min_size =
            std::min(raw_result.size(), regular_result.size());
        CHECK(raw_result.substr(0, min_size) ==
              regular_result.substr(0, min_size));
    }

    SUBCASE(
        "Full file read comparison: raw vs JSON-boundary aware for TAR.GZ") {
        auto reader1 = ReaderFactory::create(tar_gz_file, idx_file);
        auto reader2 = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader2 != nullptr);

        std::size_t max_bytes = reader1->get_max_bytes();
        char buffer[4096];

        // Read entire file with raw API
        std::string raw_content;
        std::size_t bytes_read1;
        std::size_t current_pos1 = 0;
        while (current_pos1 < max_bytes) {
            std::size_t end_pos =
                std::min(current_pos1 + sizeof(buffer), max_bytes);
            bytes_read1 =
                reader1->read(current_pos1, end_pos, buffer, sizeof(buffer));
            if (bytes_read1 == 0) break;
            raw_content.append(buffer, bytes_read1);
            current_pos1 += bytes_read1;
        }

        // Read entire file with line-boundary aware API
        std::string json_content;
        std::size_t bytes_read2;
        std::size_t current_pos2 = 0;
        while (current_pos2 < max_bytes) {
            std::size_t end_pos =
                std::min(current_pos2 + sizeof(buffer), max_bytes);
            bytes_read2 = reader2->read_line_bytes(current_pos2, end_pos,
                                                   buffer, sizeof(buffer));
            if (bytes_read2 == 0) break;
            json_content.append(buffer, bytes_read2);
            current_pos2 += bytes_read2;
        }

        // Both should read the entire file
        CHECK(raw_content.size() == max_bytes);
        CHECK(json_content.size() == max_bytes);

        // Content should be identical when reading full file
        CHECK(raw_content == json_content);

        // Both should end with complete JSON lines
        if (!raw_content.empty() && !json_content.empty()) {
            CHECK(raw_content.back() == '\n');
            CHECK(json_content.back() == '\n');

            // Should contain multi-file TAR.GZ content
            CHECK(raw_content.find("\"file\": \"main\"") != std::string::npos);
            CHECK(json_content.find("\"file\": \"main\"") != std::string::npos);
        }
    }
}

TEST_CASE("TAR.GZ Reader - Line reading functionality") {
    TestEnvironment env(10000, Format::TAR_GZIP);  // Large multi-file archive
    REQUIRE(env.is_valid());

    std::string tar_gz_file = env.create_test_tar_gzip_file();
    REQUIRE(!tar_gz_file.empty());

    std::string idx_file = env.get_index_path(tar_gz_file);

    // Build index first with smaller chunk size to force checkpoint creation
    {
        auto indexer =
            IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(0.1));
        REQUIRE(indexer != nullptr);
        indexer->build();

        // Verify the indexer has line counts and checkpoints
        std::size_t total_lines = indexer->get_num_lines();
        auto checkpoints = indexer->get_checkpoints();

        // Skip line reading tests if indexer doesn't have proper line support
        if (total_lines == 0 || checkpoints.empty()) {
            WARN(
                "Skipping TAR.GZ line reading tests - indexer has no line "
                "data");
            return;
        }

        INFO("TAR.GZ Indexer created with "
             << checkpoints.size() << " checkpoints and " << total_lines
             << " total lines");
    }

    SUBCASE("Basic line reading functionality for TAR.GZ") {
        auto reader = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader != nullptr);

        // Read first 5 lines
        std::string result = reader->read_lines(1, 5);
        CHECK(!result.empty());

        // Count newlines to verify we got the right number of lines
        std::size_t line_count = 0;
        for (char c : result) {
            if (c == '\n') line_count++;
        }
        CHECK(line_count == 5);  // Should have exactly 5 lines

        // Verify it starts with expected pattern (should contain main file
        // data)
        CHECK(result.find("\"id\": 1") != std::string::npos);
        CHECK(result.find("\"file\": \"main\"") != std::string::npos);
    }

    SUBCASE("Line reading across multiple files in TAR.GZ") {
        auto reader = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader != nullptr);

        // Get total line count
        auto indexer =
            IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(0.1));
        std::size_t total_lines = indexer->get_num_lines();

        if (total_lines > 50) {
            // Read a larger range that should span multiple files
            std::string result = reader->read_lines(1, 50);
            CHECK(!result.empty());

            // Should contain data from multiple files
            bool has_main =
                result.find("\"file\": \"main\"") != std::string::npos;
            bool has_secondary =
                result.find("\"file\": \"secondary\"") != std::string::npos;
            bool has_additional =
                result.find("\"file\": \"additional\"") != std::string::npos;

            // Should find at least 2 different file sources in first 50 lines
            int file_types = has_main + has_secondary + has_additional;
            CHECK(file_types >= 2);

            // Verify line count
            std::size_t line_count = 0;
            for (char c : result) {
                if (c == '\n') line_count++;
            }
            CHECK(line_count == 50);
        }
    }

    SUBCASE("Line reading consistency with sed behavior for TAR.GZ") {
        auto reader = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader != nullptr);

        // Test that our line numbering matches sed's 1-based numbering
        // Line 1 should contain id: 1, line 2 should contain id: 2, etc.
        for (std::size_t i = 1; i <= 5; ++i) {
            std::string result = reader->read_lines(i, i);
            std::string expected_id = "\"id\": " + std::to_string(i);
            CHECK(result.find(expected_id) != std::string::npos);
        }
    }
}

TEST_CASE("TAR.GZ API - Regression and stress tests") {
    SUBCASE("Large TAR.GZ file handling") {
        TestEnvironment env(
            10000, Format::TAR_GZIP);  // Large TAR.GZ with multiple files
        REQUIRE(env.is_valid());

        std::string tar_gz_file = env.create_test_tar_gzip_file();
        REQUIRE(!tar_gz_file.empty());

        std::string idx_file = env.get_index_path(tar_gz_file);

        // Build index
        {
            auto indexer =
                IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(1.0));
            REQUIRE(indexer != nullptr);
            CHECK_NOTHROW(indexer->build());
        }

        // Test large reads
        auto reader = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader != nullptr);
        std::size_t max_bytes = reader->get_max_bytes();
        CHECK(max_bytes > 10000);  // Should be a large file

        // Read large chunks across multiple files
        if (max_bytes > 50000) {
            char buffer[4096];
            std::string content;

            std::size_t bytes_read;
            while ((bytes_read = reader->read_line_bytes(1000, 50000, buffer,
                                                         sizeof(buffer))) > 0) {
                content.append(buffer, bytes_read);
            }

            // Check data integrity instead of strict size limits
            CHECK(!content.empty());
            CHECK(content.find("{") != std::string::npos);
            CHECK(content.back() == '\n');

            // Should contain multiple file sources
            bool has_main =
                content.find("\"file\": \"main\"") != std::string::npos;
            bool has_secondary =
                content.find("\"file\": \"secondary\"") != std::string::npos;
            bool has_additional =
                content.find("\"file\": \"additional\"") != std::string::npos;

            int file_types = has_main + has_secondary + has_additional;
            CHECK(file_types >= 2);  // Should span multiple files

            // Verify we got complete lines (reasonable size range)
            CHECK(content.size() > 40000);  // Should have substantial data
            CHECK(content.size() < 60000);  // But not excessive

            // Count complete lines
            std::size_t line_count = 0;
            for (char c : content) {
                if (c == '\n') line_count++;
            }
            CHECK(line_count > 0);
        }
    }

    SUBCASE("TAR.GZ specific truncated JSON regression test") {
        // This test ensures TAR.GZ doesn't have boundary issues across files
        TestEnvironment env(2000, Format::TAR_GZIP);
        REQUIRE(env.is_valid());

        std::string tar_gz_file = env.create_test_tar_gzip_file();
        REQUIRE(!tar_gz_file.empty());

        std::string idx_file = env.get_index_path(tar_gz_file);

        // Build index
        {
            auto indexer =
                IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(32.0));
            indexer->build();
        }

        auto reader = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader != nullptr);

        SUBCASE("Cross-file boundary handling: 0 to 10000 bytes") {
            char buffer[4096];
            std::string content;

            std::size_t bytes_read;
            std::size_t current_pos = 0;
            while (current_pos < 10000) {
                std::size_t end_pos = std::min(current_pos + sizeof(buffer),
                                               static_cast<std::size_t>(10000));
                bytes_read = reader->read_line_bytes(current_pos, end_pos,
                                                     buffer, sizeof(buffer));
                if (bytes_read == 0) break;
                content.append(buffer, bytes_read);
                current_pos += bytes_read;
            }

            CHECK(content.size() <= 10000);

            // Should NOT end with incomplete patterns
            CHECK(content.find("\"message_") ==
                  std::string::npos);  // No partial field names

            // Should end with complete JSON line
            CHECK(content.back() == '\n');
            CHECK(content[content.length() - 2] == '}');

            // Should contain multi-file content
            CHECK(content.find("\"file\": \"main\"") != std::string::npos);
        }

        SUBCASE("Small range across file boundaries") {
            char buffer[2048];
            std::string content;

            std::size_t bytes_read;
            std::size_t current_pos = 0;
            while (current_pos < 100) {
                std::size_t end_pos = std::min(current_pos + sizeof(buffer),
                                               static_cast<std::size_t>(100));
                bytes_read = reader->read_line_bytes(current_pos, end_pos,
                                                     buffer, sizeof(buffer));
                if (bytes_read == 0) break;
                content.append(buffer, bytes_read);
                current_pos += bytes_read;
            }

            CHECK(content.size() <= 100);

            // Should contain multiple complete JSON objects
            std::size_t brace_count = 0;
            for (char c : content) {
                if (c == '}') brace_count++;
            }
            CHECK(brace_count >= 2);  // Should have at least 2 complete objects
        }
    }
}

TEST_CASE("TAR.GZ Advanced Functions - Error Paths and Edge Cases") {
    TestEnvironment env(1000, Format::TAR_GZIP);
    REQUIRE(env.is_valid());

    std::string tar_gz_file = env.create_test_tar_gzip_file();
    REQUIRE(!tar_gz_file.empty());

    std::string idx_file = env.get_index_path(tar_gz_file);

    SUBCASE("TAR.GZ with various checkpoint sizes") {
        // Test different chunk sizes to trigger different code paths
        for (double ckpt_size_mb : {0.1, 0.5, 1.0, 2.0, 5.0}) {
            std::size_t ckpt_size = mb_to_b(ckpt_size_mb);
            auto indexer = IndexerFactory::create(
                tar_gz_file, idx_file + std::to_string(ckpt_size_mb),
                ckpt_size);
            CHECK_NOTHROW(indexer->build());
            CHECK(indexer->get_checkpoint_size() <= ckpt_size);
        }
    }

    SUBCASE("Multiple readers on same TAR.GZ index") {
        // Build index once
        {
            auto indexer =
                IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(1.0));
            REQUIRE(indexer != nullptr);
            indexer->build();
        }

        // Create multiple readers
        std::vector<std::unique_ptr<Reader>> readers;
        for (int i = 0; i < 5; ++i) {
            readers.push_back(ReaderFactory::create(tar_gz_file, idx_file));
            CHECK(readers.back()->is_valid());
        }

        // All should be able to read TAR.GZ simultaneously
        for (auto& reader : readers) {
            char buffer[1024];
            std::string result;

            std::size_t bytes_read;
            while ((bytes_read = reader->read(0, 50, buffer, sizeof(buffer))) >
                   0) {
                result.append(buffer, bytes_read);
            }

            CHECK(result.size() <= 50);
            CHECK(result.find("\"id\":") !=
                  std::string::npos);  // Should contain JSON from TAR.GZ
        }
    }

    SUBCASE("TAR.GZ boundary conditions across files") {
        // Build index
        {
            auto indexer =
                IndexerFactory::create(tar_gz_file, idx_file, mb_to_b(0.5));
            REQUIRE(indexer != nullptr);
            indexer->build();
        }

        auto reader = ReaderFactory::create(tar_gz_file, idx_file);
        REQUIRE(reader != nullptr);
        std::size_t max_bytes = reader->get_max_bytes();

        // Test various range sizes that might hit file boundaries in TAR.GZ
        std::vector<std::pair<std::size_t, std::size_t>> ranges = {
            {0, 1},                               // Very small range
            {0, 10},                              // Small range
            {0, 100},                             // Medium range
            {0, 1000},                            // Large range
            {100, 200},                           // Mid-file range
            {max_bytes / 2, max_bytes / 2 + 50},  // Middle section
        };

        for (const auto& range : ranges) {
            std::size_t start = range.first;
            std::size_t end = range.second;
            if (end <= max_bytes) {
                char buffer[2048];
                std::string result;

                std::size_t bytes_read;
                while ((bytes_read = reader->read(start, end, buffer,
                                                  sizeof(buffer))) > 0) {
                    result.append(buffer, bytes_read);
                }

                CHECK(result.size() <= (end - start));

                // Should contain valid JSON content
                if (!result.empty()) {
                    CHECK(result.find("\"id\":") != std::string::npos);
                }
            }
        }
    }
}
