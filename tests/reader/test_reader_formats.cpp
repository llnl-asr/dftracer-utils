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

using namespace dft_utils_test;
using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer::internal;
using namespace dftracer::utils::utilities::reader::internal;

std::string format_name(Format format);

// Parameterized test fixture for different formats
class FormatTestFixture {
   public:
    explicit FormatTestFixture(Format format)
        : format_(format), env_(100, format) {
        REQUIRE(env_.is_valid());
        test_file_ = env_.create_test_file();
        REQUIRE(!test_file_.empty());
        index_file_ = env_.get_index_path(test_file_);
    }

    Format get_format() const { return format_; }
    const std::string& get_test_file() const { return test_file_; }
    const std::string& get_index_file() const { return index_file_; }
    const TestEnvironment& get_env() const { return env_; }

   private:
    Format format_;
    TestEnvironment env_;
    std::string test_file_;
    std::string index_file_;
};

// Helper function to get format name for test output
std::string format_name(Format format) {
    switch (format) {
        case Format::GZIP:
            return "gzip";
        case Format::TAR_GZIP:
            return "tar.gz";
        default:
            return "unknown";
    }
}

// Template wrapper types for doctest parameterization
template <Format F>
struct FormatWrapper {
    static constexpr Format value = F;
};

using GZIPFormat = FormatWrapper<Format::GZIP>;
using TARGZIPFormat = FormatWrapper<Format::TAR_GZIP>;

// Parameterized tests using doctest's approach
TEST_CASE_TEMPLATE("Indexer creation and destruction", FormatType, GZIPFormat,
                   TARGZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    SUBCASE("Basic indexer creation") {
        auto indexer = IndexerFactory::create(
            fixture.get_test_file(), fixture.get_index_file(), 1024 * 1024);
        REQUIRE(indexer != nullptr);
        CHECK(indexer->exists());
    }

    SUBCASE("Invalid file path") {
        CHECK_THROWS_AS(
            IndexerFactory::create("/nonexistent/file.gz",
                                   fixture.get_index_file(), 1024 * 1024),
            IndexerError);
    }
}

TEST_CASE_TEMPLATE("Index building", FormatType, GZIPFormat, TARGZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    SUBCASE("Basic index building") {
        auto indexer = IndexerFactory::create(
            fixture.get_test_file(), fixture.get_index_file(), 1024 * 1024);
        REQUIRE(indexer != nullptr);

        indexer->build();
        CHECK(indexer->get_num_lines() > 0);
    }

    SUBCASE("Rebuild detection") {
        auto indexer = IndexerFactory::create(
            fixture.get_test_file(), fixture.get_index_file(), 1024 * 1024);
        REQUIRE(indexer != nullptr);

        // First build
        indexer->build();

        // Check if rebuild is needed (should not be needed after fresh build)
        CHECK_FALSE(indexer->need_rebuild());
    }

    SUBCASE("Force rebuild") {
        auto indexer =
            IndexerFactory::create(fixture.get_test_file(),
                                   fixture.get_index_file(), 1024 * 1024, true);
        REQUIRE(indexer != nullptr);

        indexer->build();
    }
}

TEST_CASE_TEMPLATE("Reader creation and basic functionality", FormatType,
                   GZIPFormat, TARGZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    auto indexer = IndexerFactory::create(
        fixture.get_test_file(), fixture.get_index_file(), 1024 * 1024);
    REQUIRE(indexer != nullptr);
    indexer->build();

    SUBCASE("Reader creation with indexer") {
        auto reader = ReaderFactory::create(indexer);
        CHECK(reader != nullptr);
        CHECK(reader->is_valid());
    }

    SUBCASE("Reader creation from files") {
        auto reader = ReaderFactory::create(
            fixture.get_test_file(), fixture.get_index_file(), 1024 * 1024);
        CHECK(reader != nullptr);
        CHECK(reader->is_valid());
    }
}

