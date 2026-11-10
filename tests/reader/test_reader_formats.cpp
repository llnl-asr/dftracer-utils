// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/utilities/reader/error.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <random>

using namespace dftracer::utils;

using namespace dftu_utils_test;
using namespace dftracer::utils::index::gzip;
using namespace dftracer::utils::utilities::reader::internal;

dstd::String format_name(Format format);

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

    Format format() const { return format_; }
    const dstd::String& test_file() const { return test_file_; }
    const dstd::String& index_file() const { return index_file_; }
    const TestEnvironment& env() const { return env_; }

   private:
    Format format_;
    TestEnvironment env_;
    dstd::String test_file_;
    dstd::String index_file_;
};

// Helper function to get format name for test output
dstd::String format_name(Format format) {
    switch (format) {
        case Format::Gzip:
            return "gzip";
        default:
            return "unknown";
    }
}

// Template wrapper types for doctest parameterization
template <Format F>
struct FormatWrapper {
    static constexpr Format value = F;
};

using GZIPFormat = FormatWrapper<Format::Gzip>;

// Parameterized tests using doctest's approach
TEST_CASE_TEMPLATE("Indexer creation and destruction", FormatType, GZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    SUBCASE("Basic indexer creation") {
        auto indexer = CheckpointIndexerFactory::create(
            fixture.test_file(), fixture.index_file(), 1024 * 1024);
        REQUIRE(indexer != nullptr);
        CHECK_FALSE(indexer->exists());
        CHECK(indexer->need_rebuild());
    }

    SUBCASE("Invalid file path") {
        try {
            (void)CheckpointIndexerFactory::create(
                "/nonexistent/file.gz", fixture.index_file(), 1024 * 1024);
            FAIL("create did not throw");
        } catch (const common::DFTUtilsException& e) {
            CHECK(e.error().is_code(common::ErrorCode::Indexer));
        }
    }
}

TEST_CASE_TEMPLATE("Index building", FormatType, GZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    SUBCASE("Basic index building") {
        auto indexer = CheckpointIndexerFactory::create(
            fixture.test_file(), fixture.index_file(), 1024 * 1024);
        REQUIRE(indexer != nullptr);

        indexer->build();
        CHECK(indexer->line_count() > 0);
    }

    SUBCASE("Rebuild detection") {
        auto indexer = CheckpointIndexerFactory::create(
            fixture.test_file(), fixture.index_file(), 1024 * 1024);
        REQUIRE(indexer != nullptr);

        // First build
        indexer->build();

        // Check if rebuild is needed (should not be needed after fresh build)
        CHECK_FALSE(indexer->need_rebuild());
    }

    SUBCASE("Force rebuild") {
        auto indexer = CheckpointIndexerFactory::create(
            fixture.test_file(), fixture.index_file(), 1024 * 1024, true);
        REQUIRE(indexer != nullptr);

        indexer->build();
    }
}

TEST_CASE_TEMPLATE("Reader creation and basic functionality", FormatType,
                   GZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    auto indexer = CheckpointIndexerFactory::create(
        fixture.test_file(), fixture.index_file(), 1024 * 1024);
    REQUIRE(indexer != nullptr);
    indexer->build();

    SUBCASE("Reader creation with indexer") {
        auto reader = dftu_utils_test::ok(ReaderFactory::create(indexer));
        CHECK(reader != nullptr);
        CHECK(reader->is_valid());
    }

    SUBCASE("Reader creation from files") {
        auto reader = dftu_utils_test::ok(ReaderFactory::create(
            fixture.test_file(), fixture.index_file(), 1024 * 1024));
        CHECK(reader != nullptr);
        CHECK(reader->is_valid());
    }
}

TEST_CASE_TEMPLATE("Data reading operations", FormatType, GZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    auto indexer = CheckpointIndexerFactory::create(
        fixture.test_file(), fixture.index_file(), 512 * 1024);
    REQUIRE(indexer != nullptr);
    indexer->build();

    auto reader = dftu_utils_test::ok(ReaderFactory::create(indexer));
    REQUIRE(reader != nullptr);

    SUBCASE("Basic data reading") {
        const std::size_t buffer_size = 1024;
        dstd::Vector<char> buffer(buffer_size);
        std::size_t total_bytes = 0;
        std::size_t current_pos = 0;
        std::uint64_t max_bytes = indexer->max_bytes();

        while (current_pos < max_bytes) {
            std::size_t end_pos = std::min(current_pos + buffer_size,
                                           static_cast<std::size_t>(max_bytes));
            std::size_t bytes_read = dftu_utils_test::ok(reader->read(
                current_pos, end_pos, buffer.data(), buffer.size()));
            if (bytes_read == 0) break;
            total_bytes += bytes_read;
            current_pos += bytes_read;
        }

        CHECK(total_bytes > 0);
        MESSAGE("Format: " << format_name(fixture.format())
                           << ", Total bytes read: " << total_bytes);
    }

    SUBCASE("Range reading") {
        std::size_t max_bytes = reader->max_bytes();

        // Test reading from middle of file
        std::size_t start_offset = max_bytes / 4;
        std::size_t end_offset = (max_bytes * 3) / 4;

        const std::size_t buffer_size = 1024;
        dstd::Vector<char> buffer(buffer_size);
        std::size_t bytes_read = dftu_utils_test::ok(reader->read(
            start_offset, end_offset, buffer.data(), buffer.size()));
        CHECK(bytes_read > 0);
        std::size_t total_bytes = bytes_read;

        // Should read approximately the range we set
        CHECK(total_bytes > 0);
        CHECK(total_bytes <=
              (end_offset - start_offset + 1024));  // Allow some tolerance
    }

    SUBCASE("Small buffer reading") {
        const std::size_t small_buffer_size = 16;
        dstd::Vector<char> buffer(small_buffer_size);
        std::size_t total_bytes = 0;
        std::size_t read_count = 0;
        std::size_t current_pos_small = 0;
        std::uint64_t max_bytes = indexer->max_bytes();

        while (current_pos_small < max_bytes && read_count < 100) {
            std::size_t end_pos =
                std::min(current_pos_small + small_buffer_size,
                         static_cast<std::size_t>(max_bytes));
            std::size_t bytes_read = dftu_utils_test::ok(reader->read(
                current_pos_small, end_pos, buffer.data(), buffer.size()));
            if (bytes_read == 0) break;
            total_bytes += bytes_read;
            current_pos_small += bytes_read;
            read_count++;
        }

        CHECK(total_bytes > 0);
        CHECK(read_count > 0);
        MESSAGE("Format: " << format_name(fixture.format())
                           << ", Small buffer reads: " << read_count
                           << ", Total bytes: " << total_bytes);
    }
}

