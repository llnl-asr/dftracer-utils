#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <dftracer/utils/utilities/trace_gen/fake_trace_writer.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::reader;
using namespace dftracer::utils::utilities::trace_gen;
using namespace dftracer::utils::coro;
using namespace dft_utils_test;

namespace {

CoroTask<std::vector<std::string>> collect_lines(
    AsyncGenerator<dftracer::utils::utilities::fileio::lines::Line> gen) {
    std::vector<std::string> lines;
    while (auto line = co_await gen.next()) {
        lines.emplace_back(line->content);
    }
    co_return lines;
}

}  // namespace

TEST_SUITE("trace_gen::TraceWriter") {
    TEST_CASE("writes a gzip trace that TraceReader can read back") {
        TestEnvironment env(0);
        const std::string gz_path = env.get_dir() + "/fake.pfw.gz";

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

        REQUIRE(fs::exists(gz_path));
        CHECK(fs::file_size(gz_path) > 0);

        TraceReader reader({.file_path = gz_path});
        CHECK_FALSE(reader.has_index());

        auto lines = collect_lines(reader.read_lines()).get();

        std::size_t event_lines = 0;
        bool saw_metadata = false;
        for (const auto& l : lines) {
            if (l.find(R"("ph":"X")") != std::string::npos) ++event_lines;
            if (l.find(R"("ph":"M")") != std::string::npos) saw_metadata = true;
        }
        CHECK(event_lines == NUM_EVENTS);
        CHECK(saw_metadata);
    }
}
