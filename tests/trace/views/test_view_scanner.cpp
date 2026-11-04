// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;

static dstd::String create_pfw_gz(TestEnvironment& env, int n) {
    dstd::String pfw = env.get_dir() + "/trace.pfw";
    dftu_utils_test::FileOut ofs(pfw);
    for (int i = 0; i < n; ++i) {
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i) << R"(,"args":{}})"
            << "\n";
    }
    ofs.close();
    dstd::String gz = pfw + ".gz";
    compress_file_to_gzip(pfw, gz);
    (void)dstd::fs::remove(pfw);
    return gz;
}

// A trace with hash metadata plus a dftracer "start" event that references the
// SH/FH entries through non-standard field names (exec_hash/cmd_hash/cwd), and
// one SH nothing references.
static dstd::String create_metadata_pfw_gz(TestEnvironment& env) {
    dstd::String pfw = env.get_dir() + "/meta.pfw";
    dftu_utils_test::FileOut ofs(pfw);
    ofs << R"({"name":"HH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"hhash":"H1","name":"host1","value":"H1"}})"
        << "\n"
        << R"({"name":"SH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"hhash":"H1","name":"myapp","value":"EX01"}})"
        << "\n"
        << R"({"name":"SH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"hhash":"H1","name":"mycmd","value":"CM01"}})"
        << "\n"
        << R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"hhash":"H1","name":"/my/cwd","value":"CW01"}})"
        << "\n"
        << R"({"name":"SH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"hhash":"H1","name":"unused","value":"UNUSED01"}})"
        << "\n"
        << R"({"name":"start","cat":"dftracer","pid":1,"tid":1,"ts":1000,"dur":0,"ph":"X","args":{"hhash":"H1","exec_hash":"EX01","cmd_hash":"CM01","cwd":"CW01","ppid":9}})"
        << "\n"
        << R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1100,"dur":10,"args":{"hhash":"H1"}})"
        << "\n";
    ofs.close();
    dstd::String gz = pfw + ".gz";
    compress_file_to_gzip(pfw, gz);
    (void)dstd::fs::remove(pfw);
    return gz;
}

struct CollectedViewOutput {
    dstd::Vector<dstd::String> events;
    dstd::Vector<dstd::String> fold_names;
    std::uint64_t events_matched = 0;
    std::uint64_t events_scanned = 0;
    std::uint64_t lines_invalid = 0;
};

static coro::CoroTask<CollectedViewOutput> collect_view_coro(
    ViewScannerUtility* reader, ViewScannerInput input) {
    CollectedViewOutput output;
    auto gen = (*reader)(input);
    while (auto batch = co_await gen.next()) {
        output.events_matched += batch->events_matched;
        output.events_scanned += batch->events_scanned;
        output.lines_invalid += batch->lines_invalid;
        for (const auto& ev : batch->events) output.events.emplace_back(ev);
        for (const auto& ev : batch->fold_events)
            output.fold_names.emplace_back(
                input.fold_intern->resolve(ev.name_id));
    }
    co_return output;
}

static CollectedViewOutput collect_view_output(ViewScannerUtility& reader,
                                               ViewScannerInput input) {
    return collect_view_coro(&reader, std::move(input)).get();
}

static dstd::String create_refused_gz(TestEnvironment& env) {
    dstd::String pfw = env.get_dir() + "/refused.pfw";
    {
        dftu_utils_test::FileOut ofs(pfw);
        ofs << R"({"ph":"X","name":"a","cat":"P","pid":1,"tid":1,"ts":100,"dur":5,"args":{"x":1e400}})"
            << "\n"
            << R"({"ph":"X","name":"b\q","cat":"P","pid":1,"tid":1,"ts":110,"dur":5})"
            << "\n"
            << dstd::String(
                   "{\"ph\":\"X\",\"name\":\"c\xff"
                   "d\",\"cat\":\"P\",\"pid\":1,\"tid\":1,\"ts\":120,"
                   "\"dur\":5}")
            << "\n"
            << R"({"ph":"X","name":"d","cat":"P","pid":1,"tid":1,"ts":130,"dur":5,"args":{"n":123456789012345678901234567890}})"
            << "\n"
            << R"({"ph":"X","name":"e","cat":"P","pid":1,"tid":1,"ts":140,"dur":5})"
            << "\n";
    }
    dstd::String gz = pfw + ".gz";
    compress_file_to_gzip(pfw, gz);
    (void)dstd::fs::remove(pfw);
    return gz;
}

