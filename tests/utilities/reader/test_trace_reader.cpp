// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/arrow/array_view.h>
#include <dftracer/utils/utilities/arrow/arrow_export.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <random>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::reader;
using namespace dftracer::utils::index::gzip;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::trace::internal;
using namespace dftu_utils_test;

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
    AsyncGenerator<dstd::Span<const char>> gen) {
    std::size_t total = 0;
    while (auto chunk = co_await gen.next()) {
        total += chunk->size();
    }
    co_return total;
}

static CoroTask<std::size_t> count_raw_chunks(
    AsyncGenerator<dstd::Span<const char>> gen) {
    std::size_t n = 0;
    while (auto chunk = co_await gen.next()) {
        ++n;
    }
    co_return n;
}

// Collect line numbers from the generator.
static CoroTask<dstd::Vector<std::size_t>> collect_line_numbers(
    AsyncGenerator<dftracer::utils::utilities::fileio::lines::Line> gen) {
    dstd::Vector<std::size_t> nums;
    while (auto line = co_await gen.next()) {
        nums.push_back(line->line_number);
    }
    co_return nums;
}

static CoroTask<dstd::Vector<dstd::String>> collect_lines(
    AsyncGenerator<dftracer::utils::utilities::fileio::lines::Line> gen) {
    dstd::Vector<dstd::String> lines;
    while (auto line = co_await gen.next()) {
        lines.emplace_back(line->content);
    }
    co_return lines;
}

struct ParsedEvent {
    dstd::String name;
    dstd::String cat;
    dstd::String ph;
};

static CoroTask<dstd::Vector<ParsedEvent>> collect_json_events(
    AsyncGenerator<JsonLine> gen) {
    dstd::Vector<ParsedEvent> events;
    while (auto opt = co_await gen.next()) {
        auto* p = opt->parser;
        ParsedEvent ev;
        if (auto v = p->get_string("name")) ev.name = dstd::String(*v);
        if (auto v = p->get_string("cat")) ev.cat = dstd::String(*v);
        if (auto v = p->get_string("ph")) ev.ph = dstd::String(*v);
        events.push_back(std::move(ev));
    }
    co_return events;
}

static CoroTask<std::size_t> count_json_lines(AsyncGenerator<JsonLine> gen) {
    std::size_t n = 0;
    while (auto opt = co_await gen.next()) {
        ++n;
    }
    co_return n;
}

static CoroTask<std::size_t> count_arrow_rows(
    AsyncGenerator<dftracer::utils::utilities::arrow::ArrowExportResult> gen) {
    std::size_t n = 0;
    while (auto opt = co_await gen.next()) {
        n += static_cast<std::size_t>(opt->get_array()->length);
    }
    co_return n;
}

}  // namespace

namespace {
/// Traces must be gzip, so a fixture written as plain text is compressed
/// before the reader sees it. Returns the .gz path.
///
/// The result goes in its own directory: a gzip trace sitting directly in
/// the temp root would pick up any .dftindex another test left there and be
/// read through an index that does not list it.
dstd::String gzip_fixture(const dstd::String& plain_path) {
    const dstd::Path dir = make_unique_test_path("trace_fixture");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String gz_path(
        (dir / (dstd::String(dstd::PathView(plain_path).filename()) + ".gz"))
            .str());
    REQUIRE(dftu_utils_test::compress_file_to_gzip(plain_path, gz_path));
    (void)dstd::fs::remove(plain_path);
    return gz_path;
}
}  // namespace

