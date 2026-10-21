#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/runtime/coro/when_all.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/chunk_extractor_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::trace;
using namespace dftu_utils_test;

namespace {

coro::CoroTask<ChunkExtractorUtilityOutput> admitted_extract(
    ExtractionAdmission* admission, ChunkExtractorUtilityInput input) {
    co_await admission->enter();
    ChunkExtractorUtility extractor;
    auto output = co_await extractor(input);
    admission->leave();
    co_return output;
}

struct AdmittedRun {
    std::vector<ChunkExtractorUtilityOutput> outputs;
    std::size_t peak_in_flight = 0;
    std::uint64_t peak_reserved = 0;
};

AdmittedRun run_admitted(const std::string& file, const std::string& out_dir,
                         std::size_t groups, std::uint64_t total_bytes,
                         std::uint64_t reserve_bytes) {
    ExtractionAdmission admission(total_bytes, reserve_bytes, groups);
    AdmittedRun run;
    Runtime rt(4);
    auto body = [&](CoroScope&) -> coro::CoroTask<void> {
        std::vector<coro::CoroTask<ChunkExtractorUtilityOutput>> tasks;
        for (std::size_t i = 0; i < groups; ++i) {
            tasks.push_back(admitted_extract(
                &admission, ChunkExtractorUtilityInput::from_files(
                                static_cast<int>(i + 1), {file})
                                .with_output_dir(out_dir)
                                .with_app_name("bounded")
                                .with_compression(true)
                                .with_member_size(64 * 1024)
                                .with_chunk_size(128 * 1024)
                                .with_memory_budget(reserve_bytes)));
        }
        run.outputs = co_await coro::when_all(std::move(tasks));
    };
    rt.submit(run_coro_scope(rt.executor(), body), "AdmittedExtract").wait();
    rt.shutdown();
    run.peak_in_flight = admission.peak_in_flight();
    run.peak_reserved = admission.peak_reserved_bytes();
    return run;
}

}  // namespace