TEST_CASE_TEMPLATE("Data reading operations", FormatType, GZIPFormat,
                   TARGZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    auto indexer = IndexerFactory::create(fixture.get_test_file(),
                                          fixture.get_index_file(), 512 * 1024);
    REQUIRE(indexer != nullptr);
    indexer->build();

    auto reader = ReaderFactory::create(indexer);
    REQUIRE(reader != nullptr);

    SUBCASE("Basic data reading") {
        const std::size_t buffer_size = 1024;
        std::vector<char> buffer(buffer_size);
        std::size_t total_bytes = 0;
        std::size_t current_pos = 0;
        std::uint64_t max_bytes = indexer->get_max_bytes();

        while (current_pos < max_bytes) {
            std::size_t end_pos = std::min(current_pos + buffer_size,
                                           static_cast<std::size_t>(max_bytes));
            std::size_t bytes_read = reader->read(current_pos, end_pos,
                                                  buffer.data(), buffer.size());
            if (bytes_read == 0) break;
            total_bytes += bytes_read;
            current_pos += bytes_read;
        }

        CHECK(total_bytes > 0);
        MESSAGE("Format: " << format_name(fixture.get_format())
                           << ", Total bytes read: " << total_bytes);
    }

    SUBCASE("Range reading") {
        std::size_t max_bytes = reader->get_max_bytes();

        // Test reading from middle of file
        std::size_t start_offset = max_bytes / 4;
        std::size_t end_offset = (max_bytes * 3) / 4;

        const std::size_t buffer_size = 1024;
        std::vector<char> buffer(buffer_size);
        std::size_t bytes_read = reader->read(start_offset, end_offset,
                                              buffer.data(), buffer.size());
        CHECK(bytes_read > 0);
        std::size_t total_bytes = bytes_read;

        // Should read approximately the range we set
        CHECK(total_bytes > 0);
        CHECK(total_bytes <=
              (end_offset - start_offset + 1024));  // Allow some tolerance
    }

    SUBCASE("Small buffer reading") {
        const std::size_t small_buffer_size = 16;
        std::vector<char> buffer(small_buffer_size);
        std::size_t total_bytes = 0;
        std::size_t read_count = 0;
        std::size_t current_pos_small = 0;
        std::uint64_t max_bytes = indexer->get_max_bytes();

        while (current_pos_small < max_bytes && read_count < 100) {
            std::size_t end_pos =
                std::min(current_pos_small + small_buffer_size,
                         static_cast<std::size_t>(max_bytes));
            std::size_t bytes_read = reader->read(current_pos_small, end_pos,
                                                  buffer.data(), buffer.size());
            if (bytes_read == 0) break;
            total_bytes += bytes_read;
            current_pos_small += bytes_read;
            read_count++;
        }

        CHECK(total_bytes > 0);
        CHECK(read_count > 0);
        MESSAGE("Format: " << format_name(fixture.get_format())
                           << ", Small buffer reads: " << read_count
                           << ", Total bytes: " << total_bytes);
    }
}

TEST_CASE_TEMPLATE("JSON boundary detection", FormatType, GZIPFormat,
                   TARGZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    auto indexer = IndexerFactory::create(fixture.get_test_file(),
                                          fixture.get_index_file(), 512 * 1024);
    REQUIRE(indexer != nullptr);
    indexer->build();

    auto reader = ReaderFactory::create(indexer);
    REQUIRE(reader != nullptr);

    SUBCASE("JSON boundary vs raw reading comparison") {
        // Create two readers - one for raw reading, one for JSON boundary
        // detection
        auto raw_reader = ReaderFactory::create(indexer);
        REQUIRE(raw_reader != nullptr);

        const std::size_t buffer_size = 1024;
        std::vector<char> raw_buffer(buffer_size);
        std::vector<char> json_buffer(buffer_size);

        std::uint64_t max_bytes = indexer->get_max_bytes();

        // Read raw data
        std::size_t raw_total = raw_reader->read(
            0, max_bytes, raw_buffer.data(), raw_buffer.size());

        // Read with JSON boundary detection
        std::size_t json_total = reader->read_line_bytes(
            0, max_bytes, json_buffer.data(), json_buffer.size());

        CHECK(raw_total > 0);
        CHECK(json_total > 0);

        MESSAGE("Format: " << format_name(fixture.get_format())
                           << ", Raw: " << raw_total
                           << " bytes, JSON: " << json_total << " bytes");

        // JSON boundary detection may read slightly less due to boundary
        // alignment
        CHECK(json_total <= raw_total);
    }
}

TEST_CASE_TEMPLATE("Line-based reading", FormatType, GZIPFormat,
                   TARGZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    // Create a larger test environment for line testing
    TestEnvironment large_env(1000, fixture.get_format());
    REQUIRE(large_env.is_valid());

    std::string large_test_file = large_env.create_test_file();
    REQUIRE(!large_test_file.empty());

    std::string large_index_file = large_env.get_index_path(large_test_file);

    auto indexer =
        IndexerFactory::create(large_test_file, large_index_file, 100 * 1024);
    REQUIRE(indexer != nullptr);
    indexer->build();

    uint64_t total_lines = indexer->get_num_lines();
    CHECK(total_lines > 0);

    auto reader = ReaderFactory::create(indexer);
    REQUIRE(reader != nullptr);

    SUBCASE("Basic line reading") {
        const std::size_t buffer_size = 1024;
        std::vector<char> buffer(buffer_size);
        std::size_t line_count = 0;

        for (std::size_t line = 1;
             line <= std::min(total_lines, static_cast<std::uint64_t>(10));
             ++line) {
            try {
                std::string line_content = reader->read_lines(line, line);
                if (!line_content.empty()) {
                    line_count++;
                    CHECK(line_content.size() > 0);
                }
            } catch (...) {
                // Skip lines that can't be read
            }
        }

        CHECK(line_count > 0);
        MESSAGE("Format: " << format_name(fixture.get_format())
                           << ", Successfully read " << line_count << " lines");
    }

    SUBCASE("Specific line reading accuracy") {
        // Test reading specific lines
        std::vector<std::size_t> test_lines = {0,  1,   10,
                                               50, 100, total_lines - 1};

        for (std::size_t line_num : test_lines) {
            if (line_num >= total_lines) continue;

            const std::size_t buffer_size = 1024;
            std::vector<char> buffer(buffer_size);

            try {
                // Convert to 1-based indexing
                std::string line_content =
                    reader->read_lines(line_num + 1, line_num + 1);
                CHECK(!line_content.empty());
                CHECK(line_content.find("\"id\":") != std::string::npos);
                CHECK(line_content.find("\"message\":") != std::string::npos);
            } catch (...) {
                // Skip lines that can't be read
            }
        }
    }
}

