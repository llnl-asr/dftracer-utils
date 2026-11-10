// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/scope_exit.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <generator_ok.h>
#include <refused_lines.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>

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

// Counts the lines of a generator of Results; an error item ends the count and
// is the result.
static CoroTask<dstd::Result<std::size_t>> count_lines_until_error(
    AsyncGenerator<
        dstd::Result<dftracer::utils::utilities::fileio::lines::Line>>
        gen) {
    std::size_t n = 0;
    while (auto item = co_await gen.next()) {
        if (!item->has_value()) co_return item->error();
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
    dstd::fs::remove(plain_path).discard();
    return gz_path;
}
}  // namespace

TEST_SUITE("TraceReader") {
    TEST_CASE("Read all lines without index") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = dstd::String(gz_file)});

        CHECK_FALSE(reader.has_index());

        auto n = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines())).get());
        CHECK(n > 0);
        CHECK(n == 102);
    }

    TEST_CASE("Read all lines with pre-built index") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        dstd::String index_dir = env.dir();
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

        auto n_indexed = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines())).get());
        CHECK(n_indexed > 0);
        CHECK(n_indexed == 102);

        SUBCASE("Indexed and unindexed counts match") {
            // Remove the index and re-read to compare.
            dstd::fs::remove_all(determine_index_path(gz_file, index_dir))
                .has_value();
            TraceReader plain_reader({.file_path = dstd::String(gz_file)});
            CHECK_FALSE(plain_reader.has_index());
            auto n_plain =
                dftu_utils_test::ok(count_lines(dftu_utils_test::ok_items(
                                                    plain_reader.read_lines()))
                                        .get());
            CHECK(n_plain == n_indexed);
        }
    }

    TEST_CASE("An index that cannot be read is an error, not an empty read") {
        if (geteuid() == 0) return;  // root reads every file
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        dstd::String index_dir = env.dir();
        dstd::String index_path = env.get_index_path(gz_file);

        auto indexer = CheckpointIndexerFactory::create(
            gz_file, index_path, 32 * 1024 * 1024, false);
        REQUIRE(indexer != nullptr);
        indexer->build();
        const dstd::String root = determine_index_path(gz_file, index_dir);
        REQUIRE(dstd::fs::exists(root).value());

        // The build's open handle is gone and the store directory stops being
        // readable.
        indexer.reset();
        dftracer::utils::index::release_root(root);
        const dstd::String store = dftracer::utils::index::store_dir(root);
        REQUIRE(dstd::fs::set_permissions(store, 0).has_value());
        auto restore = dstd::defer(
            [&store] { (void)dstd::fs::set_permissions(store, 0755); });

        TraceReader reader({.file_path = dstd::String(gz_file),
                            .index_dir = dstd::String(index_dir)});
        auto counted = count_lines_until_error(reader.read_lines()).get();
        REQUIRE_FALSE(counted.has_value());
        CHECK_FALSE(counted.error().message().empty());
    }

    TEST_CASE("Query nested args field by bare and dotted name") {
        // Test data events carry args:{"ret":1024*i, ...}. Bare `ret` and
        // dotted `args.ret` must select the same events in the ValueMap path.
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        dstd::String index_dir = env.dir();
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
        auto n_bare = dftu_utils_test::ok(
            count_json_lines(dftu_utils_test::ok_items(reader.read_json(bare)))
                .get());

        ReadConfig dotted;
        dotted.query = "args.ret >= 100000";
        auto n_dotted =
            dftu_utils_test::ok(count_json_lines(dftu_utils_test::ok_items(
                                                     reader.read_json(dotted)))
                                    .get());

        CHECK(n_bare > 0);
        CHECK(n_dotted == n_bare);
    }

    TEST_CASE("Read with start_line skip") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = dstd::String(gz_file)});

        auto n_full = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines())).get());
        REQUIRE(n_full > 0);

        ReadConfig rc;
        rc.start_line = 50;
        auto n_skipped = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());
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
        auto nums =
            dftu_utils_test::ok(collect_line_numbers(dftu_utils_test::ok_items(
                                                         reader.read_lines(rc)))
                                    .get());
        CHECK(nums.size() <= 10);
        CHECK(nums.size() > 0);
    }

    TEST_CASE("read_raw returns non-empty spans") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = dstd::String(gz_file)});
        auto total = dftu_utils_test::ok(
            count_raw_bytes(dftu_utils_test::ok_items(reader.read_raw()))
                .get());
        CHECK(total > 0);
    }

    TEST_CASE("read_raw line_aligned=false returns raw bytes") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = dstd::String(gz_file)});
        ReadConfig rc;
        rc.line_aligned = false;
        auto total = dftu_utils_test::ok(
            count_raw_bytes(dftu_utils_test::ok_items(reader.read_raw(rc)))
                .get());
        CHECK(total > 0);
    }

    TEST_CASE("read_raw multi_line=false yields single lines") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);

        TraceReader reader({.file_path = dstd::String(gz_file)});
        ReadConfig rc;
        rc.line_aligned = true;
        rc.multi_line = false;
        auto chunks = dftu_utils_test::ok(
            count_raw_chunks(dftu_utils_test::ok_items(reader.read_raw(rc)))
                .get());
        CHECK(chunks > 0);
    }

    TEST_CASE("Empty file path yields zero lines or an error item") {
        TraceReader reader({.file_path = ""});
        auto counted = count_lines_until_error(reader.read_lines()).get();
        CHECK((!counted.has_value() || counted.value() == 0));
    }

    TEST_CASE("A file that is not gzip is an error item") {
        TestEnvironment env(10);
        dstd::String plain = dstd::fmt<"{}/plain.pfw">(env.dir());
        REQUIRE(
            dstd::fs::write_file(plain, "{\"name\":\"a\"}\n{\"name\":\"b\"}\n")
                .has_value());
        TraceReader reader({.file_path = plain});
        auto counted = count_lines_until_error(reader.read_lines()).get();
        REQUIRE(!counted.has_value());
        CHECK(counted.error().code() ==
              static_cast<std::int32_t>(common::ErrorCode::Reader));
    }

    TEST_CASE("A query that does not parse is an error item") {
        TestEnvironment env(10);
        dstd::String gz_file = env.create_dft_test_gzip_file(10);
        TraceReader reader({.file_path = dstd::String(gz_file)});
        ReadConfig rc;
        rc.query = "name ==";
        auto lines = count_lines_until_error(reader.read_lines(rc)).get();
        CHECK(!lines.has_value());
    }

    TEST_CASE("read_raw with byte range returns subset") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        auto total = dftu_utils_test::ok(
            count_raw_bytes(dftu_utils_test::ok_items(reader.read_raw()))
                .get());
        REQUIRE(total > 0);

        ReadConfig rc;
        rc.start_byte = 100;
        rc.end_byte = total / 2;
        auto subset = dftu_utils_test::ok(
            count_raw_bytes(dftu_utils_test::ok_items(reader.read_raw(rc)))
                .get());
        CHECK(subset > 0);
        CHECK(subset < total);
    }

    TEST_CASE("read_raw byte range works with index") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        dstd::String index_dir = env.dir();
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

        auto total = dftu_utils_test::ok(
            count_raw_bytes(dftu_utils_test::ok_items(reader.read_raw()))
                .get());
        REQUIRE(total > 0);

        ReadConfig rc;
        rc.start_byte = 100;
        rc.end_byte = total / 2;
        auto subset = dftu_utils_test::ok(
            count_raw_bytes(dftu_utils_test::ok_items(reader.read_raw(rc)))
                .get());
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
            dstd::fs::write_file(dstd::PathView(test_file), out).discard();
        }
        auto gz_file = gzip_fixture(dstd::String(test_file.str()));

        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.start_byte = 0;
        rc.end_byte =
            first_line.size() + 1 + dstd::String(R"({"name":"write")").size();

        auto lines = dftu_utils_test::ok(
            collect_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());

        REQUIRE(lines.size() == 2);
        CHECK(lines[0].find(R"("name":"read")") != dstd::String::npos);
        CHECK(lines[1].find(R"("name":"write")") != dstd::String::npos);

        dstd::fs::remove(gz_file).discard();
    }

    TEST_CASE("read_lines plain file byte range skips partial first line") {
        auto test_file =
            make_unique_test_path("trace_reader_plain_skip_partial.pfw");
        {
            dstd::String out;
            out += "alpha\n";
            out += "beta\n";
            out += "gamma\n";
            dstd::fs::write_file(dstd::PathView(test_file), out).discard();
        }
        auto gz_file = gzip_fixture(dstd::String(test_file.str()));

        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.start_byte = 2;
        rc.end_byte = 100;
        auto lines = dftu_utils_test::ok(
            collect_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());

        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "beta");
        CHECK(lines[1] == "gamma");

        dstd::fs::remove(gz_file).discard();
    }

    TEST_CASE("read_raw indexed and unindexed produce same chunk count") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        dstd::String index_dir = env.dir();
        dstd::String index_path = env.get_index_path(gz_file);

        TraceReader plain_reader({.file_path = dstd::String(gz_file)});
        CHECK_FALSE(plain_reader.has_index());
        ReadConfig single_line;
        single_line.line_aligned = true;
        single_line.multi_line = false;
        auto plain_chunks = dftu_utils_test::ok(
            count_raw_chunks(
                dftu_utils_test::ok_items(plain_reader.read_raw(single_line)))
                .get());

        auto indexer = CheckpointIndexerFactory::create(
            gz_file, index_path, 32 * 1024 * 1024, false);
        REQUIRE(indexer != nullptr);
        indexer->build();

        TraceReader indexed_reader({.file_path = dstd::String(gz_file),
                                    .index_dir = dstd::String(index_dir)});
        CHECK(indexed_reader.has_index());
        auto indexed_chunks = dftu_utils_test::ok(
            count_raw_chunks(
                dftu_utils_test::ok_items(indexed_reader.read_raw(single_line)))
                .get());

        CHECK(plain_chunks > 0);
        CHECK(plain_chunks == indexed_chunks);
    }

    TEST_CASE("read_raw small buffer_size produces more chunks") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig large_buf;
        large_buf.buffer_size = 4 * 1024 * 1024;
        auto large_chunks = dftu_utils_test::ok(
            count_raw_chunks(
                dftu_utils_test::ok_items(reader.read_raw(large_buf)))
                .get());

        ReadConfig small_buf;
        small_buf.buffer_size = 256;
        auto small_chunks = dftu_utils_test::ok(
            count_raw_chunks(
                dftu_utils_test::ok_items(reader.read_raw(small_buf)))
                .get());

        CHECK(large_chunks > 0);
        CHECK(small_chunks > 0);
        CHECK(small_chunks >= large_chunks);
    }

    TEST_CASE("Query filters matching events") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        auto all = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines())).get());
        REQUIRE(all > 0);

        ReadConfig rc;
        rc.query = R"(cat == "POSIX")";
        auto matched = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());
        CHECK(matched > 0);
        CHECK(matched <= all);
    }

    TEST_CASE("Query with no matches returns zero lines") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.query = R"(cat == "NONEXISTENT")";
        auto matched = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());
        CHECK(matched == 0);
    }

    TEST_CASE("Query filters by name") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.query = R"(name == "read")";
        auto lines = dftu_utils_test::ok(
            collect_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());
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
            dstd::fs::write_file(dstd::PathView(test_file), out).discard();
        }

        const auto uncompressed_size = dstd::fs::file_size(test_file).value();
        auto gz_file = gzip_fixture(dstd::String(test_file.str()));

        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.start_byte = 0;
        rc.end_byte = uncompressed_size;
        rc.query = R"(name == "write")";
        auto lines = dftu_utils_test::ok(
            collect_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());

        REQUIRE(lines.size() == 1);
        CHECK(lines[0].find(R"("name":"write")") != dstd::String::npos);

        dstd::fs::remove(gz_file).discard();
    }

    TEST_CASE("Query keeps the lines the DOM parser refuses") {
        auto test_file =
            make_unique_test_path("trace_reader_refused_query.pfw");
        {
            dstd::String out;
            out +=
                R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":10,"ph":"X","args":{"ret":1}})"
                "\n";
            for (int k = 0; k < REFUSED_KINDS; ++k)
                out +=
                    refused_line(k, R"("name":"write","cat":"POSIX","pid":2,)"
                                    R"("tid":1,"ts":2,"dur":20,"ph":"X")") +
                    "\n";
            for (int k = 0; k < REFUSED_KINDS; ++k)
                out += refused_line(k, R"("name":"read","cat":"POSIX","pid":3,)"
                                       R"("tid":1,"ts":3,"dur":20,"ph":"X")") +
                       "\n";
            dstd::fs::write_file(dstd::PathView(test_file), out).discard();
        }
        const auto uncompressed_size = dstd::fs::file_size(test_file).value();
        auto gz_file = gzip_fixture(dstd::String(test_file.str()));

        TraceReader reader({.file_path = dstd::String(gz_file)});
        ReadConfig rc;
        rc.start_byte = 0;
        rc.end_byte = uncompressed_size;
        rc.query = R"(name == "write")";
        auto lines = dftu_utils_test::ok(
            collect_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());

        REQUIRE(lines.size() == static_cast<std::size_t>(REFUSED_KINDS));
        for (const auto& line : lines)
            CHECK(line.find(R"("name":"write")") != dstd::String::npos);

        dstd::fs::remove(gz_file).discard();
    }

    TEST_CASE("Query keeps a line nested 100000 deep") {
        auto test_file = make_unique_test_path("trace_reader_deep_query.pfw");
        {
            dstd::String out;
            for (const char* name : {"read", "write"}) {
                out +=
                    R"({"name":")" + dstd::String(name) +
                    R"(","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":10,"ph":"X","args":{"ret":1,"deep":)";
                out.append(100000, '[');
                out.append(100000, ']');
                out += "}}\n";
            }
            dstd::fs::write_file(dstd::PathView(test_file), out).discard();
        }
        const auto uncompressed_size = dstd::fs::file_size(test_file).value();
        auto gz_file = gzip_fixture(dstd::String(test_file.str()));
        TraceReader reader({.file_path = dstd::String(gz_file)});
        ReadConfig rc;
        rc.start_byte = 0;
        rc.end_byte = uncompressed_size;
        rc.query = R"(name == "write")";
        auto lines = dftu_utils_test::ok(
            collect_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].find(R"("name":"write")") != dstd::String::npos);
        CHECK(lines[0].size() > 200000);
        dstd::fs::remove(gz_file).discard();
    }

    TEST_CASE("The time metric is read from a line the DOM parser refuses") {
        for (int k = 0; k < REFUSED_KINDS; ++k) {
            INFO("kind: ", k);
            auto test_file =
                make_unique_test_path("trace_reader_refused_metric.pfw");
            {
                dstd::String out =
                    R"({"name":"CM","cat":"dftracer","pid":1,"tid":1,"ts":1,"ph":"M","args":{"name":"time_metric","value":"NS",)" +
                    refused_member(k) + "}}\n";
                out +=
                    R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"ph":"X","args":{"ret":1}})"
                    "\n";
                dstd::fs::write_file(dstd::PathView(test_file), out).discard();
            }
            auto gz_file = gzip_fixture(dstd::String(test_file.str()));
            TraceReader reader({.file_path = dstd::String(gz_file)});
            CHECK(dftu_utils_test::ok(reader.read_time_metric().get()) ==
                  trace::TimeMetric::Ns);
            dstd::fs::remove(gz_file).discard();
        }
    }

    TEST_CASE("Query with AND narrows results") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc_cat;
        rc_cat.query = R"(cat == "POSIX")";
        auto cat_count = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc_cat)))
                .get());

        ReadConfig rc_both;
        rc_both.query = R"(cat == "POSIX" and name == "read")";
        auto both_count = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc_both)))
                .get());

        CHECK(both_count > 0);
        CHECK(both_count <= cat_count);
    }

    TEST_CASE("Query with OR widens results") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc_read;
        rc_read.query = R"(name == "read")";
        auto read_count = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc_read)))
                .get());

        ReadConfig rc_write;
        rc_write.query = R"(name == "write")";
        auto write_count = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc_write)))
                .get());

        ReadConfig rc_or;
        rc_or.query = R"(name == "read" or name == "write")";
        auto or_count = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc_or)))
                .get());

        CHECK(or_count == read_count + write_count);
    }

    TEST_CASE("Query works with index") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        dstd::String index_dir = env.dir();
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
        auto lines = dftu_utils_test::ok(
            collect_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());
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
        auto matched = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());
        CHECK(matched <= 20);
    }

    TEST_CASE("Empty query string reads all") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        auto all = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines())).get());

        ReadConfig rc;
        rc.query = "";
        auto with_empty = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());

        CHECK(all == with_empty);
    }

    TEST_CASE("Query with index filters events per-line") {
        TestEnvironment env(100);
        dstd::String pfw = env.dir() + "/multi_cat.pfw";
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
            dstd::fs::write_file(dstd::PathView(pfw), out).discard();
        }
        dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        dstd::fs::remove(pfw).discard();

        REQUIRE(dftu_utils_test::build_index(gz));

        TraceReader reader({.file_path = dstd::String(gz)});
        REQUIRE(reader.has_index());

        auto all = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines())).get());
        REQUIRE(all == 400);

        ReadConfig rc_posix;
        rc_posix.query = R"(cat == "POSIX")";
        auto posix_lines =
            dftu_utils_test::ok(collect_lines(dftu_utils_test::ok_items(
                                                  reader.read_lines(rc_posix)))
                                    .get());
        CHECK(posix_lines.size() == 200);
        for (const auto& line : posix_lines) {
            CHECK(line.find("\"cat\":\"POSIX\"") != dstd::String::npos);
        }

        ReadConfig rc_compute;
        rc_compute.query = R"(cat == "COMPUTE")";
        auto compute_lines = dftu_utils_test::ok(
            collect_lines(
                dftu_utils_test::ok_items(reader.read_lines(rc_compute)))
                .get());
        CHECK(compute_lines.size() == 200);

        ReadConfig rc_none;
        rc_none.query = R"(cat == "NONEXISTENT")";
        auto none_lines = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc_none)))
                .get());
        CHECK(none_lines == 0);
    }

    TEST_CASE("Query file-level skip for raw bytes") {
        TestEnvironment env(100);
        dstd::String pfw = env.dir() + "/raw_skip.pfw";
        {
            dstd::String out;
            for (int i = 0; i < 100; ++i) {
                out += common::str_cat(
                    R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
                    (1000 + i), R"(,"dur":10,"args":{}})", "\n");
            }
            dstd::fs::write_file(dstd::PathView(pfw), out).discard();
        }
        dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        dstd::fs::remove(pfw).discard();

        REQUIRE(dftu_utils_test::build_index(gz));

        TraceReader reader({.file_path = dstd::String(gz)});
        REQUIRE(reader.has_index());

        auto all_bytes = dftu_utils_test::ok(
            count_raw_bytes(dftu_utils_test::ok_items(reader.read_raw()))
                .get());
        REQUIRE(all_bytes > 0);

        // No match → file-level skip → zero bytes
        ReadConfig rc_none;
        rc_none.query = R"(cat == "NONEXISTENT")";
        auto none_bytes = dftu_utils_test::ok(
            count_raw_bytes(dftu_utils_test::ok_items(reader.read_raw(rc_none)))
                .get());
        CHECK(none_bytes == 0);

        // Match → all bytes (no per-event filtering for raw)
        ReadConfig rc_posix;
        rc_posix.query = R"(cat == "POSIX")";
        auto posix_bytes =
            dftu_utils_test::ok(count_raw_bytes(dftu_utils_test::ok_items(
                                                    reader.read_raw(rc_posix)))
                                    .get());
        CHECK(posix_bytes == all_bytes);
    }

    TEST_CASE("Chunk pruning skips non-matching checkpoints") {
        TestEnvironment env(100);
        dstd::String pfw = env.dir() + "/multi_ckpt.pfw";
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
            dstd::fs::write_file(dstd::PathView(pfw), out).discard();
        }
        dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        dstd::fs::remove(pfw).discard();

        REQUIRE(dftu_utils_test::build_index(gz, "",
                                             /*checkpoint_size=*/32 * 1024));

        TraceReader reader(
            {.file_path = dstd::String(gz), .checkpoint_size = 32 * 1024});
        REQUIRE(reader.has_index());

        auto all = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines())).get());
        REQUIRE(all == TOTAL);

        // Selective query: only COMPUTE events (5 out of 205)
        ReadConfig rc_compute;
        rc_compute.query = R"(cat == "COMPUTE")";
        auto compute_lines = dftu_utils_test::ok(
            collect_lines(
                dftu_utils_test::ok_items(reader.read_lines(rc_compute)))
                .get());
        CHECK(compute_lines.size() == COMPUTE_COUNT);
        for (const auto& line : compute_lines) {
            CHECK(line.find("\"cat\":\"COMPUTE\"") != dstd::String::npos);
        }

        // Full category query should still return all POSIX
        ReadConfig rc_posix;
        rc_posix.query = R"(cat == "POSIX")";
        auto posix_lines =
            dftu_utils_test::ok(collect_lines(dftu_utils_test::ok_items(
                                                  reader.read_lines(rc_posix)))
                                    .get());
        CHECK(posix_lines.size() == POSIX_BEFORE + POSIX_AFTER);

        // No match
        ReadConfig rc_none;
        rc_none.query = R"(cat == "NONEXISTENT")";
        auto none_lines = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc_none)))
                .get());
        CHECK(none_lines == 0);
    }
}