TEST_SUITE("ChunkExtractor") {
    TEST_CASE("ChunkExtractor - Basic extraction") {
        TestEnvironment env(100);

        std::string test_file = env.create_dft_test_gzip_file(5);

        auto input = ChunkExtractorUtilityInput::from_files(0, {test_file})
                         .with_output_dir(env.get_dir())
                         .with_app_name("test");

        ChunkExtractorUtility extractor;
        auto output = extractor(input).get();

        CHECK(output.chunk_index == 0);
        CHECK(output.success == true);
        CHECK(output.events > 0);
        CHECK(output.chunks.size() == 1);
    }

    TEST_CASE("ChunkExtractor - Rolls over at the chunk size") {
        TestEnvironment env(100);

        std::string test_file = env.create_dft_test_gzip_file(20000);

        // A chunk ends at the first member end past its size, so small
        // members give several chunks.
        auto input = ChunkExtractorUtilityInput::from_files(1, {test_file})
                         .with_output_dir(env.get_dir())
                         .with_app_name("test")
                         .with_compression(true)
                         .with_member_size(64 * 1024)
                         .with_chunk_size(128 * 1024);

        ChunkExtractorUtility extractor;
        auto output = extractor(input).get();

        REQUIRE(output.success);
        CHECK(output.chunks.size() >= 2);
        std::size_t chunk_events = 0;
        for (const auto& chunk : output.chunks) {
            CHECK(fs::exists(chunk.path));
            chunk_events += chunk.events_written;
        }
        CHECK(chunk_events == output.events);
    }

    TEST_CASE("ChunkExtractor - Reads files in order into one writer") {
        TestEnvironment env(100);

        std::string first = env.create_dft_test_gzip_file(30);
        std::string second = env.create_dft_test_gzip_file(40);

        ChunkExtractorUtility extractor;
        auto one = extractor(ChunkExtractorUtilityInput::from_files(0, {first})
                                 .with_output_dir(env.get_dir())
                                 .with_app_name("one"))
                       .get();
        auto two = extractor(ChunkExtractorUtilityInput::from_files(0, {second})
                                 .with_output_dir(env.get_dir())
                                 .with_app_name("two"))
                       .get();
        auto both =
            extractor(ChunkExtractorUtilityInput::from_files(0, {first, second})
                          .with_output_dir(env.get_dir())
                          .with_app_name("both"))
                .get();

        REQUIRE(one.success);
        REQUIRE(two.success);
        REQUIRE(both.success);
        CHECK(both.events == one.events + two.events);
    }

    TEST_CASE("ChunkExtractor - A missing file fails the group") {
        TestEnvironment env(100);

        auto input =
            ChunkExtractorUtilityInput::from_files(0, {env.get_dir() + "/no"})
                .with_output_dir(env.get_dir())
                .with_app_name("test");

        ChunkExtractorUtility extractor;
        auto output = extractor(input).get();

        CHECK(output.success == false);
        CHECK(output.chunks.empty());
    }

    TEST_CASE("ChunkExtractor - A file that is not gzip fails the group") {
        TestEnvironment env(100);

        const std::string plain = env.get_dir() + "/plain.pfw.gz";
        {
            std::ofstream out(plain);
            out << "{\"name\":\"read\",\"cat\":\"POSIX\",\"ph\":\"X\"}\n";
        }

        auto input = ChunkExtractorUtilityInput::from_files(0, {plain})
                         .with_output_dir(env.get_dir())
                         .with_app_name("test");

        ChunkExtractorUtility extractor;
        auto output = extractor(input).get();

        CHECK(output.success == false);
    }

    TEST_CASE("ChunkExtractor - event_of_line keeps events only") {
        CHECK(event_of_line("").empty());
        CHECK(event_of_line("[").empty());
        CHECK(event_of_line("]").empty());

        const std::string event = "{\"name\":\"read\",\"cat\":\"POSIX\"}";
        CHECK(event_of_line(event) == event);
    }

    TEST_CASE("ChunkExtractor - plan_extraction_budget shares the budget") {
        constexpr std::uint64_t MEMBER = 1 << 20;
        constexpr std::uint64_t GIB = std::uint64_t{1} << 30;

        const auto tiny = plan_extraction_budget(1, 4, 16, MEMBER);
        CHECK(tiny.writer_bytes == 4 * MEMBER);
        CHECK(tiny.reserve_bytes > tiny.writer_bytes);

        const auto shared = plan_extraction_budget(GIB, 4, 16, MEMBER);
        CHECK(shared.writer_bytes == 4 * MEMBER * 18);

        const auto small = plan_extraction_budget(32 * MEMBER, 4, 16, MEMBER);
        CHECK(small.writer_bytes == 8 * MEMBER);
    }

    TEST_CASE("ChunkExtractor - A small budget bounds the extractions held") {
        TestEnvironment env(100);
        const std::string file = env.create_dft_test_gzip_file(20000);

        constexpr std::size_t GROUPS = 6;
        constexpr std::uint64_t RESERVE = 8 * 1024 * 1024;

        const std::string loose_dir = env.get_dir() + "/loose";
        const std::string tight_dir = env.get_dir() + "/tight";
        const auto loose =
            run_admitted(file, loose_dir, GROUPS, RESERVE * GROUPS, RESERVE);
        const auto tight =
            run_admitted(file, tight_dir, GROUPS, RESERVE * 2, RESERVE);
        const auto single = run_admitted(file, env.get_dir() + "/single",
                                         GROUPS, RESERVE / 2, RESERVE);

        CHECK(loose.peak_in_flight <= GROUPS);
        CHECK(tight.peak_in_flight <= 2);
        CHECK(tight.peak_reserved <= RESERVE * 2);
        CHECK(single.peak_in_flight == 1);
        CHECK(single.peak_reserved <= RESERVE / 2);

        for (const auto* run : {&loose, &tight, &single}) {
            REQUIRE(run->outputs.size() == GROUPS);
            for (const auto& output : run->outputs) {
                CHECK(output.success);
                CHECK(output.peak_bytes > 0);
            }
        }
        for (std::size_t i = 0; i < GROUPS; ++i) {
            for (const auto* run : {&tight, &single}) {
                CHECK(run->outputs[i].events == loose.outputs[i].events);
                CHECK(run->outputs[i].event_hash ==
                      loose.outputs[i].event_hash);
                REQUIRE(run->outputs[i].chunks.size() ==
                        loose.outputs[i].chunks.size());
                for (std::size_t c = 0; c < loose.outputs[i].chunks.size();
                     ++c) {
                    CHECK(run->outputs[i].chunks[c].bytes_written ==
                          loose.outputs[i].chunks[c].bytes_written);
                    CHECK(run->outputs[i].chunks[c].events_written ==
                          loose.outputs[i].chunks[c].events_written);
                }
            }
        }
    }
}
