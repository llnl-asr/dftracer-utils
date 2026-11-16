// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/cli/comm_args.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <stdlib.h>

using namespace dftracer::utils;

namespace {

const char* const VARIABLES[] = {"DFTRACER_UTILS_COMM",
                                 "DFTRACER_UTILS_COMM_PATH",
                                 "PMIX_RANK",
                                 "PMIX_NAMESPACE",
                                 "OMPI_COMM_WORLD_SIZE",
                                 "PMI_RANK",
                                 "PMI_SIZE",
                                 "MV2_COMM_WORLD_SIZE",
                                 "MPI_LOCALNRANKS",
                                 "FLUX_TASK_RANK",
                                 "SLURM_STEP_NUM_TASKS",
                                 "SLURM_PROCID",
                                 "FLUX_JOB_SIZE",
                                 "FLUX_TASK_COUNT",
                                 "DFTRACER_UTILS_THREADS"};

// Clears every variable that picks a backend, and puts them back.
struct CleanEnvironment {
    CleanEnvironment() {
        for (const char* name : VARIABLES) {
            const char* value = ::getenv(name);
            saved.push_back(value != nullptr ? dstd::String(value)
                                             : dstd::String());
            was_set.push_back(value != nullptr);
            ::unsetenv(name);
        }
    }
    ~CleanEnvironment() {
        std::size_t at = 0;
        for (const char* name : VARIABLES) {
            if (was_set[at])
                ::setenv(name, saved[at].c_str(), 1);
            else
                ::unsetenv(name);
            ++at;
        }
    }
    dstd::Vector<dstd::String> saved;
    dstd::Vector<bool> was_set;
};

struct Tool : cli::ArgParse {
    explicit Tool(cli::ArgParser& parser) : ArgParse(parser) {
        schema(comm_args);
    }
    cli::CommArgs comm_args;
};

// The exit code when the parse ends the run, else nothing.
dstd::Optional<int> parse(Tool& tool, dstd::Vector<const char*> words) {
    dstd::Vector<char*> argv;
    for (const char* word : words) argv.push_back(const_cast<char*>(word));
    tool.setup();
    return tool.parse(static_cast<int>(argv.size()), argv.data());
}

// A mask of the CPUs from `first` up to `last`, as words of 64.
dstd::Vector<std::uint64_t> cpus(std::size_t first, std::size_t last) {
    dstd::Vector<std::uint64_t> words(2, 0);
    for (std::size_t cpu = first; cpu < last; ++cpu)
        words[cpu / 64] |= std::uint64_t{1} << (cpu % 64);
    return words;
}

std::size_t share(const dstd::Vector<dstd::Vector<std::uint64_t>>& masks,
                  std::size_t rank, std::size_t limit) {
    return cli::core_share(dstd::Span<const dstd::Vector<std::uint64_t>>(
                               masks.data(), masks.size()),
                           rank, limit);
}

}  // namespace

TEST_SUITE("CliCoreShare") {
    TEST_CASE("ranks bound to disjoint CPUs keep their own") {
        const dstd::Vector<dstd::Vector<std::uint64_t>> masks = {cpus(0, 24),
                                                                 cpus(24, 48)};
        CHECK(share(masks, 0, 48) == 24);
        CHECK(share(masks, 1, 48) == 24);
    }

    TEST_CASE("ranks bound to overlapping CPUs split their union") {
        const dstd::Vector<dstd::Vector<std::uint64_t>> masks = {cpus(0, 32),
                                                                 cpus(16, 48)};
        CHECK(share(masks, 0, 48) == 24);
        CHECK(share(masks, 1, 48) == 24);
    }

    TEST_CASE("ranks that all see the whole node split it") {
        const dstd::Vector<dstd::Vector<std::uint64_t>> masks(4, cpus(0, 48));
        for (std::size_t rank = 0; rank < 4; ++rank)
            CHECK(share(masks, rank, 48) == 12);
    }

    TEST_CASE("unequal masks scale by their own CPUs") {
        const dstd::Vector<dstd::Vector<std::uint64_t>> masks = {cpus(0, 48),
                                                                 cpus(0, 16)};
        CHECK(share(masks, 0, 48) == 36);
        CHECK(share(masks, 1, 48) == 12);
    }

    TEST_CASE("the core limit of the process caps the share") {
        const dstd::Vector<dstd::Vector<std::uint64_t>> masks(2, cpus(0, 48));
        CHECK(share(masks, 0, 2) == 2);
        const dstd::Vector<dstd::Vector<std::uint64_t>> alone = {cpus(0, 48)};
        CHECK(share(alone, 0, 2) == 2);
    }

    TEST_CASE("one rank keeps its CPUs, and an empty mask gives one") {
        const dstd::Vector<dstd::Vector<std::uint64_t>> alone = {cpus(0, 48)};
        CHECK(share(alone, 0, 48) == 48);
        const dstd::Vector<dstd::Vector<std::uint64_t>> none = {cpus(0, 0)};
        CHECK(share(none, 0, 48) == 1);
    }
}