TEST_SUITE("TraceReader::read_json") {
    TEST_CASE("read_json returns parsed events") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        auto events = dftu_utils_test::ok(
            collect_json_events(dftu_utils_test::ok_items(reader.read_json()))
                .get());
        CHECK(events.size() > 0);
        for (const auto& ev : events) {
            CHECK_FALSE(ev.ph.empty());
        }
    }

    TEST_CASE("read_json count matches read_lines count") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        auto line_count = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines())).get());
        auto json_count = dftu_utils_test::ok(
            count_json_lines(dftu_utils_test::ok_items(reader.read_json()))
                .get());
        CHECK(json_count <= line_count);
        CHECK(json_count > 0);
    }

    TEST_CASE("read_json query filters events") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        auto all = dftu_utils_test::ok(
            count_json_lines(dftu_utils_test::ok_items(reader.read_json()))
                .get());
        REQUIRE(all > 0);

        ReadConfig rc;
        rc.query = R"(cat == "POSIX")";
        auto events = dftu_utils_test::ok(
            collect_json_events(dftu_utils_test::ok_items(reader.read_json(rc)))
                .get());
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
        auto n = dftu_utils_test::ok(
            count_json_lines(dftu_utils_test::ok_items(reader.read_json(rc)))
                .get());
        CHECK(n == 0);
    }

    TEST_CASE("read_json with AND query") {
        TestEnvironment env(100);
        dstd::String gz_file = env.create_dft_test_gzip_file(100);
        TraceReader reader({.file_path = dstd::String(gz_file)});

        ReadConfig rc;
        rc.query = R"(cat == "POSIX" and name == "read")";
        auto events = dftu_utils_test::ok(
            collect_json_events(dftu_utils_test::ok_items(reader.read_json(rc)))
                .get());
        CHECK(events.size() > 0);
        for (const auto& ev : events) {
            CHECK(ev.cat == "POSIX");
            CHECK(ev.name == "read");
        }
    }

    TEST_CASE("read_json matches read_lines query count") {
        TestEnvironment env(100);
        dstd::String pfw = env.dir() + "/json_vs_lines.pfw";
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
            dstd::fs::write_file(dstd::PathView(pfw), out).discard();
        }

        auto gz = gzip_fixture(pfw);

        TraceReader reader({.file_path = dstd::String(gz)});

        ReadConfig rc;
        rc.query = R"(cat == "POSIX")";
        auto line_count = dftu_utils_test::ok(
            count_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                .get());
        auto json_count = dftu_utils_test::ok(
            count_json_lines(dftu_utils_test::ok_items(reader.read_json(rc)))
                .get());
        CHECK(line_count == json_count);
        CHECK(json_count == 100);

        dstd::fs::remove(gz).discard();
    }

    TEST_CASE("read_json works with index and chunk pruning") {
        TestEnvironment env(100);
        dstd::String pfw = env.dir() + "/json_indexed.pfw";
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
            dstd::fs::write_file(dstd::PathView(pfw), out).discard();
        }
        dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        dstd::fs::remove(pfw).discard();

        REQUIRE(dftu_utils_test::build_index(gz, "",
                                             /*checkpoint_size=*/32 * 1024));

        TraceReader reader(
            {.file_path = dstd::String(gz), .checkpoint_size = 32 * 1024});
        REQUIRE(reader.has_index());

        ReadConfig rc;
        rc.query = R"(cat == "COMPUTE")";
        auto events = dftu_utils_test::ok(
            collect_json_events(dftu_utils_test::ok_items(reader.read_json(rc)))
                .get());
        CHECK(events.size() == 5);
        for (const auto& ev : events) {
            CHECK(ev.cat == "COMPUTE");
        }

        ReadConfig rc_posix;
        rc_posix.query = R"(cat == "POSIX")";
        auto posix_count = dftu_utils_test::ok(
            count_json_lines(
                dftu_utils_test::ok_items(reader.read_json(rc_posix)))
                .get());
        CHECK(posix_count == 200);

        ReadConfig rc_none;
        rc_none.query = R"(cat == "NONEXISTENT")";
        auto none_count =
            dftu_utils_test::ok(count_json_lines(dftu_utils_test::ok_items(
                                                     reader.read_json(rc_none)))
                                    .get());
        CHECK(none_count == 0);
    }

    TEST_CASE("read_json parser fields are accessible") {
        auto test_file = make_unique_test_path("json_parser_fields.pfw");
        {
            dstd::fs::write_file(
                dstd::PathView(test_file),
                common::str_cat(
                    R"({"ph":"X","name":"read","cat":"POSIX","pid":42,"tid":7,"ts":1000,"dur":10,"args":{"ret":1}})",
                    "\n"))
                .discard();
        }

        auto gz_file = gzip_fixture(dstd::String(test_file.str()));

        TraceReader reader({.file_path = dstd::String(gz_file)});
        auto events = dftu_utils_test::ok(
            collect_json_events(dftu_utils_test::ok_items(reader.read_json()))
                .get());
        REQUIRE(events.size() == 1);
        CHECK(events[0].name == "read");
        CHECK(events[0].cat == "POSIX");
        CHECK(events[0].ph == "X");

        dstd::fs::remove(gz_file).discard();
    }
}

