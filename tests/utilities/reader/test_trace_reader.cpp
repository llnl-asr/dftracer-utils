#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::reader;
using namespace dftracer::utils::utilities::indexer::internal;
using namespace dftracer::utils::coro;
using namespace dft_utils_test;

namespace {

// Consume AsyncGenerator<Line> into a count, materialising string copies so
// the string_view lifetime is not a concern.
static CoroTask<std::size_t> count_lines(
    AsyncGenerator<dftracer::utils::utilities::fileio::lines::Line> gen) {
    std::size_t n = 0;
    while (auto line = co_await gen.next()) {
        ++n;
    }
    co_return n;
}

static CoroTask<std::size_t> count_raw_bytes(
    AsyncGenerator<std::span<const char>> gen) {
    std::size_t total = 0;
    while (auto chunk = co_await gen.next()) {
        total += chunk->size();
    }
    co_return total;
}

static CoroTask<std::size_t> count_raw_chunks(
    AsyncGenerator<std::span<const char>> gen) {
    std::size_t n = 0;
    while (auto chunk = co_await gen.next()) {
        ++n;
    }
    co_return n;
}

// Collect line numbers from the generator.
static CoroTask<std::vector<std::size_t>> collect_line_numbers(
    AsyncGenerator<dftracer::utils::utilities::fileio::lines::Line> gen) {
    std::vector<std::size_t> nums;
    while (auto line = co_await gen.next()) {
        nums.push_back(line->line_number);
    }
    co_return nums;
}

}  // namespace

TEST_SUITE("TraceReader") {
    TEST_CASE("Read all lines without index") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = gz_file});

        CHECK_FALSE(reader.has_index());

        auto n = count_lines(reader.read_lines()).get();
        CHECK(n > 0);
        CHECK(n == 100);
    }

    TEST_CASE("Read all lines with pre-built index") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);
        std::string index_dir = env.get_dir();
        std::string idx_path = env.get_index_path(gz_file);

        auto indexer =
            IndexerFactory::create(gz_file, idx_path, 32 * 1024 * 1024, false);
        REQUIRE(indexer != nullptr);
        indexer->build();
        REQUIRE(fs::exists(idx_path));

        TraceReader reader({.file_path = gz_file, .index_dir = index_dir});

        CHECK(reader.has_index());

        auto n_indexed = count_lines(reader.read_lines()).get();
        CHECK(n_indexed > 0);
        CHECK(n_indexed == 100);

        SUBCASE("Indexed and unindexed counts match") {
            // Remove the index and re-read to compare.
            fs::remove(idx_path);
            TraceReader plain_reader({.file_path = gz_file});
            CHECK_FALSE(plain_reader.has_index());
            auto n_plain = count_lines(plain_reader.read_lines()).get();
            CHECK(n_plain == n_indexed);
        }
    }

    TEST_CASE("Read with start_line skip") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = gz_file});

        auto n_full = count_lines(reader.read_lines()).get();
        REQUIRE(n_full > 0);

        ReadConfig rc;
        rc.start_line = 50;
        auto n_skipped = count_lines(reader.read_lines(rc)).get();
        CHECK(n_skipped < n_full);
        CHECK(n_skipped > 0);
    }

    TEST_CASE("Read with end_line limit") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = gz_file});

        ReadConfig rc;
        rc.start_line = 1;
        rc.end_line = 10;
        auto nums = collect_line_numbers(reader.read_lines(rc)).get();
        CHECK(nums.size() <= 10);
        CHECK(nums.size() > 0);
    }

    TEST_CASE("read_raw returns non-empty spans") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = gz_file});
        auto total = count_raw_bytes(reader.read_raw()).get();
        CHECK(total > 0);
    }

    TEST_CASE("read_raw line_aligned=false returns raw bytes") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = gz_file});
        ReadConfig rc;
        rc.line_aligned = false;
        auto total = count_raw_bytes(reader.read_raw(rc)).get();
        CHECK(total > 0);
    }

    TEST_CASE("read_raw multi_line=false yields single lines") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = gz_file});
        ReadConfig rc;
        rc.line_aligned = true;
        rc.multi_line = false;
        auto chunks = count_raw_chunks(reader.read_raw(rc)).get();
        CHECK(chunks > 0);
    }

    TEST_CASE("Empty file path produces zero lines or throws") {
        std::size_t n = 0;
        bool threw = false;
        try {
            TraceReader reader({.file_path = ""});
            n = count_lines(reader.read_lines()).get();
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK((n == 0 || threw));
    }
}