TEST_SUITE("TraceReader") {
    TEST_CASE("Read all lines without index") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = dstd::String(gz_file)});

        CHECK_FALSE(reader.has_index());

        auto n = count_lines(reader.read_lines()).get();
        CHECK(n > 0);
        CHECK(n == 102);
    }

    TEST_CASE("Read all lines with pre-built index") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        dstd::String index_dir = env.get_dir();
        dstd::String index_path = env.get_index_path(gz_file);

        auto indexer = CheckpointIndexerFactory::create(
            gz_file, index_path, 32 * 1024 * 1024, false);
        REQUIRE(indexer != nullptr);
        indexer->build();
        REQUIRE(
            dstd::fs::exists(determine_index_path(gz_file, index_dir)).value());

        TraceReader reader({.file_path = dstd::String(gz_file),
                            .index_dir = dstd::String(index_dir)});

        CHECK(reader.has_index());

        auto n_indexed = count_lines(reader.read_lines()).get();
        CHECK(n_indexed > 0);
        CHECK(n_indexed == 102);

        SUBCASE("Indexed and unindexed counts match") {
            // Remove the index and re-read to compare.
            dstd::fs::remove_all(determine_index_path(gz_file, index_dir))
                .has_value();
            TraceReader plain_reader({.file_path = dstd::String(gz_file)});
            CHECK_FALSE(plain_reader.has_index());
            auto n_plain = count_lines(plain_reader.read_lines()).get();
            CHECK(n_plain == n_indexed);
        }
    }

    TEST_CASE("Query nested args field by bare and dotted name") {
        // Test data events carry args:{"ret":1024*i, ...}. Bare `ret` and
        // dotted `args.ret` must select the same events in the ValueMap path.
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        dstd::String index_dir = env.get_dir();
        dstd::String index_path = env.get_index_path(gz_file);
        auto indexer = CheckpointIndexerFactory::create(
            gz_file, index_path, 32 * 1024 * 1024, false);
        REQUIRE(indexer != nullptr);
        indexer->build();

        TraceReader reader({.file_path = dstd::String(gz_file),
                            .index_dir = dstd::String(index_dir)});
        REQUIRE(reader.has_index());

        ReadConfig bare;
        bare.query = "ret >= 100000";
        auto n_bare = count_json_lines(reader.read_json(bare)).get();

        ReadConfig dotted;
        dotted.query = "args.ret >= 100000";
        auto n_dotted = count_json_lines(reader.read_json(dotted)).get();

        CHECK(n_bare > 0);
        CHECK(n_dotted == n_bare);
    }

    TEST_CASE("Read with start_line skip") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = dstd::String(gz_file)});

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
        dstd::String gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.start_line = 1;
        rc.end_line = 10;
        auto nums = collect_line_numbers(reader.read_lines(rc)).get();
        CHECK(nums.size() <= 10);
        CHECK(nums.size() > 0);
    }

    TEST_CASE("read_raw returns non-empty spans") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = dstd::String(gz_file)});
        auto total = count_raw_bytes(reader.read_raw()).get();
        CHECK(total > 0);
    }

    TEST_CASE("read_raw line_aligned=false returns raw bytes") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = dstd::String(gz_file)});
        ReadConfig rc;
        rc.line_aligned = false;
        auto total = count_raw_bytes(reader.read_raw(rc)).get();
        CHECK(total > 0);
    }

    TEST_CASE("read_raw multi_line=false yields single lines") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = dstd::String(gz_file)});
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
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

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
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        dstd::String index_dir = env.get_dir();
        dstd::String index_path = env.get_index_path(gz_file);

        auto indexer = CheckpointIndexerFactory::create(
            gz_file, index_path, 32 * 1024 * 1024, false);
        REQUIRE(indexer != nullptr);
        indexer->build();
        REQUIRE(
            dstd::fs::exists(determine_index_path(gz_file, index_dir)).value());

        TraceReader reader({.file_path = dstd::String(gz_file),
                            .index_dir = dstd::String(index_dir)});
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

    TEST_CASE("read_lines plain file byte range completes current line") {
        auto test_file = make_unique_test_path("trace_reader_plain_range.pfw");
        const dstd::String first_line =
            R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"ph":"X","args":{"ret":1}})";
        const dstd::String second_line =
            R"({"name":"write","cat":"POSIX","pid":1,"tid":1,"ts":2000,"dur":20,"ph":"X","args":{"ret":2}})";
        const dstd::String third_line =
            R"({"name":"close","cat":"POSIX","pid":1,"tid":1,"ts":3000,"dur":30,"ph":"X","args":{"ret":3}})";
        {
            dstd::String out;
            out += common::str_cat(first_line, "\n");
            out += common::str_cat(second_line, "\n");
            out += common::str_cat(third_line, "\n");
            (void)dstd::fs::write_file(dstd::PathView(test_file), out);
        }
        auto gz_file = gzip_fixture(dstd::String(test_file.str()));

        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.start_byte = 0;
        rc.end_byte =
            first_line.size() + 1 + dstd::String(R"({"name":"write")").size();

        auto lines = collect_lines(reader.read_lines(rc)).get();

        REQUIRE(lines.size() == 2);
        CHECK(lines[0].find(R"("name":"read")") != dstd::String::npos);
        CHECK(lines[1].find(R"("name":"write")") != dstd::String::npos);

        (void)dstd::fs::remove(gz_file);
    }

    TEST_CASE("read_lines plain file byte range skips partial first line") {
        auto test_file =
            make_unique_test_path("trace_reader_plain_skip_partial.pfw");
        {
            dstd::String out;
            out += "alpha\n";
            out += "beta\n";
            out += "gamma\n";
            (void)dstd::fs::write_file(dstd::PathView(test_file), out);
        }
        auto gz_file = gzip_fixture(dstd::String(test_file.str()));

        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.start_byte = 2;
        rc.end_byte = 100;
        auto lines = collect_lines(reader.read_lines(rc)).get();

        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "beta");
        CHECK(lines[1] == "gamma");

        (void)dstd::fs::remove(gz_file);
    }

    TEST_CASE("read_raw indexed and unindexed produce same chunk count") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        dstd::String index_dir = env.get_dir();
        dstd::String index_path = env.get_index_path(gz_file);

        TraceReader plain_reader({.file_path = dstd::String(gz_file)});
        CHECK_FALSE(plain_reader.has_index());
        ReadConfig single_line;
        single_line.line_aligned = true;
        single_line.multi_line = false;
        auto plain_chunks =
            count_raw_chunks(plain_reader.read_raw(single_line)).get();

        auto indexer = CheckpointIndexerFactory::create(
            gz_file, index_path, 32 * 1024 * 1024, false);
        REQUIRE(indexer != nullptr);
        indexer->build();

        TraceReader indexed_reader({.file_path = dstd::String(gz_file),
                                    .index_dir = dstd::String(index_dir)});
        CHECK(indexed_reader.has_index());
        auto indexed_chunks =
            count_raw_chunks(indexed_reader.read_raw(single_line)).get();

        CHECK(plain_chunks > 0);
        CHECK(plain_chunks == indexed_chunks);
    }

    TEST_CASE("read_raw small buffer_size produces more chunks") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

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
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

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
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.query = R"(cat == "NONEXISTENT")";
        auto matched = count_lines(reader.read_lines(rc)).get();
        CHECK(matched == 0);
    }

    TEST_CASE("Query filters by name") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.query = R"(name == "read")";
        auto lines = collect_lines(reader.read_lines(rc)).get();
        CHECK(lines.size() > 0);
        for (const auto& line : lines) {
            CHECK(line.find("\"name\":\"read\"") != dstd::String::npos);
        }
    }

    TEST_CASE("Query filters plain file byte ranges") {
        auto test_file =
            make_unique_test_path("trace_reader_plain_query_range.pfw");
        {
            dstd::String out;
            out += common::str_cat(
                R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"ph":"X","args":{"ret":1}})",
                "\n");
            out += common::str_cat(
                R"({"name":"write","cat":"POSIX","pid":1,"tid":1,"ts":2000,"dur":20,"ph":"X","args":{"ret":2}})",
                "\n");
            (void)dstd::fs::write_file(dstd::PathView(test_file), out);
        }

        const auto uncompressed_size = dstd::fs::file_size(test_file).value();
        auto gz_file = gzip_fixture(dstd::String(test_file.str()));

        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.start_byte = 0;
        rc.end_byte = uncompressed_size;
        rc.query = R"(name == "write")";
        auto lines = collect_lines(reader.read_lines(rc)).get();

        REQUIRE(lines.size() == 1);
        CHECK(lines[0].find(R"("name":"write")") != dstd::String::npos);

        (void)dstd::fs::remove(gz_file);
    }

    TEST_CASE("Query with AND narrows results") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

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
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

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
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        dstd::String index_dir = env.get_dir();
        dstd::String index_path = env.get_index_path(gz_file);

        auto indexer = CheckpointIndexerFactory::create(
            gz_file, index_path, 32 * 1024 * 1024, false);
        REQUIRE(indexer != nullptr);
        indexer->build();
        REQUIRE(
            dstd::fs::exists(determine_index_path(gz_file, index_dir)).value());

        TraceReader reader({.file_path = dstd::String(gz_file),
                            .index_dir = dstd::String(index_dir)});
        CHECK(reader.has_index());

        ReadConfig rc;
        rc.query = R"(name == "read")";
        auto lines = collect_lines(reader.read_lines(rc)).get();
        CHECK(lines.size() > 0);
        for (const auto& line : lines) {
            CHECK(line.find("\"name\":\"read\"") != dstd::String::npos);
        }
    }

    TEST_CASE("Query combines with line range") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.start_line = 1;
        rc.end_line = 20;
        rc.query = R"(cat == "POSIX")";
        auto matched = count_lines(reader.read_lines(rc)).get();
        CHECK(matched <= 20);
    }

    TEST_CASE("Empty query string reads all") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        auto all = count_lines(reader.read_lines()).get();

        ReadConfig rc;
        rc.query = "";
        auto with_empty = count_lines(reader.read_lines(rc)).get();

        CHECK(all == with_empty);
    }

    TEST_CASE("Query with index filters events per-line") {
        TestEnvironment env(100);
        dstd::String pfw = env.get_dir() + "/multi_cat.pfw";
        {
            dstd::String out;
            for (int i = 0; i < 200; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
                    (1000 + i), R"(,"dur":10,"args":{}})", "\n");
            }
            for (int i = 0; i < 200; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"train","cat":"COMPUTE","pid":1,"tid":1,"ts":)",
                    (100000 + i), R"(,"dur":500,"args":{}})", "\n");
            }
            (void)dstd::fs::write_file(dstd::PathView(pfw), out);
        }
        dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        (void)dstd::fs::remove(pfw);

        REQUIRE(dftu_utils_test::build_index(gz));

        TraceReader reader({.file_path = dstd::String(gz)});
        REQUIRE(reader.has_index());

        auto all = count_lines(reader.read_lines()).get();
        REQUIRE(all == 400);

        ReadConfig rc_posix;
        rc_posix.query = R"(cat == "POSIX")";
        auto posix_lines = collect_lines(reader.read_lines(rc_posix)).get();
        CHECK(posix_lines.size() == 200);
        for (const auto& line : posix_lines) {
            CHECK(line.find("\"cat\":\"POSIX\"") != dstd::String::npos);
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
        dstd::String pfw = env.get_dir() + "/raw_skip.pfw";
        {
            dstd::String out;
            for (int i = 0; i < 100; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
                    (1000 + i), R"(,"dur":10,"args":{}})", "\n");
            }
            (void)dstd::fs::write_file(dstd::PathView(pfw), out);
        }
        dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        (void)dstd::fs::remove(pfw);

        REQUIRE(dftu_utils_test::build_index(gz));

        TraceReader reader({.file_path = dstd::String(gz)});
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

    TEST_CASE("Chunk pruning skips non-matching checkpoints") {
        TestEnvironment env(100);
        dstd::String pfw = env.get_dir() + "/multi_ckpt.pfw";
        constexpr int POSIX_BEFORE = 100;
        constexpr int COMPUTE_COUNT = 5;
        constexpr int POSIX_AFTER = 100;
        constexpr int TOTAL = POSIX_BEFORE + COMPUTE_COUNT + POSIX_AFTER;
        // Each line is ~550 bytes (padded args). 205 events * 550 = ~112KB.
        // With 32KB checkpoint window -> 3-4 checkpoints.
        // COMPUTE events cluster in one checkpoint in the middle.
        dstd::String pad(400, 'x');
        {
            dstd::String out;
            for (int i = 0; i < POSIX_BEFORE; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
                    (1000 + i), R"(,"dur":10,"args":{"pad":")", pad, R"("}})",
                    "\n");
            }
            for (int i = 0; i < COMPUTE_COUNT; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"train","cat":"COMPUTE","pid":2,"tid":2,"ts":)",
                    (100000 + i), R"(,"dur":500,"args":{"pad":")", pad,
                    R"("}})", "\n");
            }
            for (int i = 0; i < POSIX_AFTER; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"write","cat":"POSIX","pid":1,"tid":1,"ts":)",
                    (200000 + i), R"(,"dur":10,"args":{"pad":")", pad, R"("}})",
                    "\n");
            }
            (void)dstd::fs::write_file(dstd::PathView(pfw), out);
        }
        dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        (void)dstd::fs::remove(pfw);

        REQUIRE(dftu_utils_test::build_index(gz, "",
                                             /*checkpoint_size=*/32 * 1024));

        TraceReader reader(
            {.file_path = dstd::String(gz), .checkpoint_size = 32 * 1024});
        REQUIRE(reader.has_index());

        auto all = count_lines(reader.read_lines()).get();
        REQUIRE(all == TOTAL);

        // Selective query: only COMPUTE events (5 out of 205)
        ReadConfig rc_compute;
        rc_compute.query = R"(cat == "COMPUTE")";
        auto compute_lines = collect_lines(reader.read_lines(rc_compute)).get();
        CHECK(compute_lines.size() == COMPUTE_COUNT);
        for (const auto& line : compute_lines) {
            CHECK(line.find("\"cat\":\"COMPUTE\"") != dstd::String::npos);
        }

        // Full category query should still return all POSIX
        ReadConfig rc_posix;
        rc_posix.query = R"(cat == "POSIX")";
        auto posix_lines = collect_lines(reader.read_lines(rc_posix)).get();
        CHECK(posix_lines.size() == POSIX_BEFORE + POSIX_AFTER);

        // No match
        ReadConfig rc_none;
        rc_none.query = R"(cat == "NONEXISTENT")";
        auto none_lines = count_lines(reader.read_lines(rc_none)).get();
        CHECK(none_lines == 0);
    }
}