namespace {

// Lines written to a gzip trace, read back through read_json with and without
// an index. Returns the line count the reader keeps.
std::size_t read_kept(const dstd::Vector<dstd::String>& lines,
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
    return dftu_utils_test::ok(
        count_json_lines(dftu_utils_test::ok_items(reader.read_json(rc)))
            .get());
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
            CHECK(kept == 1);
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
            CHECK(kept == 2);
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
            CHECK(field_filter == 0);
            const auto expression =
                read_kept(lines, R"(lower(name) == "read")", indexed);
            CHECK(expression == 1);
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
            CHECK(plain >= good_lines);
            CHECK(plain <= good_lines + extra);
            CHECK(indexed == plain);
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
                CHECK(kept == 2);
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
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        ReadConfig rc;
        rc.start_byte = cuts[i];
        rc.end_byte = cuts[i + 1];
        TraceReader reader({.file_path = f.gz});
        REQUIRE(reader.has_index());
        for (auto& l : dftu_utils_test::ok(
                 collect_lines(dftu_utils_test::ok_items(reader.read_lines(rc)))
                     .get()))
            raw.push_back(std::move(l));
        for (auto& l :
             dftu_utils_test::ok(collect_json_text(dftu_utils_test::ok_items(
                                                       reader.read_json(rc)))
                                     .get()))
            json.push_back(std::move(l));
    }
    const bool raw_same = raw == f.lines;
    const bool json_same = json == f.lines;
    CHECK(raw_same);
    CHECK(json_same);
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
        const std::size_t checkpoint = 64 * 1024;
        const WindowFixture f = window_fixture(30, checkpoint);
        const auto indexer = CheckpointIndexerFactory::create(
            f.gz, determine_index_path(f.gz, ""), checkpoint, false);
        REQUIRE(indexer != nullptr);
        std::size_t mid_line_pieces = 0;
        dstd::Vector<std::size_t> cuts;
        for (const auto& member : indexer->members()) {
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

// What read_json gives back for lines that hold a number too large for a
// double or a string with an invalid escape.
struct OddRead {
    dstd::Vector<dstd::String> lines;
    dstd::Vector<dstd::Optional<double>> json_doubles;
    dstd::Vector<dstd::Optional<dstd::String>> json_strings;
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
                    OddRead json;
                    dftu_utils_test::ok(
                        collect_odd_json(dftu_utils_test::ok_items(
                                             reader.read_json(selective)),
                                         json)
                            .get());
                    REQUIRE(json.lines.size() == 1);
                    CHECK(json.lines[0] == odd.line);
                    if (odd.is_string) {
                        REQUIRE(json.json_strings[0].has_value());
                        CHECK(*json.json_strings[0] == odd.expected_string);
                    } else {
                        REQUIRE(json.json_doubles[0].has_value());
                        CHECK(*json.json_doubles[0] == odd.expected_double);
                    }
                    ReadConfig both;
                    both.query = expression
                                     ? R"(x > 5 or lower(name) == "read")"
                                     : R"(x > 5 or name == "read")";
                    OddRead all;
                    dftu_utils_test::ok(
                        collect_odd_json(
                            dftu_utils_test::ok_items(reader.read_json(both)),
                            all)
                            .get());
                    CHECK(all.lines.size() == 2);
                    CHECK(all.lines[1] == lines[1]);
                }
            }
        }
    }

    TEST_CASE("a 20-digit integer above uint64 is read like a longer one") {
        const char* const wide[] = {
            "18446744073709551616", "99999999999999999999",
            "123456789012345678901", "123456789012345678901234567890"};
        for (const char* digits : wide) {
            for (const bool indexed : {false, true}) {
                for (const char* query :
                     {R"(x > 1 or name == "read")",
                      R"(lower(name) == "read" or x > 1)"}) {
                    INFO(dstd::String(digits)
                         << " indexed=" << indexed << " " << query);
                    const dstd::Vector<dstd::String> lines = {
                        common::str_cat(R"({"name":"read","x":)", digits, "}"),
                        R"({"name":"read","x":2})"};
                    const dstd::String gz = write_odd_trace(lines, indexed);
                    TraceReader reader({.file_path = gz});
                    ReadConfig rc;
                    rc.query = query;
                    CHECK(dftu_utils_test::ok(
                              count_json_lines(dftu_utils_test::ok_items(
                                                   reader.read_json(rc)))
                                  .get()) == 2);
                }
            }
        }
    }

    TEST_CASE("bad UTF-8 or a raw control byte in a string keeps the line") {
        struct Bad {
            dstd::String line;
            dstd::String repaired;
        };
        const Bad bads[] = {
            {dstd::String("{\"name\":\"read\",\"x\":\"a\xffz\"}"), "a?z"},
            {dstd::String("{\"name\":\"read\",\"x\":\"a\tz\"}"), "a?z"},
            {dstd::String("{\"name\":\"read\",\"x\":\"a\x01z\"}"), "a?z"},
            {dstd::String("{\"name\":\"read\",\"x\":\"a\xe2\x82\"}"), "a??"},
            {dstd::String("{\"x\":\"\xff\",\"name\":\"read\"}"), "?"},
            {dstd::String(
                 "{\"name\":\"read\",\"args\":{\"x\":\"q\"},\"x\":\"\t\"}"),
             "?"},
        };
        const dstd::String good = R"({"name":"read","x":"ok"})";
        enum class Place { First, Middle, Last };
        for (const Bad& bad : bads) {
            for (const Place place :
                 {Place::First, Place::Middle, Place::Last}) {
                for (const bool indexed : {false, true}) {
                    for (const char* query :
                         {"", R"(x != "zzz")", R"(lower(name) == "read")",
                          R"(lower(x) != "zzz")"}) {
                        INFO("bad=" << (&bad - bads)
                                    << " place=" << static_cast<int>(place)
                                    << " indexed=" << indexed
                                    << " query=" << dstd::String(query));
                        dstd::Vector<dstd::String> lines;
                        if (place == Place::First) lines.push_back(bad.line);
                        lines.push_back(good);
                        if (place == Place::Middle) lines.push_back(bad.line);
                        lines.push_back(good);
                        if (place == Place::Last) lines.push_back(bad.line);
                        const std::size_t at = static_cast<std::size_t>(place);
                        const dstd::String gz = write_odd_trace(lines, indexed);
                        TraceReader reader({.file_path = gz});
                        ReadConfig rc;
                        rc.query = query;
                        OddRead json;
                        dftu_utils_test::ok(
                            collect_odd_json(
                                dftu_utils_test::ok_items(reader.read_json(rc)),
                                json)
                                .get());
                        CAPTURE(json.lines.size());
                        REQUIRE(json.lines.size() == 3);
                        CHECK(json.lines[at] == bad.line);
                        CHECK(json.json_strings[at] == bad.repaired);
                    }
                }
            }
        }
    }

    TEST_CASE("a control byte outside a string or a cropped line is dropped") {
        const dstd::String good = R"({"name":"read","x":"ok"})";
        // Without a query, read_json hands over a line that starts as an
        // object without reading it, so a byte broken inside it is found only
        // by a consumer.
        struct Drop {
            dstd::String line;
            std::size_t plain_json_kept;
            std::size_t plain_json_dropped;
        };
        const Drop drops[] = {
            {dstd::String("{\"name\":\"read\",\x01\"x\":\"a\"}"), 3, 0},
            {dstd::String("{\"name\":\"read\",\"x\":\x01 1}"), 3, 0},
            {dstd::String("{\"name\":\"read\",\xff\"x\":1}"), 2, 1},
            {dstd::String("{\"name\":\"read\",\"x\":\"a\tb"), 2, 1},
            {dstd::String("{\"name\":\"read\",\"x\":\"\xff"), 2, 1},
        };
        for (const Drop& drop : drops) {
            for (const bool indexed : {false, true}) {
                INFO(drop.line << " indexed=" << indexed);
                const dstd::Vector<dstd::String> lines = {good, drop.line,
                                                          good};
                const dstd::String gz = write_odd_trace(lines, indexed);
                ReadConfig rc;
                rc.query = R"(x != "zzz" or name == "read")";
                {
                    TraceReader reader({.file_path = gz});
                    CHECK(dftu_utils_test::ok(
                              count_json_lines(dftu_utils_test::ok_items(
                                                   reader.read_json(rc)))
                                  .get()) == 2);
                    CHECK(reader.dropped_line_count() == 1);
                }
                ReadConfig plain;
                {
                    TraceReader reader({.file_path = gz});
                    CHECK(dftu_utils_test::ok(
                              count_json_lines(dftu_utils_test::ok_items(
                                                   reader.read_json(plain)))
                                  .get()) == drop.plain_json_kept);
                    CHECK(reader.dropped_line_count() ==
                          drop.plain_json_dropped);
                }
            }
        }
    }

    TEST_CASE("the reader counts the lines it could not read") {
        const dstd::String good = R"({"name":"read","x":"ok"})";
        // Lines simdjson refuses at parse time: cropped strings and bytes
        // that no repair can fix.
        const dstd::String broken[] = {
            R"({"name":"read","x":"cropped)", R"({"name":"read","x":"a\)",
            R"("just a quote)", dstd::String("{\"name\":\"read\",\xff\"x\":1}"),
            "garbage \" with a quote"};
        enum class Place { First, Middle, Last, Each };
        for (const dstd::String& bad : broken) {
            for (const Place place :
                 {Place::First, Place::Middle, Place::Last, Place::Each}) {
                for (const bool indexed : {false, true}) {
                    for (const char* query :
                         {"", R"(x != "zzz")", R"(lower(name) == "read")"}) {
                        INFO("place=" << static_cast<int>(place)
                                      << " indexed=" << indexed
                                      << " query=" << dstd::String(query));
                        dstd::Vector<dstd::String> lines;
                        std::size_t bad_count = 0;
                        const auto add_bad = [&] {
                            lines.push_back(bad);
                            ++bad_count;
                        };
                        if (place == Place::First || place == Place::Each)
                            add_bad();
                        lines.push_back(good);
                        if (place == Place::Middle || place == Place::Each)
                            add_bad();
                        lines.push_back("");
                        lines.push_back(good);
                        if (place == Place::Last || place == Place::Each)
                            add_bad();
                        const dstd::String gz = write_odd_trace(lines, indexed);
                        ReadConfig rc;
                        rc.query = query;
                        {
                            TraceReader reader({.file_path = gz});
                            CHECK(reader.dropped_line_count() == 0);
                            CHECK(
                                dftu_utils_test::ok(
                                    count_json_lines(dftu_utils_test::ok_items(
                                                         reader.read_json(rc)))
                                        .get()) == 2);
                            CHECK(reader.dropped_line_count() == bad_count);
                        }
                        {
                            TraceReader reader({.file_path = gz});
                            (void)count_json_lines(
                                dftu_utils_test::ok_items(reader.read_json(rc)))
                                .get();
                            (void)count_json_lines(
                                dftu_utils_test::ok_items(reader.read_json(rc)))
                                .get();
                            CHECK(reader.dropped_line_count() == 2 * bad_count);
                        }
                    }
                }
            }
        }
    }

    TEST_CASE("a clean file, a blank line and a bracket line count nothing") {
        for (const bool indexed : {false, true}) {
            const dstd::Vector<dstd::String> lines = {
                "[",   R"({"name":"read","x":1},)", "",
                "   ", R"({"name":"read","x":2})",  "]"};
            const dstd::String gz = write_odd_trace(lines, indexed);
            TraceReader reader({.file_path = gz});
            ReadConfig rc;
            (void)count_json_lines(
                dftu_utils_test::ok_items(reader.read_json(rc)))
                .get();
            CHECK(reader.dropped_line_count() == 0);
        }
    }

    TEST_CASE("a line the query could not read counts, a mismatch does not") {
        const dstd::Vector<dstd::String> lines = {
            R"({"name":"read","x":01})", R"({"name":"read","x":"ok"})",
            R"({"name":"write","x":"no"})", R"({"name":"read","x":tru})"};
        for (const bool indexed : {false, true}) {
            for (const char* query :
                 {R"(x != "ok" and name == "read")",
                  R"(lower(name) == "read" and x != "ok")"}) {
                INFO("indexed=" << indexed << " " << dstd::String(query));
                const dstd::String gz = write_odd_trace(lines, indexed);
                ReadConfig rc;
                rc.query = query;
                {
                    TraceReader reader({.file_path = gz});
                    CHECK(dftu_utils_test::ok(
                              count_json_lines(dftu_utils_test::ok_items(
                                                   reader.read_json(rc)))
                                  .get()) == 0);
                    CHECK(reader.dropped_line_count() == 2);
                }
            }
        }
    }

    TEST_CASE("read_json hands over every line and counts a cropped object") {
        const dstd::String good = R"({"name":"read","x":"ok"})";
        struct Bad {
            dstd::String line;
            std::size_t json_dropped;
        };
        const Bad bads[] = {
            {"tru", 0}, {"5", 0}, {R"([1,2])", 0}, {R"({"a":1,"b":)", 1}};
        for (const Bad& bad : bads) {
            for (const bool indexed : {false, true}) {
                INFO(bad.line << " indexed=" << indexed);
                const dstd::Vector<dstd::String> lines = {good, bad.line, good};
                const dstd::String gz = write_odd_trace(lines, indexed);
                ReadConfig rc;
                {
                    TraceReader reader({.file_path = gz});
                    CHECK(dftu_utils_test::ok(
                              count_json_lines(dftu_utils_test::ok_items(
                                                   reader.read_json(rc)))
                                  .get()) == 3);
                    CHECK(reader.dropped_line_count() == bad.json_dropped);
                }
            }
        }
    }

    TEST_CASE("a field read of a cropped or non-object line finds nothing") {
        const dstd::Vector<dstd::String> lines = {R"({"name":"a"})",
                                                  R"({"name":"b","x":)", "5",
                                                  "tru", R"({"name":"c"})"};
        for (const bool indexed : {false, true}) {
            CAPTURE(indexed);
            const dstd::String gz = write_odd_trace(lines, indexed);
            TraceReader reader({.file_path = gz});
            const auto events = dftu_utils_test::ok(
                collect_json_events(
                    dftu_utils_test::ok_items(reader.read_json()))
                    .get());
            REQUIRE(events.size() == lines.size());
            CHECK(events[0].name == "a");
            CHECK(events[2].name.empty());
            CHECK(events[3].name.empty());
            CHECK(events[4].name == "c");
            CHECK(reader.dropped_line_count() == 1);
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
                    CHECK(dftu_utils_test::ok(
                              count_json_lines(dftu_utils_test::ok_items(
                                                   reader.read_json(rc)))
                                  .get()) == expected);
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
                const std::size_t bad_kept = dftu_utils_test::ok(
                    count_json_lines(
                        dftu_utils_test::ok_items(reader.read_json(rc)))
                        .get());
                CHECK(bad_kept <= 1);
            }
        }
    }
}

