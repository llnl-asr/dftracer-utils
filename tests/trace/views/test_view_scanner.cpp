#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using dftracer::utils::duql::Query;

static std::string create_pfw_gz(TestEnvironment& env, int n) {
    std::string pfw = env.get_dir() + "/trace.pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < n; ++i) {
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i) << R"(,"args":{}})"
            << "\n";
    }
    ofs.close();
    std::string gz = pfw + ".gz";
    compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

// A trace with hash metadata plus a dftracer "start" event that references the
// SH/FH entries through non-standard field names (exec_hash/cmd_hash/cwd), and
// one SH nothing references.
static std::string create_metadata_pfw_gz(TestEnvironment& env) {
    std::string pfw = env.get_dir() + "/meta.pfw";
    std::ofstream ofs(pfw);
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
    std::string gz = pfw + ".gz";
    compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

struct CollectedViewOutput {
    std::vector<std::string> events;
    std::uint64_t events_matched = 0;
    std::uint64_t events_scanned = 0;
};

static coro::CoroTask<CollectedViewOutput> collect_view_coro(
    ViewScannerUtility* reader, ViewScannerInput input) {
    CollectedViewOutput output;
    auto gen = (*reader)(input);
    while (auto batch = co_await gen.next()) {
        output.events_matched += batch->events_matched;
        output.events_scanned += batch->events_scanned;
        for (const auto& ev : batch->events) output.events.emplace_back(ev);
    }
    co_return output;
}

static CollectedViewOutput collect_view_output(ViewScannerUtility& reader,
                                               ViewScannerInput input) {
    return collect_view_coro(&reader, std::move(input)).get();
}

// A two-member trace whose first member ends inside a line: the newline that
// ends it opens the second member, and the file's last line has none.
static std::string write_newline_split_trace(TestEnvironment& env,
                                             std::size_t& first_len) {
    auto line = [](int i) {
        return R"({"name":"read","cat":"POSIX","ph":"X","pid":1,"tid":1,"ts":)" +
               std::to_string(1000 + i) + R"(,"dur":1,"args":{}})";
    };
    std::string first, second;
    for (int i = 0; i < 10; ++i) first += (i ? "\n" : "") + line(i);
    for (int i = 10; i < 20; ++i) second += "\n" + line(i);
    first_len = first.size();
    const std::string gz = env.get_dir() + "/split_line.pfw.gz";
    std::ofstream out(gz, std::ios::binary);
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
        const std::string gz = write_newline_split_trace(env, first_len);
        auto scan = [&](std::size_t begin, std::size_t end) {
            ViewScannerInput input;
            input.with_file_path(gz)
                .with_index_path(determine_index_path(gz, ""))
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
        std::vector<std::string> ts;
        for (const auto& e : events) {
            const auto at = e.find("\"ts\":");
            REQUIRE(at != std::string::npos);
            ts.push_back(e.substr(at + 5, 4));
        }
        std::sort(ts.begin(), ts.end());
        std::vector<std::string> want;
        for (int i = 0; i < 20; ++i) want.push_back(std::to_string(1000 + i));
        CHECK(ts == want);
    }

    TEST_CASE("ViewScanner - No query matches all events") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_pfw_gz(env, 50);
        std::string db_root = determine_index_path(gz, "");

        ViewScannerInput input;
        input.with_file_path(gz)
            .with_index_path(db_root)
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
        std::string gz = create_pfw_gz(env, 50);
        std::string db_root = determine_index_path(gz, "");

        ViewScannerInput input;
        input.with_file_path(gz)
            .with_index_path(db_root)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());

        auto q = Query::from_string(R"(cat == "POSIX")");
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
        std::string gz = create_pfw_gz(env, 50);
        std::string db_root = determine_index_path(gz, "");

        ViewScannerInput input;
        input.with_file_path(gz)
            .with_index_path(db_root)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());

        auto q = Query::from_string(R"(cat == "NONEXISTENT")");
        REQUIRE(q.has_value());
        input.query = std::move(*q);

        ViewScannerUtility reader;
        auto output = collect_view_output(reader, input);

        CHECK(output.events_matched == 0);
    }

    TEST_CASE(
        "ViewScanner - include_metadata returns every metadata record past "
        "the query") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_metadata_pfw_gz(env);
        std::string db_root = determine_index_path(gz, "");

        ViewScannerInput input;
        input.with_file_path(gz)
            .with_index_path(db_root)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());
        input.view.include_metadata = true;

        auto q = Query::from_string(R"(name == "start")");
        REQUIRE(q.has_value());
        input.query = std::move(*q);

        ViewScannerUtility reader;
        auto output = collect_view_output(reader, input);

        auto has = [&](const std::string& needle) {
            for (const auto& e : output.events)
                if (e.find(needle) != std::string::npos) return true;
            return false;
        };
        CHECK(has(R"("value":"EX01")"));
        CHECK(has(R"("value":"CM01")"));
        CHECK(has(R"("value":"CW01")"));
        CHECK(has(R"("value":"UNUSED01")"));
        CHECK(has(R"("name":"start")"));
    }
}