TEST_SUITE("TraceReader::read_json") {
    TEST_CASE("read_json returns parsed events") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        auto events = collect_json_events(reader.read_json()).get();
        CHECK(events.size() > 0);
        for (const auto& ev : events) {
            CHECK_FALSE(ev.ph.empty());
        }
    }

    TEST_CASE("read_json count matches read_lines count") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        auto line_count = count_lines(reader.read_lines()).get();
        auto json_count = count_json_lines(reader.read_json()).get();
        CHECK(json_count <= line_count);
        CHECK(json_count > 0);
    }

    TEST_CASE("read_json query filters events") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        auto all = count_json_lines(reader.read_json()).get();
        REQUIRE(all > 0);

        ReadConfig rc;
        rc.query = R"(cat == "POSIX")";
        auto events = collect_json_events(reader.read_json(rc)).get();
        CHECK(events.size() > 0);
        CHECK(events.size() <= all);
        for (const auto& ev : events) {
            CHECK(ev.cat == "POSIX");
        }
    }

    TEST_CASE("read_json query with no matches returns zero") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.query = R"(cat == "NONEXISTENT")";
        auto n = count_json_lines(reader.read_json(rc)).get();
        CHECK(n == 0);
    }

    TEST_CASE("read_json with AND query") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.query = R"(cat == "POSIX" and name == "read")";
        auto events = collect_json_events(reader.read_json(rc)).get();
        CHECK(events.size() > 0);
        for (const auto& ev : events) {
            CHECK(ev.cat == "POSIX");
            CHECK(ev.name == "read");
        }
    }

    TEST_CASE("read_json matches read_lines query count") {
        TestEnvironment env(100);
        dstd::String pfw = env.get_dir() + "/json_vs_lines.pfw";
        {
            dstd::String out;
            for (int i = 0; i < 100; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
                    (1000 + i), R"(,"dur":10,"args":{}})", "\n");
            }
            for (int i = 0; i < 50; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"train","cat":"COMPUTE","pid":2,"tid":2,"ts":)",
                    (100000 + i), R"(,"dur":500,"args":{}})", "\n");
            }
            (void)dstd::fs::write_file(dstd::PathView(pfw), out);
        }

        auto gz = gzip_fixture(pfw);

        TraceReader reader({.file_path = dstd::String(gz)});

        ReadConfig rc;
        rc.query = R"(cat == "POSIX")";
        auto line_count = count_lines(reader.read_lines(rc)).get();
        auto json_count = count_json_lines(reader.read_json(rc)).get();
        CHECK(line_count == json_count);
        CHECK(json_count == 100);

        (void)dstd::fs::remove(gz);
    }

    TEST_CASE("read_json works with index and chunk pruning") {
        TestEnvironment env(100);
        dstd::String pfw = env.get_dir() + "/json_indexed.pfw";
        dstd::String pad(400, 'x');
        {
            dstd::String out;
            for (int i = 0; i < 100; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
                    (1000 + i), R"(,"dur":10,"args":{"pad":")", pad, R"("}})",
                    "\n");
            }
            for (int i = 0; i < 5; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"train","cat":"COMPUTE","pid":2,"tid":2,"ts":)",
                    (100000 + i), R"(,"dur":500,"args":{"pad":")", pad,
                    R"("}})", "\n");
            }
            for (int i = 0; i < 100; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"write","cat":"POSIX","pid":1,"tid":1,"ts":)",
                    (200000 + i), R"(,"dur":10,"args":{"pad":")", pad, R"("}})",
                    "\n");
            }
            (void)dstd::fs::write_file(dstd::PathView(pfw), out);
        }
        dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        (void)dstd::fs::remove(pfw);

        REQUIRE(dftu_utils_test::build_index(gz, "",
                                             /*checkpoint_size=*/32 * 1024));

        TraceReader reader(
            {.file_path = dstd::String(gz), .checkpoint_size = 32 * 1024});
        REQUIRE(reader.has_index());

        ReadConfig rc;
        rc.query = R"(cat == "COMPUTE")";
        auto events = collect_json_events(reader.read_json(rc)).get();
        CHECK(events.size() == 5);
        for (const auto& ev : events) {
            CHECK(ev.cat == "COMPUTE");
        }

        ReadConfig rc_posix;
        rc_posix.query = R"(cat == "POSIX")";
        auto posix_count = count_json_lines(reader.read_json(rc_posix)).get();
        CHECK(posix_count == 200);

        ReadConfig rc_none;
        rc_none.query = R"(cat == "NONEXISTENT")";
        auto none_count = count_json_lines(reader.read_json(rc_none)).get();
        CHECK(none_count == 0);
    }

    TEST_CASE("read_json parser fields are accessible") {
        auto test_file = make_unique_test_path("json_parser_fields.pfw");
        {
            (void)dstd::fs::write_file(
                dstd::PathView(test_file),
                common::str_cat(
                    R"({"ph":"X","name":"read","cat":"POSIX","pid":42,"tid":7,"ts":1000,"dur":10,"args":{"ret":1}})",
                    "\n"));
        }

        auto gz_file = gzip_fixture(dstd::String(test_file.str()));

        TraceReader reader({.file_path = dstd::String(gz_file)});
        auto events = collect_json_events(reader.read_json()).get();
        REQUIRE(events.size() == 1);
        CHECK(events[0].name == "read");
        CHECK(events[0].cat == "POSIX");
        CHECK(events[0].ph == "X");

        (void)dstd::fs::remove(gz_file);
    }
}