namespace {

constexpr std::size_t LONG_LINE_BYTES = 9 * 1024 * 1024;

WindowFixture long_line_fixture(bool indexed) {
    WindowFixture f;
    dstd::String content;
    static const char HEX[] = "0123456789abcdef";
    std::mt19937_64 rng(11);
    const std::size_t sizes[] = {40, 400, LONG_LINE_BYTES, 40, 4000};
    for (const std::size_t size : sizes) {
        dstd::String line = common::str_cat(R"({"name":"read","ph":"X","id":)",
                                            f.lines.size(), R"(,"pad":")");
        for (std::size_t i = 0; i < size; ++i) line.push_back(HEX[rng() & 15]);
        line += R"("})";
        f.starts.push_back(content.size());
        content += line;
        content += "\n";
        f.lines.push_back(std::move(line));
    }
    f.total = content.size();
    const dstd::Path dir = make_unique_test_path("long_line");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw((dir / "trace.pfw").str());
    REQUIRE(dstd::fs::write_file(dstd::PathView(pfw), content).has_value());
    f.gz = gzip_fixture(pfw);
    if (indexed) REQUIRE(dftu_utils_test::build_index(f.gz, "", 1024 * 1024));
    return f;
}

static CoroTask<dstd::String> collect_raw_text(
    AsyncGenerator<dstd::Span<const char>> gen) {
    dstd::String out;
    while (auto chunk = co_await gen.next())
        out.append(chunk->data(), chunk->size());
    co_return out;
}

}  // namespace