TEST_SUITE("CliCommArgs") {
    TEST_CASE("with no option and no launcher a tool is one local rank") {
        CleanEnvironment clean;
        cli::ArgParser parser("tool", "1", "A test tool.");
        Tool tool(parser);
        CHECK_FALSE(parse(tool, {"tool"}).has_value());
        CHECK(tool.comm_args.world().size() == 1);
        CHECK(tool.comm_args.world().rank() == 0);
        CHECK(tool.comm_args.node().size() == 1);
        CHECK(::getenv("DFTRACER_UTILS_THREADS") == nullptr);
    }

    TEST_CASE("--comm local is one rank under a launcher") {
        CleanEnvironment clean;
        ::setenv("OMPI_COMM_WORLD_SIZE", "4", 1);
        cli::ArgParser parser("tool", "1", "A test tool.");
        Tool tool(parser);
        CHECK_FALSE(parse(tool, {"tool", "--comm", "local"}).has_value());
        CHECK(tool.comm_args.world().size() == 1);
    }

    TEST_CASE("the variable picks the backend and the option wins over it") {
        CleanEnvironment clean;
        ::setenv("DFTRACER_UTILS_COMM", "bogus", 1);
        {
            cli::ArgParser parser("tool", "1", "A test tool.");
            Tool tool(parser);
            const auto code = parse(tool, {"tool"});
            REQUIRE(code.has_value());
            CHECK(*code != 0);
        }
        cli::ArgParser parser("tool", "1", "A test tool.");
        Tool tool(parser);
        CHECK_FALSE(parse(tool, {"tool", "--comm", "local"}).has_value());
        CHECK(tool.comm_args.world().size() == 1);
    }

    TEST_CASE("an unknown backend name ends the parse with an error") {
        CleanEnvironment clean;
        cli::ArgParser parser("tool", "1", "A test tool.");
        Tool tool(parser);
        const auto code = parse(tool, {"tool", "--comm", "carrier-pigeon"});
        REQUIRE(code.has_value());
        CHECK(*code != 0);
    }

    TEST_CASE("mpi with no backend library ends the parse with an error") {
        CleanEnvironment clean;
        ::setenv("DFTRACER_UTILS_COMM_PATH", "/nonexistent-dftu-comm-dir", 1);
        cli::ArgParser parser("tool", "1", "A test tool.");
        Tool tool(parser);
        const auto code = parse(tool, {"tool", "--comm", "mpi"});
        REQUIRE(code.has_value());
        CHECK(*code != 0);
    }

    TEST_CASE("auto under a launcher with no backend library is an error") {
        CleanEnvironment clean;
        ::setenv("DFTRACER_UTILS_COMM_PATH", "/nonexistent-dftu-comm-dir", 1);
        ::setenv("OMPI_COMM_WORLD_SIZE", "2", 1);
        cli::ArgParser parser("tool", "1", "A test tool.");
        Tool tool(parser);
        const auto code = parse(tool, {"tool"});
        REQUIRE(code.has_value());
        CHECK(*code != 0);
    }
}