namespace {

// Lines written to a gzip trace, read back through read_json and read_arrow
// with and without an index. Returns the line counts the two readers keep.
struct KeptCounts {
    std::size_t json;
    std::size_t arrow;
};

KeptCounts read_kept(const dstd::Vector<dstd::String>& lines,
                     const dstd::String& query, bool indexed) {
    dstd::String content;
    for (const auto& l : lines) content += l + "\n";
    const dstd::Path dir = make_unique_test_path("malformed_reader");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw((dir / "trace.pfw").str());
    REQUIRE(dstd::fs::write_file(dstd::PathView(pfw), content).has_value());
    const dstd::String gz = gzip_fixture(pfw);
    if (indexed) REQUIRE(dftu_utils_test::build_index(gz));
    TraceReader reader({.file_path = gz});
    REQUIRE(reader.has_index() == indexed);
    ReadConfig rc;
    rc.query = query;
    KeptCounts out;
    out.json = count_json_lines(reader.read_json(rc)).get();
    out.arrow = count_arrow_rows(reader.read_arrow(rc)).get();
    return out;
}

}  // namespace

TEST_SUITE("TraceReader malformed lines") {
    TEST_CASE("a field filter drops a line whose read part is bad") {
        // `x == 1 or name == "read"` reads every field of a line, so a bad
        // value anywhere in the line drops it, as Query::matches does.
        const dstd::Vector<dstd::String> lines = {
            R"({"name":"read","dur":5})", R"({"x":01,"name":"read"})",
            R"({"name":"read","x":01})",  R"({"name":"read","x":tru,"y":1})",
            R"({"name":"read","x":-})",   R"({"name":"read","args":{"x":01}})",
            R"({"name":"read","x":1x})",
        };
        for (const bool indexed : {false, true}) {
            CAPTURE(indexed);
            const auto kept =
                read_kept(lines, R"(x == 1 or name == "read")", indexed);
            CHECK(kept.json == 1);
            CHECK(kept.arrow == 1);
        }
    }

    TEST_CASE("a bigint field is not a reason to drop the line") {
        const dstd::Vector<dstd::String> lines = {
            R"({"name":"read","x":123456789012345678901234567890})",
            R"({"name":"write","x":1})",
        };
        for (const bool indexed : {false, true}) {
            CAPTURE(indexed);
            const auto kept =
                read_kept(lines, R"(x == 1 or name == "read")", indexed);
            CHECK(kept.json == 2);
            CHECK(kept.arrow == 2);
        }
    }

    TEST_CASE("a field filter walks the whole line, an expression only reads") {
        // The field-map path reads every field, so the bad `dur` drops the
        // line. An expression query reads only `name`, so the line stays.
        const dstd::Vector<dstd::String> lines = {
            R"({"name":"read","dur":})",
        };
        for (const bool indexed : {false, true}) {
            CAPTURE(indexed);
            const auto field_filter =
                read_kept(lines, R"(name == "read" or pid == 1)", indexed);
            CHECK(field_filter.json == 0);
            CHECK(field_filter.arrow == 0);
            const auto expression =
                read_kept(lines, R"(lower(name) == "read")", indexed);
            CHECK(expression.json == 1);
        }
    }

    TEST_CASE(
        "a line with mismatched brackets never stops the lines around it") {
        // Each line is its own document: a bad line is dropped or kept on what
        // is read from it, and every good line around it is read exactly once,
        // with the same count on the indexed and the plain path.
        struct Bad {
            const char* text;
            std::size_t extra;  // lines it may add when named "read"
        };
        const Bad bad_templates[] = {
            {R"({"name":"NAME","dur":5,"yy":[1,2})", 1},
            {R"({"name":"NAME","dur":5,"yy":{"a":1]})", 1},
            {R"({"name":"NAME","dur":5,"yy":{"a":[1,2}})", 1},
            {R"({"name":"NAME","dur":5,"args":{"yy":[1,2},"n":1})", 1},
            {R"({"name":"NAME","dur":5,"x":")", 1},
            {R"({"name":"NAME","dur":5)", 1},
            {R"([{"name":"NAME","dur":5)", 1},
            {R"({"name":"NAME","dur":5}})", 1},
            {R"({"name":"NAME","dur":5} {"name":"NAME","dur":5})", 2},
            {"{", 0},
            {"[", 0},
            {"}", 0},
        };
        const dstd::String good = R"({"name":"read","dur":5})";
        const dstd::String long_good = R"({"name":"read","dur":5,"s":")" +
                                       dstd::String(1500000, 'a') + R"("})";
        const dstd::String long_bad =
            R"({"name":"bad","dur":5,"s":")" + dstd::String(1500000, 'a');
        const char* const queries[] = {
            R"(name == "read" and dur > 1)",
            R"(lower(name) == "read" and dur > 1)",
        };
        std::size_t shapes = 0;
        const auto check = [&](const dstd::Vector<dstd::String>& lines,
                               std::size_t good_lines, std::size_t extra,
                               const char* query) {
            const auto plain = read_kept(lines, query, false);
            const auto indexed = read_kept(lines, query, true);
            CAPTURE(query);
            CAPTURE(lines.size());
            CHECK(plain.json >= good_lines);
            CHECK(plain.json <= good_lines + extra);
            CHECK(plain.arrow >= good_lines);
            CHECK(plain.arrow <= good_lines + extra);
            CHECK(indexed.json == plain.json);
            CHECK(indexed.arrow == plain.arrow);
            ++shapes;
        };
        for (const bool crlf : {false, true}) {
            const dstd::String eol = crlf ? "\r" : "";
            for (const auto& bad_template : bad_templates) {
                for (const bool named_read : {false, true}) {
                    dstd::String bad = bad_template.text;
                    for (auto at = bad.find("NAME"); at != dstd::String::npos;
                         at = bad.find("NAME"))
                        bad.replace(at, 4, named_read ? "read" : "bad");
                    const dstd::Vector<dstd::Vector<dstd::String>> orders = {
                        {bad, good},
                        {good, bad},
                        {good, bad, good},
                        {bad, good, good},
                        {bad, bad, good}};
                    for (auto lines : orders) {
                        std::size_t good_lines = 0;
                        for (auto& l : lines) {
                            good_lines += l == good;
                            l += eol;
                        }
                        for (const char* query : queries)
                            check(lines, good_lines,
                                  named_read ? bad_template.extra * 2 : 0,
                                  query);
                    }
                }
            }
        }
        // A line longer than any batch, good or unterminated, at each place.
        for (const char* query : queries) {
            check({long_good, good, good}, 3, 0, query);
            check({good, long_good, good}, 3, 0, query);
            check({good, good, long_good}, 3, 0, query);
            check({long_bad, good, good}, 2, 0, query);
            check({good, long_bad, good}, 2, 0, query);
            check({good, good, long_bad}, 2, 0, query);
        }
        CHECK(shapes > 400);
    }

    TEST_CASE("a line holding two documents is read as its first one") {
        const dstd::String two =
            R"({"name":"read","dur":5} {"name":"read","dur":7})";
        const dstd::Vector<dstd::String> lines = {two,
                                                  R"({"name":"read","dur":5})"};
        for (const char* query : {R"(name == "read" and dur > 1)",
                                  R"(lower(name) == "read" and dur > 1)"}) {
            for (const bool indexed : {false, true}) {
                CAPTURE(query);
                CAPTURE(indexed);
                const auto kept = read_kept(lines, query, indexed);
                CHECK(kept.json == 2);
                CHECK(kept.arrow == 2);
            }
        }
    }
}