// Format-specific tests
TEST_CASE("TAR.GZ specific functionality") {
    SUBCASE("Multiple files in archive") {
        TestEnvironment tar_env(300, Format::TAR_GZIP);
        REQUIRE(tar_env.is_valid());

        std::string tar_gz_file = tar_env.create_test_tar_gzip_file();
        REQUIRE(!tar_gz_file.empty());

        std::string index_file = tar_env.get_index_path(tar_gz_file);

        auto indexer =
            IndexerFactory::create(tar_gz_file, index_file, 1024 * 1024);
        REQUIRE(indexer != nullptr);

        indexer->build();

        // Verify that we can read from the tar.gz archive
        auto reader = ReaderFactory::create(indexer);
        REQUIRE(reader != nullptr);

        const std::size_t buffer_size = 1024;
        std::vector<char> buffer(buffer_size);
        std::size_t total_bytes = 0;
        std::size_t current_pos = 0;
        std::uint64_t max_bytes = indexer->get_max_bytes();

        while (current_pos < max_bytes) {
            std::size_t end_pos = std::min(current_pos + buffer_size,
                                           static_cast<std::size_t>(max_bytes));
            std::size_t bytes_read = reader->read(current_pos, end_pos,
                                                  buffer.data(), buffer.size());
            if (bytes_read == 0) break;
            total_bytes += bytes_read;
            current_pos += bytes_read;
        }

        CHECK(total_bytes > 0);
        MESSAGE("TAR.GZ archive total bytes read: " << total_bytes);

        // Verify we read data from multiple files (should contain different
        // "file" field values)
        std::string content(buffer.data(), std::min(buffer_size, total_bytes));
        // Reset reader and read all content for verification
        reader = ReaderFactory::create(indexer);
        std::vector<char> full_content(total_bytes + 1024);
        std::size_t full_total = 0;
        std::size_t current_pos_full = 0;

        while (current_pos_full < max_bytes &&
               full_total < full_content.size() - 1024) {
            std::size_t end_pos = std::min(current_pos_full + 1024,
                                           static_cast<std::size_t>(max_bytes));
            std::size_t bytes_read =
                reader->read(current_pos_full, end_pos,
                             full_content.data() + full_total, 1024);
            if (bytes_read == 0) break;
            full_total += bytes_read;
            current_pos_full += bytes_read;
        }

        std::string full_string(full_content.data(), full_total);
        CHECK(full_string.find("\"file\": \"main\"") != std::string::npos);
        CHECK(full_string.find("\"file\": \"secondary\"") != std::string::npos);
        CHECK(full_string.find("\"file\": \"additional\"") !=
              std::string::npos);
    }

    SUBCASE("Directory structure handling") {
        TestEnvironment tar_env(150, Format::TAR_GZIP);
        REQUIRE(tar_env.is_valid());

        std::string tar_gz_file = tar_env.create_test_tar_gzip_file();
        REQUIRE(!tar_gz_file.empty());

        // The tar.gz should contain files with directory paths like
        // "logs/additional.jsonl"
        std::string index_file = tar_env.get_index_path(tar_gz_file);

        auto indexer =
            IndexerFactory::create(tar_gz_file, index_file, 1024 * 1024);
        REQUIRE(indexer != nullptr);

        indexer->build();
        CHECK(indexer->get_num_lines() >=
              150);  // Should have at least 150 lines across all files
    }
}

TEST_CASE("GZIP specific functionality") {
    SUBCASE("Single file structure") {
        TestEnvironment gzip_env(200, Format::GZIP);
        REQUIRE(gzip_env.is_valid());

        std::string gz_file = gzip_env.create_test_gzip_file();
        REQUIRE(!gz_file.empty());

        std::string index_file = gzip_env.get_index_path(gz_file);

        auto indexer = IndexerFactory::create(gz_file, index_file, 1024 * 1024);
        REQUIRE(indexer != nullptr);

        indexer->build();
        CHECK(indexer->get_num_lines() ==
              200);  // Should have exactly 200 lines

        auto reader = ReaderFactory::create(indexer);
        REQUIRE(reader != nullptr);

        // Read some content and verify it's the simple gzip format
        std::vector<char> buffer(1024);
        std::uint64_t max_bytes = indexer->get_max_bytes();
        std::size_t bytes_read = reader->read(
            0, std::min(static_cast<std::size_t>(max_bytes), buffer.size()),
            buffer.data(), buffer.size());
        CHECK(bytes_read > 0);

        std::string content(buffer.data(), bytes_read);
        CHECK(content.find("\"message\": \"Test message") != std::string::npos);
        CHECK(content.find("\"file\":") ==
              std::string::npos);  // GZIP format shouldn't have file field
    }
}
