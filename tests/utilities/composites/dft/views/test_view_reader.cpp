#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::internal;
using namespace dftracer::utils::utilities::composites::dft::views;
using namespace dft_utils_test;
using dftracer::utils::utilities::common::query::Query;

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
    ViewReaderUtility* reader, ViewReaderInput input) {
    CollectedViewOutput output;
    auto gen = reader->process(input);
    while (auto batch = co_await gen.next()) {
        output.events_matched += batch->events_matched;
        output.events_scanned += batch->events_scanned;
        for (const auto& ev : batch->events) output.events.emplace_back(ev);
    }
    co_return output;
}

static CollectedViewOutput collect_view_output(ViewReaderUtility& reader,
                                               ViewReaderInput input) {
    return collect_view_coro(&reader, std::move(input)).get();
}

TEST_SUITE("ViewReader") {
    TEST_CASE("ViewReader - No query matches all events") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_pfw_gz(env, 50);
        std::string db_root = determine_index_path(gz, "");

        ViewReaderInput input;
        input.with_file_path(gz)
            .with_index_path(db_root)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());
        input.view.with_include_metadata(false);

        ViewReaderUtility reader;
        auto output = collect_view_output(reader, input);

        CHECK(output.events_scanned > 0);
        CHECK(output.events_matched > 0);
        CHECK(output.events_matched == output.events_scanned);
    }

    TEST_CASE("ViewReader - Query filters events") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_pfw_gz(env, 50);
        std::string db_root = determine_index_path(gz, "");

        ViewReaderInput input;
        input.with_file_path(gz)
            .with_index_path(db_root)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());
        input.view.with_include_metadata(false);

        auto q = Query::from_string(R"(cat == "POSIX")");
        REQUIRE(q.has_value());
        input.query = std::move(*q);

        ViewReaderUtility reader;
        auto output = collect_view_output(reader, input);

        CHECK(output.events_scanned > 0);
        CHECK(output.events_matched > 0);
    }

    TEST_CASE("ViewReader - Non-matching query returns empty") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_pfw_gz(env, 50);
        std::string db_root = determine_index_path(gz, "");

        ViewReaderInput input;
        input.with_file_path(gz)
            .with_index_path(db_root)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());
        input.view.with_include_metadata(false);

        auto q = Query::from_string(R"(cat == "NONEXISTENT")");
        REQUIRE(q.has_value());
        input.query = std::move(*q);

        ViewReaderUtility reader;
        auto output = collect_view_output(reader, input);

        CHECK(output.events_matched == 0);
    }

    TEST_CASE(
        "ViewReader - re-emits SH/FH referenced by exec_hash/cmd_hash/cwd") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_metadata_pfw_gz(env);
        std::string db_root = determine_index_path(gz, "");

        ViewReaderInput input;
        input.with_file_path(gz)
            .with_index_path(db_root)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max());
        input.view.with_include_metadata(true);

        auto q = Query::from_string(R"(name == "start")");
        REQUIRE(q.has_value());
        input.query = std::move(*q);

        ViewReaderUtility reader;
        auto output = collect_view_output(reader, input);

        auto has = [&](const std::string& needle) {
            for (const auto& e : output.events)
                if (e.find(needle) != std::string::npos) return true;
            return false;
        };
        // The start event points at SH via exec_hash/cmd_hash and FH via cwd;
        // all three must be flushed despite the non-standard field names.
        CHECK(has(R"("value":"EX01")"));
        CHECK(has(R"("value":"CM01")"));
        CHECK(has(R"("value":"CW01")"));
        // Metadata nothing references is still pruned.
        CHECK_FALSE(has(R"("value":"UNUSED01")"));
        CHECK(has(R"("name":"start")"));
    }
}
