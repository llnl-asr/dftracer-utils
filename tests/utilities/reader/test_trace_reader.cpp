#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstddef>
#include <fstream>
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

static CoroTask<std::vector<std::string>> collect_lines(
    AsyncGenerator<dftracer::utils::utilities::fileio::lines::Line> gen) {
    std::vector<std::string> lines;
    while (auto line = co_await gen.next()) {
        lines.emplace_back(line->content);
    }
    co_return lines;
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
        CHECK(n == 102);
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
        CHECK(n_indexed == 102);

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

    TEST_CASE("read_raw with byte range returns subset") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = gz_file});

        auto total = count_raw_bytes(reader.read_raw()).get();
        REQUIRE(total > 0);

        ReadConfig rc;
        rc.start_byte = 100;
        rc.end_byte = total / 2;
        auto subset = count_raw_bytes(reader.read_raw(rc)).get();
        CHECK(subset > 0);
        CHECK(subset < total);
    }

    TEST_CASE("read_raw byte range works with index") {
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

        auto total = count_raw_bytes(reader.read_raw()).get();
        REQUIRE(total > 0);

        ReadConfig rc;
        rc.start_byte = 100;
        rc.end_byte = total / 2;
        auto subset = count_raw_bytes(reader.read_raw(rc)).get();
        CHECK(subset > 0);
        CHECK(subset < total);
    }

    TEST_CASE("read_raw indexed and unindexed produce same chunk count") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);
        std::string index_dir = env.get_dir();
        std::string idx_path = env.get_index_path(gz_file);

        TraceReader plain_reader({.file_path = gz_file});
        CHECK_FALSE(plain_reader.has_index());
        ReadConfig single_line;
        single_line.line_aligned = true;
        single_line.multi_line = false;
        auto plain_chunks =
            count_raw_chunks(plain_reader.read_raw(single_line)).get();

        auto indexer =
            IndexerFactory::create(gz_file, idx_path, 32 * 1024 * 1024, false);
        REQUIRE(indexer != nullptr);
        indexer->build();

        TraceReader indexed_reader(
            {.file_path = gz_file, .index_dir = index_dir});
        CHECK(indexed_reader.has_index());
        auto indexed_chunks =
            count_raw_chunks(indexed_reader.read_raw(single_line)).get();

        CHECK(plain_chunks > 0);
        CHECK(plain_chunks == indexed_chunks);
    }

    TEST_CASE("read_raw small buffer_size produces more chunks") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = gz_file});

        ReadConfig large_buf;
        large_buf.buffer_size = 4 * 1024 * 1024;
        auto large_chunks = count_raw_chunks(reader.read_raw(large_buf)).get();

        ReadConfig small_buf;
        small_buf.buffer_size = 256;
        auto small_chunks = count_raw_chunks(reader.read_raw(small_buf)).get();

        CHECK(large_chunks > 0);
        CHECK(small_chunks > 0);
        CHECK(small_chunks >= large_chunks);
    }

    TEST_CASE("Query filters matching events") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = gz_file});

        auto all = count_lines(reader.read_lines()).get();
        REQUIRE(all > 0);

        ReadConfig rc;
        rc.query = R"(cat == "POSIX")";
        auto matched = count_lines(reader.read_lines(rc)).get();
        CHECK(matched > 0);
        CHECK(matched <= all);
    }

    TEST_CASE("Query with no matches returns zero lines") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = gz_file});

        ReadConfig rc;
        rc.query = R"(cat == "NONEXISTENT")";
        auto matched = count_lines(reader.read_lines(rc)).get();
        CHECK(matched == 0);
    }

    TEST_CASE("Query filters by name") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = gz_file});

        ReadConfig rc;
        rc.query = R"(name == "read")";
        auto lines = collect_lines(reader.read_lines(rc)).get();
        CHECK(lines.size() > 0);
        for (const auto& line : lines) {
            CHECK(line.find("\"name\":\"read\"") != std::string::npos);
        }
    }

    TEST_CASE("Query with AND narrows results") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = gz_file});

        ReadConfig rc_cat;
        rc_cat.query = R"(cat == "POSIX")";
        auto cat_count = count_lines(reader.read_lines(rc_cat)).get();

        ReadConfig rc_both;
        rc_both.query = R"(cat == "POSIX" and name == "read")";
        auto both_count = count_lines(reader.read_lines(rc_both)).get();

        CHECK(both_count > 0);
        CHECK(both_count <= cat_count);
    }

    TEST_CASE("Query with OR widens results") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = gz_file});

        ReadConfig rc_read;
        rc_read.query = R"(name == "read")";
        auto read_count = count_lines(reader.read_lines(rc_read)).get();

        ReadConfig rc_write;
        rc_write.query = R"(name == "write")";
        auto write_count = count_lines(reader.read_lines(rc_write)).get();

        ReadConfig rc_or;
        rc_or.query = R"(name == "read" or name == "write")";
        auto or_count = count_lines(reader.read_lines(rc_or)).get();

        CHECK(or_count == read_count + write_count);
    }

    TEST_CASE("Query works with index") {
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

        ReadConfig rc;
        rc.query = R"(name == "read")";
        auto lines = collect_lines(reader.read_lines(rc)).get();
        CHECK(lines.size() > 0);
        for (const auto& line : lines) {
            CHECK(line.find("\"name\":\"read\"") != std::string::npos);
        }
    }

    TEST_CASE("Query combines with line range") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = gz_file});

        ReadConfig rc;
        rc.start_line = 1;
        rc.end_line = 20;
        rc.query = R"(cat == "POSIX")";
        auto matched = count_lines(reader.read_lines(rc)).get();
        CHECK(matched <= 20);
    }

    TEST_CASE("Empty query string reads all") {
        TestEnvironment env(100);
        std::string gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = gz_file});

        auto all = count_lines(reader.read_lines()).get();

        ReadConfig rc;
        rc.query = "";
        auto with_empty = count_lines(reader.read_lines(rc)).get();

        CHECK(all == with_empty);
    }

    TEST_CASE("Query with index filters events per-line") {
        TestEnvironment env(100);
        std::string pfw = env.get_dir() + "/multi_cat.pfw";
        {
            std::ofstream out(pfw);
            for (int i = 0; i < 200; ++i) {
                out << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                    << (1000 + i) << R"(,"dur":10,"args":{}})" << "\n";
            }
            for (int i = 0; i < 200; ++i) {
                out << R"({"ph":"X","name":"train","cat":"COMPUTE","pid":1,"tid":1,"ts":)"
                    << (100000 + i) << R"(,"dur":500,"args":{}})" << "\n";
            }
        }
        std::string gz = pfw + ".gz";
        REQUIRE(dft_utils_test::compress_file_to_gzip(pfw, gz));
        fs::remove(pfw);

        using dftracer::utils::utilities::indexer::IndexBuildConfig;
        using dftracer::utils::utilities::indexer::IndexBuilderUtility;
        IndexBuilderUtility builder;
        auto build_result = builder
                                .process(IndexBuildConfig::for_file(gz)
                                             .with_bloom(true)
                                             .with_index_threshold(0))
                                .get();
        REQUIRE(build_result.success);

        TraceReader reader({.file_path = gz});
        REQUIRE(reader.has_index());

        auto all = count_lines(reader.read_lines()).get();
        REQUIRE(all == 400);

        ReadConfig rc_posix;
        rc_posix.query = R"(cat == "POSIX")";
        auto posix_lines = collect_lines(reader.read_lines(rc_posix)).get();
        CHECK(posix_lines.size() == 200);
        for (const auto& line : posix_lines) {
            CHECK(line.find("\"cat\":\"POSIX\"") != std::string::npos);
        }

        ReadConfig rc_compute;
        rc_compute.query = R"(cat == "COMPUTE")";
        auto compute_lines = collect_lines(reader.read_lines(rc_compute)).get();
        CHECK(compute_lines.size() == 200);

        ReadConfig rc_none;
        rc_none.query = R"(cat == "NONEXISTENT")";
        auto none_lines = count_lines(reader.read_lines(rc_none)).get();
        CHECK(none_lines == 0);
    }

    TEST_CASE("Query file-level skip for raw bytes") {
        TestEnvironment env(100);
        std::string pfw = env.get_dir() + "/raw_skip.pfw";
        {
            std::ofstream out(pfw);
            for (int i = 0; i < 100; ++i) {
                out << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                    << (1000 + i) << R"(,"dur":10,"args":{}})" << "\n";
            }
        }
        std::string gz = pfw + ".gz";
        REQUIRE(dft_utils_test::compress_file_to_gzip(pfw, gz));
        fs::remove(pfw);

        using dftracer::utils::utilities::indexer::IndexBuildConfig;
        using dftracer::utils::utilities::indexer::IndexBuilderUtility;
        IndexBuilderUtility builder;
        auto build_result = builder
                                .process(IndexBuildConfig::for_file(gz)
                                             .with_bloom(true)
                                             .with_index_threshold(0))
                                .get();
        REQUIRE(build_result.success);

        TraceReader reader({.file_path = gz});
        REQUIRE(reader.has_index());

        auto all_bytes = count_raw_bytes(reader.read_raw()).get();
        REQUIRE(all_bytes > 0);

        // No match → file-level skip → zero bytes
        ReadConfig rc_none;
        rc_none.query = R"(cat == "NONEXISTENT")";
        auto none_bytes = count_raw_bytes(reader.read_raw(rc_none)).get();
        CHECK(none_bytes == 0);

        // Match → all bytes (no per-event filtering for raw)
        ReadConfig rc_posix;
        rc_posix.query = R"(cat == "POSIX")";
        auto posix_bytes = count_raw_bytes(reader.read_raw(rc_posix)).get();
        CHECK(posix_bytes == all_bytes);
    }
}