namespace {

static CoroTask<dstd::Vector<dstd::String>> collect_json_text(
    AsyncGenerator<JsonLine> gen) {
    dstd::Vector<dstd::String> out;
    while (auto line = co_await gen.next()) out.emplace_back(line->content);
    co_return out;
}

struct WindowFixture {
    dstd::String gz;
    dstd::Vector<dstd::String> lines;
    dstd::Vector<std::size_t> starts;
    std::size_t total = 0;
};

// Lines long and short around the sizes a line start search could assume.
WindowFixture window_fixture(std::size_t rounds, std::size_t checkpoint_size) {
    WindowFixture f;
    dstd::String content;
    const std::size_t sizes[] = {40, 400, 511, 512, 513, 600, 3000, 20000};
    static const char HEX[] = "0123456789abcdef";
    std::mt19937_64 rng(7);
    for (std::size_t round = 0; round < rounds; ++round) {
        for (const std::size_t size : sizes) {
            dstd::String line =
                common::str_cat(R"({"name":"read","ph":"X","id":)",
                                f.lines.size(), R"(,"pad":")");
            for (std::size_t i = 0; i < size; ++i)
                line.push_back(HEX[rng() & 15]);
            line += R"("})";
            f.starts.push_back(content.size());
            content += line;
            content += "\n";
            f.lines.push_back(std::move(line));
        }
    }
    f.total = content.size();
    const dstd::Path dir = make_unique_test_path("byte_windows");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw((dir / "trace.pfw").str());
    REQUIRE(dstd::fs::write_file(dstd::PathView(pfw), content).has_value());
    f.gz = gzip_fixture(pfw);
    REQUIRE(dftu_utils_test::build_index(f.gz, "", checkpoint_size));
    return f;
}