TEST_SUITE("TraceReader long lines") {
    TEST_CASE("a line longer than the read buffer is kept whole") {
        for (const bool indexed : {false, true}) {
            INFO("indexed=" << indexed);
            const WindowFixture f = long_line_fixture(indexed);
            dstd::String expected;
            for (const auto& l : f.lines) expected += indexed ? l + "\n" : l;
            TraceReader reader({.file_path = f.gz});
            REQUIRE(reader.has_index() == indexed);
            const bool lines_same =
                dftu_utils_test::ok(
                    collect_lines(dftu_utils_test::ok_items(
                                      reader.read_lines(ReadConfig{})))
                        .get()) == f.lines;
            CHECK(lines_same);
            const bool json_same =
                dftu_utils_test::ok(
                    collect_json_text(dftu_utils_test::ok_items(
                                          reader.read_json(ReadConfig{})))
                        .get()) == f.lines;
            CHECK(json_same);
            const bool raw_same =
                dftu_utils_test::ok(
                    collect_raw_text(dftu_utils_test::ok_items(
                                         reader.read_raw(ReadConfig{})))
                        .get()) == expected;
            CHECK(raw_same);
            CHECK(reader.dropped_line_count() == 0);
        }
    }

    TEST_CASE("byte windows cut inside a long line give each line once") {
        const WindowFixture f = long_line_fixture(true);
        const std::size_t long_start = f.starts[2];
        for (const std::size_t at :
             {std::size_t{0}, std::size_t{1}, std::size_t{4 * 1024 * 1024},
              std::size_t{LONG_LINE_BYTES / 2}, f.lines[2].size()}) {
            CAPTURE(at);
            check_windows(f, {long_start + at});
        }
    }
}
