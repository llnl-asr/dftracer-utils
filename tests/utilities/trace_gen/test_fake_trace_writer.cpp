// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/domains/workload/trace_gen/fake_trace_writer.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <generator_ok.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::reader;
using namespace dftracer::utils::domains::workload::trace_gen;
using namespace dftracer::utils::coro;
using namespace dftu_utils_test;

namespace {

CoroTask<dstd::Vector<dstd::String>> collect_lines(
    AsyncGenerator<dftracer::utils::fileio::lines::Line> gen) {
    dstd::Vector<dstd::String> lines;
    while (auto line = co_await gen.next()) {
        lines.emplace_back(line->content);
    }
    co_return lines;
}

}  // namespace

TEST_SUITE("trace_gen::TraceWriter") {
    TEST_CASE("writes a gzip trace that TraceReader can read back") {
        TestEnvironment env(0);
        const dstd::String gz_path = env.dir() + "/fake.pfw.gz";

        constexpr int NUM_EVENTS = 8;
        {
            TraceWriter writer(gz_path);
            writer.write("[\n");
            emit_metadata(writer, "HH", "hh0", "node-0", "hh0");
            for (int i = 0; i < NUM_EVENTS; ++i) {
                EventArgs a;
                a.id = static_cast<std::uint64_t>(i);
                a.pid = 1000;
                a.tid = 10001;
                a.name = "read";
                a.cat = "POSIX";
                a.ts = 1000000000ULL + static_cast<std::uint64_t>(i) * 100;
                a.dur = 42;
                a.level = 2;
                a.hhash = "hh0";
                emit_event(writer, a);
            }
            writer.write("]\n");
        }  // writer flushes and closes on scope exit

        REQUIRE(dstd::fs::exists(gz_path).value());
        CHECK(dstd::fs::file_size(gz_path).value() > 0);

        TraceReader reader({.file_path = dstd::String(gz_path)});
        CHECK_FALSE(reader.has_index());

        auto lines = dftu_utils_test::ok(
            collect_lines(dftu_utils_test::ok_items(reader.read_lines()))
                .get());

        std::size_t event_lines = 0;
        bool saw_metadata = false;
        for (const auto& l : lines) {
            if (l.find(R"("ph":1)") != dstd::String::npos) ++event_lines;
            if (l.find(R"("ph":4)") != dstd::String::npos) saw_metadata = true;
        }
        CHECK(event_lines == NUM_EVENTS);
        CHECK(saw_metadata);
    }

    TEST_CASE("close reports a file that cannot be created") {
        TestEnvironment env(0);
        TraceWriter writer(env.dir() + "/missing_dir/x.pfw.gz");
        writer.write("[\n");
        CHECK_FALSE(writer.flush().has_value());
        CHECK_FALSE(writer.close().has_value());
    }

    TEST_CASE("escapes control characters in string fields") {
        TestEnvironment env(0);
        const dstd::String gz_path = env.dir() + "/escaped.pfw.gz";
        {
            TraceWriter writer(gz_path);
            EventArgs a;
            a.id = 1;
            a.name =
                "a\nb\x01"
                "c\td";
            a.cat = "POSIX";
            a.hhash = "hh0";
            emit_event(writer, a);
        }

        TraceReader reader({.file_path = dstd::String(gz_path)});
        auto lines = dftu_utils_test::ok(
            collect_lines(dftu_utils_test::ok_items(reader.read_lines()))
                .get());

        REQUIRE(lines.size() == 1);
        CHECK(lines[0].find(R"("name":"a\nb\u0001c\td")") !=
              dstd::String::npos);
    }
}