void check_windows(const WindowFixture& f, dstd::Vector<std::size_t> cuts) {
    cuts.push_back(0);
    cuts.push_back(f.total);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    dstd::Vector<dstd::String> raw;
    dstd::Vector<dstd::String> json;
    std::size_t arrow = 0;
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        ReadConfig rc;
        rc.start_byte = cuts[i];
        rc.end_byte = cuts[i + 1];
        TraceReader reader({.file_path = f.gz});
        REQUIRE(reader.has_index());
        for (auto& l : collect_lines(reader.read_lines(rc)).get())
            raw.push_back(std::move(l));
        for (auto& l : collect_json_text(reader.read_json(rc)).get())
            json.push_back(std::move(l));
        arrow += count_arrow_rows(reader.read_arrow(rc)).get();
    }
    CHECK(raw == f.lines);
    CHECK(json == f.lines);
    CHECK(arrow == f.lines.size());
}

}  // namespace

TEST_SUITE("TraceReader byte windows") {
    TEST_CASE("adjacent windows give each line once and whole, at any cut") {
        const WindowFixture f = window_fixture(1, 1024 * 1024);
        for (std::size_t i = 0; i < f.lines.size(); ++i) {
            const std::size_t size = f.lines[i].size();
            for (const std::size_t at :
                 {std::size_t{0}, std::size_t{1}, std::size_t{511},
                  std::size_t{512}, std::size_t{513}, size - 1, size}) {
                if (at > size) continue;
                CAPTURE(i);
                CAPTURE(at);
                check_windows(f, {f.starts[i] + at});
            }
        }
    }

    TEST_CASE("a window near a restart piece finds its line") {
        const std::size_t checkpoint = 1024 * 1024;
        const WindowFixture f = window_fixture(480, checkpoint);
        const auto indexer = CheckpointIndexerFactory::create(
            f.gz, determine_index_path(f.gz, ""), checkpoint, false);
        REQUIRE(indexer != nullptr);
        std::size_t mid_line_pieces = 0;
        dstd::Vector<std::size_t> cuts;
        for (const auto& member : indexer->get_members()) {
            if (member.kind != GzipRecordKind::Restart) continue;
            const auto at = static_cast<std::size_t>(member.uc_offset);
            mid_line_pieces +=
                !std::binary_search(f.starts.begin(), f.starts.end(), at);
            for (const std::size_t cut :
                 {at - 600, at - 1, at, at + 1, at + 600})
                cuts.push_back(cut);
        }
        CHECK(mid_line_pieces > 0);
        check_windows(f, cuts);
    }
}