TEST_CASE_TEMPLATE("JSON boundary detection", FormatType, GZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    auto indexer = CheckpointIndexerFactory::create(
        fixture.test_file(), fixture.index_file(), 512 * 1024);
    REQUIRE(indexer != nullptr);
    indexer->build();

    auto reader = dftu_utils_test::ok(ReaderFactory::create(indexer));
    REQUIRE(reader != nullptr);

    SUBCASE("JSON boundary vs raw reading comparison") {
        // Create two readers - one for raw reading, one for JSON boundary
        // detection
        auto raw_reader = dftu_utils_test::ok(ReaderFactory::create(indexer));
        REQUIRE(raw_reader != nullptr);

        const std::size_t buffer_size = 1024;
        dstd::Vector<char> raw_buffer(buffer_size);
        dstd::Vector<char> json_buffer(buffer_size);

        std::uint64_t max_bytes = indexer->max_bytes();

        // Read raw data
        std::size_t raw_total = dftu_utils_test::ok(raw_reader->read(
            0, max_bytes, raw_buffer.data(), raw_buffer.size()));

        // Read with JSON boundary detection
        std::size_t json_total = dftu_utils_test::ok(reader->read_line_bytes(
            0, max_bytes, json_buffer.data(), json_buffer.size()));

        CHECK(raw_total > 0);
        CHECK(json_total > 0);

        MESSAGE("Format: " << format_name(fixture.format())
                           << ", Raw: " << raw_total
                           << " bytes, JSON: " << json_total << " bytes");

        // JSON boundary detection may read slightly less due to boundary
        // alignment
        CHECK(json_total <= raw_total);
    }
}

TEST_CASE_TEMPLATE("Line-based reading", FormatType, GZIPFormat) {
    FormatTestFixture fixture(FormatType::value);

    // Create a larger test environment for line testing
    TestEnvironment large_env(1000, fixture.format());
    REQUIRE(large_env.is_valid());

    dstd::String large_test_file = large_env.create_test_file();
    REQUIRE(!large_test_file.empty());

    dstd::String large_index_file = large_env.get_index_path(large_test_file);

    auto indexer = CheckpointIndexerFactory::create(
        large_test_file, large_index_file, 100 * 1024);
    REQUIRE(indexer != nullptr);
    indexer->build();

    uint64_t total_lines = indexer->line_count();
    CHECK(total_lines > 0);

    auto reader = dftu_utils_test::ok(ReaderFactory::create(indexer));
    REQUIRE(reader != nullptr);

    SUBCASE("Basic line reading") {
        const std::size_t buffer_size = 1024;
        dstd::Vector<char> buffer(buffer_size);
        std::size_t line_count = 0;

        for (std::size_t line = 1;
             line <= std::min(total_lines, static_cast<std::uint64_t>(10));
             ++line) {
            try {
                dstd::String line_content =
                    dftu_utils_test::ok(reader->read_lines(line, line));
                if (!line_content.empty()) {
                    line_count++;
                    CHECK(line_content.size() > 0);
                }
            } catch (...) {
                // Skip lines that can't be read
            }
        }

        CHECK(line_count > 0);
        MESSAGE("Format: " << format_name(fixture.format())
                           << ", Successfully read " << line_count << " lines");
    }

    SUBCASE("Specific line reading accuracy") {
        // Test reading specific lines
        dstd::Vector<std::size_t> test_lines = {0,  1,   10,
                                                50, 100, total_lines - 1};

        for (std::size_t line_num : test_lines) {
            if (line_num >= total_lines) continue;

            const std::size_t buffer_size = 1024;
            dstd::Vector<char> buffer(buffer_size);

            try {
                // Convert to 1-based indexing
                dstd::String line_content = dftu_utils_test::ok(
                    reader->read_lines(line_num + 1, line_num + 1));
                CHECK(!line_content.empty());
                CHECK(line_content.find("\"id\":") != dstd::String::npos);
                CHECK(line_content.find("\"message\":") != dstd::String::npos);
            } catch (...) {
                // Skip lines that can't be read
            }
        }
    }
}