// A two-member trace whose first member ends inside a line: the newline that
// ends it opens the second member, and the file's last line has none.
static dstd::String write_newline_split_trace(TestEnvironment& env,
                                              std::size_t& first_len) {
    auto line = [](int i) {
        return R"({"name":"read","cat":"POSIX","ph":"X","pid":1,"tid":1,"ts":)" +
               dstd::to_string(1000 + i) + R"(,"dur":1,"args":{}})";
    };
    dstd::String first, second;
    for (int i = 0; i < 10; ++i) first += (i ? "\n" : "") + line(i);
    for (int i = 10; i < 20; ++i) second += "\n" + line(i);
    first_len = first.size();
    const dstd::String gz = env.get_dir() + "/split_line.pfw.gz";
    dftu_utils_test::FileOut out(gz);
    for (const auto* text : {&first, &second}) {
        dftracer::utils::utilities::fileio::compress::GzipMemberCompressor comp;
        auto member = comp.compress_member(text->data(), text->size());
        REQUIRE(member.has_value());
        out.write(reinterpret_cast<const char*>(member->data()),
                  static_cast<std::streamsize>(member->size()));
    }
    return gz;
}

TEST_SUITE("ViewScanner") {
    TEST_CASE("ViewScanner - a line ending at a member end is read once") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::size_t first_len = 0;
        const dstd::String gz = write_newline_split_trace(env, first_len);
        auto scan = [&](std::size_t begin, std::size_t end) {
            ViewScannerInput input;
            input.with_file_path(dstd::String(gz))
                .with_index_path(
                    dstd::String(dstd::String(determine_index_path(gz, ""))))
                .with_checkpoint_size(1024)
                .with_batch_size(128)
                .with_byte_range(begin, end);
            ViewScannerUtility reader;
            return collect_view_output(reader, input).events;
        };
        auto events = scan(0, first_len);
        const auto rest =
            scan(first_len, std::numeric_limits<std::size_t>::max());
        events.insert(events.end(), rest.begin(), rest.end());
        dstd::Vector<dstd::String> ts;
        for (const auto& e : events) {
            const auto at = e.find("\"ts\":");
            REQUIRE(at != dstd::String::npos);
            ts.push_back(e.substr(at + 5, 4));
        }
        std::sort(ts.begin(), ts.end());
        dstd::Vector<dstd::String> want;
        for (int i = 0; i < 20; ++i) want.push_back(dstd::to_string(1000 + i));
        CHECK(ts == want);
    }

    TEST_CASE("ViewScanner - a line longer than the read buffer is kept") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String pfw = env.get_dir() + "/longline.pfw";
        dstd::Vector<dstd::String> lines;
        for (int i = 0; i < 5; ++i) {
            dstd::String line =
                R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)" +
                dstd::to_string(1000 + i) + R"(,"dur":1,"args":{"pad":")";
            if (i == 2) {
                for (std::size_t k = 0; k < 9 * 1024 * 1024; ++k)
                    line.push_back(
                        static_cast<char>('a' + (k * 7 + k / 13) % 26));
            }
            line += R"("}})";
            lines.push_back(std::move(line));
        }
        {
            dftu_utils_test::FileOut ofs(pfw);
            for (const auto& l : lines) ofs << l << "\n";
        }
        dstd::String gz = pfw + ".gz";
        compress_file_to_gzip(pfw, gz);
        (void)dstd::fs::remove(pfw);

        ViewScannerInput input;
        input.with_file_path(dstd::String(gz))
            .with_index_path(dstd::String(determine_index_path(gz, "")))
            .with_checkpoint_size(1024 * 1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());
        ViewScannerUtility reader;
        const auto output = collect_view_output(reader, input);
        REQUIRE(output.events.size() == lines.size());
        bool same = true;
        for (std::size_t i = 0; i < lines.size(); ++i)
            same = same && output.events[i] == lines[i];
        CHECK(same);
    }

    TEST_CASE("ViewScanner - No query matches all events") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_pfw_gz(env, 50);
        dstd::String db_root = dstd::String(determine_index_path(gz, ""));

        ViewScannerInput input;
        input.with_file_path(dstd::String(gz))
            .with_index_path(dstd::String(db_root))
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());

        ViewScannerUtility reader;
        auto output = collect_view_output(reader, input);

        CHECK(output.events_scanned > 0);
        CHECK(output.events_matched > 0);
        CHECK(output.events_matched == output.events_scanned);
    }

    TEST_CASE("ViewScanner - Query filters events") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_pfw_gz(env, 50);
        dstd::String db_root = dstd::String(determine_index_path(gz, ""));

        ViewScannerInput input;
        input.with_file_path(dstd::String(gz))
            .with_index_path(dstd::String(db_root))
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());

        auto q = duql::Query::from_string(R"(cat == "POSIX")");
        REQUIRE(q.has_value());
        input.query = std::move(*q);

        ViewScannerUtility reader;
        auto output = collect_view_output(reader, input);

        CHECK(output.events_scanned > 0);
        CHECK(output.events_matched > 0);
    }

    TEST_CASE("ViewScanner - Non-matching query returns empty") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_pfw_gz(env, 50);
        dstd::String db_root = dstd::String(determine_index_path(gz, ""));

        ViewScannerInput input;
        input.with_file_path(dstd::String(gz))
            .with_index_path(dstd::String(db_root))
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());

        auto q = duql::Query::from_string(R"(cat == "NONEXISTENT")");
        REQUIRE(q.has_value());
        input.query = std::move(*q);

        ViewScannerUtility reader;
        auto output = collect_view_output(reader, input);

        CHECK(output.events_matched == 0);
    }

    TEST_CASE(
        "ViewScanner - a metadata record with a bad byte is still dropped") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String pfw = env.get_dir() + "/badmeta.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":1,"args":{}})"
                << "\n"
                << dstd::String(
                       "{\"name\":\"FH\",\"ph\":\"M\",\"args\":{"
                       "\"name\":\"/a\xffz\",\"value\":\"fh1\"}}")
                << "\n"
                << dstd::String(
                       "{\"name\":\"HH\",\"ph\":\"M\",\"args\":{"
                       "\"name\":\"n\tm\",\"value\":\"hh1\"}}")
                << "\n"
                << R"({"ph":"X","name":"write","cat":"POSIX","pid":1,"tid":1,"ts":1100,"dur":1,"args":{}})"
                << "\n";
        }
        dstd::String gz = pfw + ".gz";
        compress_file_to_gzip(pfw, gz);
        (void)dstd::fs::remove(pfw);

        ViewScannerInput input;
        input.with_file_path(dstd::String(gz))
            .with_index_path(dstd::String(determine_index_path(gz, "")))
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());
        ViewScannerUtility reader;
        const auto output = collect_view_output(reader, input);
        REQUIRE(output.events.size() == 2);
        for (const auto& e : output.events)
            CHECK(e.find(R"("ph":"M")") == dstd::String::npos);
    }

    TEST_CASE(
        "ViewScanner - include_metadata returns every metadata record past "
        "the query") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_metadata_pfw_gz(env);
        dstd::String db_root = dstd::String(determine_index_path(gz, ""));

        ViewScannerInput input;
        input.with_file_path(dstd::String(gz))
            .with_index_path(dstd::String(db_root))
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());
        input.view.include_metadata = true;

        auto q = duql::Query::from_string(R"(name == "start")");
        REQUIRE(q.has_value());
        input.query = std::move(*q);

        ViewScannerUtility reader;
        auto output = collect_view_output(reader, input);

        auto has = [&](const dstd::String& needle) {
            for (const auto& e : output.events)
                if (e.find(needle) != dstd::String::npos) return true;
            return false;
        };
        CHECK(has(R"("value":"EX01")"));
        CHECK(has(R"("value":"CM01")"));
        CHECK(has(R"("value":"CW01")"));
        CHECK(has(R"("value":"UNUSED01")"));
        CHECK(has(R"("name":"start")"));
    }

    TEST_CASE(
        "ViewScanner - a query keeps lines with a huge number, a bad escape "
        "or bad bytes") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        const dstd::String gz = create_refused_gz(env);
        ViewScannerInput input;
        input.with_file_path(dstd::String(gz))
            .with_index_path(dstd::String(determine_index_path(gz, "")))
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());
        auto q = duql::Query::from_string("pid == 1");
        REQUIRE(q.has_value());
        input.query = std::move(*q);
        ViewScannerUtility reader;
        const auto output = collect_view_output(reader, input);
        CHECK(output.events.size() == 5);
        CHECK(output.events_matched == 5);
        CHECK(output.lines_invalid == 0);
    }

    TEST_CASE(
        "ViewScanner - fold mode keeps lines with a huge number, a bad "
        "escape or bad bytes") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        const dstd::String gz = create_refused_gz(env);
        dftracer::utils::dstd::StringIntern intern;
        detail::FoldEventBuffer buffer;
        ViewScannerInput input;
        input.with_file_path(dstd::String(gz))
            .with_index_path(dstd::String(determine_index_path(gz, "")))
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());
        input.fold_intern = &intern;
        input.fold_buffer = &buffer;
        input.fold_needs_args = true;
        ViewScannerUtility reader;
        auto output = collect_view_output(reader, input);
        CHECK(output.lines_invalid == 0);
        std::sort(output.fold_names.begin(), output.fold_names.end());
        CHECK(output.fold_names ==
              dstd::Vector<dstd::String>{"a", R"(b\q)", "c?d", "d", "e"});
    }
}