namespace {

// What the two readers give back for lines that hold a number too large for a
// double or a string with an invalid escape.
struct OddRead {
    dstd::Vector<dstd::String> lines;
    dstd::Vector<dstd::Optional<double>> json_doubles;
    dstd::Vector<dstd::Optional<dstd::String>> json_strings;
    std::size_t arrow_rows = 0;
};

static CoroTask<void> collect_odd_json(AsyncGenerator<JsonLine> gen,
                                       OddRead& out) {
    while (auto opt = co_await gen.next()) {
        out.lines.emplace_back(opt->content);
        auto* p = opt->parser;
        out.json_doubles.push_back(p->get_double("x"));
        p->rewind();
        auto s = p->get_string("x");
        out.json_strings.push_back(
            s ? dstd::Optional<dstd::String>(dstd::String(*s)) : dstd::nullopt);
    }
}

// The values of column `name` over every row, as doubles or strings.
static CoroTask<void> collect_odd_arrow(
    AsyncGenerator<dftracer::utils::utilities::arrow::ArrowExportResult> gen,
    const char* name, dstd::Vector<dstd::Optional<double>>& doubles,
    dstd::Vector<dstd::Optional<dstd::String>>& strings, std::size_t& rows) {
    while (auto opt = co_await gen.next()) {
        ArrowArrayView view;
        if (dftracer::utils::utilities::arrow::init_array_view(
                view, opt->get_schema(), opt->get_array()) != NANOARROW_OK)
            co_return;
        int column = -1;
        for (std::int64_t i = 0; i < opt->get_schema()->n_children; ++i)
            if (std::strcmp(opt->get_schema()->children[i]->name, name) == 0)
                column = static_cast<int>(i);
        for (std::int64_t r = 0; r < view.length; ++r) {
            ++rows;
            if (column < 0 || ArrowArrayViewIsNull(view.children[column], r)) {
                doubles.push_back(dstd::nullopt);
                strings.push_back(dstd::nullopt);
                continue;
            }
            const ArrowArrayView* child = view.children[column];
            if (child->storage_type == NANOARROW_TYPE_STRING) {
                const ArrowStringView text =
                    ArrowArrayViewGetStringUnsafe(child, r);
                strings.push_back(dstd::String(
                    text.data, static_cast<std::size_t>(text.size_bytes)));
                doubles.push_back(dstd::nullopt);
            } else {
                doubles.push_back(ArrowArrayViewGetDoubleUnsafe(child, r));
                strings.push_back(dstd::nullopt);
            }
        }
        ArrowArrayViewReset(&view);
    }
}

dstd::String write_odd_trace(const dstd::Vector<dstd::String>& lines,
                             bool indexed) {
    dstd::String content;
    for (const auto& l : lines) content += l + "\n";
    const dstd::Path dir = make_unique_test_path("odd_reader");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw((dir / "trace.pfw").str());
    REQUIRE(dstd::fs::write_file(dstd::PathView(pfw), content).has_value());
    const dstd::String gz = gzip_fixture(pfw);
    if (indexed) REQUIRE(dftu_utils_test::build_index(gz));
    return gz;
}

}  // namespace

TEST_SUITE("TraceReader valid but odd values") {
    // The odd line comes first and its `x` is the value under test; the line
    // after it must be read whatever the first one holds.
    struct Odd {
        const char* name;
        const char* line;
        const char* selective;   // matches the odd line only
        const char* expression;  // the same, as an expression query
        bool is_string;
        double expected_double;
        const char* expected_string;
    };
    constexpr double INF = std::numeric_limits<double>::infinity();
    const Odd cases[] = {
        {"plus_overflow", R"({"name":"read","x":1e400})", "x > 1",
         R"(lower(name) == "read" and x > 1)", false, INF, nullptr},
        {"minus_overflow", R"({"name":"read","x":-1e400})", "x < -1",
         R"(lower(name) == "read" and x < -1)", false, -INF, nullptr},
        {"big_exponent", R"({"name":"read","x":1.5E+999})", "x > 1",
         R"(lower(name) == "read" and x > 1)", false, INF, nullptr},
        {"bad_escape", R"({"name":"read","x":"a\qb"})", R"(x != "ok")",
         R"(lower(x) == "a\\qb")", true, 0, R"(a\qb)"},
        {"high_surrogate", R"({"name":"read","x":"\ud800"})", R"(x != "ok")",
         R"(lower(x) == "\\ud800")", true, 0, R"(\ud800)"},
        {"low_surrogate", R"({"name":"read","x":"\udc00"})", R"(x != "ok")",
         R"(lower(x) == "\\udc00")", true, 0, R"(\udc00)"},
        {"short_unicode", R"({"name":"read","x":"\u12"})", R"(x != "ok")",
         R"(lower(x) == "\\u12")", true, 0, R"(\u12)"},
    };
    const char* const next_line = R"({"name":"read","dur":7,"y":0})";
    const char* const next_string_line =
        R"({"name":"read","dur":7,"y":0,"x":"ok"})";

    TEST_CASE("a value read by the query keeps the line and reads as is") {
        for (const Odd& odd : cases) {
            for (const bool indexed : {false, true}) {
                for (const bool expression : {false, true}) {
                    INFO(dstd::String(odd.name)
                         << " indexed=" << indexed
                         << " expression=" << expression);
                    const dstd::Vector<dstd::String> lines = {
                        odd.line, odd.is_string ? next_string_line : next_line};
                    const dstd::String gz = write_odd_trace(lines, indexed);
                    TraceReader reader({.file_path = gz});
                    REQUIRE(reader.has_index() == indexed);

                    ReadConfig selective;
                    selective.query =
                        expression ? odd.expression : odd.selective;
                    selective.flatten_objects = true;
                    OddRead json;
                    collect_odd_json(reader.read_json(selective), json).get();
                    REQUIRE(json.lines.size() == 1);
                    CHECK(json.lines[0] == odd.line);
                    if (odd.is_string) {
                        REQUIRE(json.json_strings[0].has_value());
                        CHECK(*json.json_strings[0] == odd.expected_string);
                    } else {
                        REQUIRE(json.json_doubles[0].has_value());
                        CHECK(*json.json_doubles[0] == odd.expected_double);
                    }
                    dstd::Vector<dstd::Optional<double>> doubles;
                    dstd::Vector<dstd::Optional<dstd::String>> strings;
                    std::size_t rows = 0;
                    collect_odd_arrow(reader.read_arrow(selective), "x",
                                      doubles, strings, rows)
                        .get();
                    REQUIRE(rows == 1);
                    if (odd.is_string) {
                        REQUIRE(strings[0].has_value());
                        CHECK(*strings[0] == odd.expected_string);
                    } else {
                        REQUIRE(doubles[0].has_value());
                        CHECK(*doubles[0] == odd.expected_double);
                    }

                    ReadConfig both;
                    both.query = expression
                                     ? R"(x > 5 or lower(name) == "read")"
                                     : R"(x > 5 or name == "read")";
                    OddRead all;
                    collect_odd_json(reader.read_json(both), all).get();
                    CHECK(all.lines.size() == 2);
                    CHECK(all.lines[1] == lines[1]);
                    std::size_t arrow_rows = 0;
                    dstd::Vector<dstd::Optional<double>> d2;
                    dstd::Vector<dstd::Optional<dstd::String>> s2;
                    collect_odd_arrow(reader.read_arrow(both), "x", d2, s2,
                                      arrow_rows)
                        .get();
                    CHECK(arrow_rows == 2);
                }
            }
        }
    }

    TEST_CASE("a name with an invalid escape keeps the line") {
        const dstd::Vector<dstd::String> shapes = {
            R"({"name":"read","k\q":1,"x":5})",
            R"({"name":"read","args":{"k\q":1,"x":5}})",
            R"({"name":"read","args":{"p":{"k\ud800":1},"x":5}})",
        };
        for (const auto& shape : shapes) {
            for (const bool indexed : {false, true}) {
                for (const char* query :
                     {"x == 5", R"(lower(name) == "read" and x == 5)",
                      R"(lower(name) == "read")"}) {
                    INFO(shape << " indexed=" << indexed << " " << query);
                    const dstd::Vector<dstd::String> lines = {
                        shape, R"({"name":"read","x":0})"};
                    const dstd::String gz = write_odd_trace(lines, indexed);
                    TraceReader reader({.file_path = gz});
                    ReadConfig rc;
                    rc.query = query;
                    const bool selects_one =
                        std::strcmp(query, "x == 5") == 0 ||
                        std::strstr(query, "and x == 5");
                    const std::size_t expected = selects_one ? 1 : 2;
                    CHECK(count_json_lines(reader.read_json(rc)).get() ==
                          expected);
                    CHECK(count_arrow_rows(reader.read_arrow(rc)).get() ==
                          expected);
                }
            }
        }
    }

    TEST_CASE("text that is no number or an unterminated string is dropped") {
        const char* const bad_values[] = {"tru", "01", "1x", "-",
                                          "1.",  "1e", "\"x"};
        for (const char* bad : bad_values) {
            for (const bool indexed : {false, true}) {
                INFO(dstd::String(bad) << " indexed=" << indexed);
                const dstd::Vector<dstd::String> lines = {
                    common::str_cat(R"({"name":"read","x":)", bad,
                                    R"(,"y":1})"),
                    R"({"name":"read","x":2,"y":1})"};
                const dstd::String gz = write_odd_trace(lines, indexed);
                TraceReader reader({.file_path = gz});
                ReadConfig rc;
                rc.query = R"(x == 1 or name == "read")";
                const std::size_t bad_kept =
                    count_json_lines(reader.read_json(rc)).get();
                CHECK(bad_kept <= 1);
                CHECK(count_arrow_rows(reader.read_arrow(rc)).get() ==
                      bad_kept);
            }
        }
    }
}
