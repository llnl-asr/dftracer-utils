// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/backend/loader.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <stdlib.h>
#include <testing_utilities.h>

using namespace dftracer::utils;

namespace {

const char* const LAUNCHER_VARIABLES[] = {
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
    "DFTRACER_UTILS_COMM_PATH",
    "DFTRACER_UTILS_COMM",
};

/// Clears every launcher variable for a test case, and puts them back.
struct CleanEnvironment {
    CleanEnvironment() {
        for (const char* name : LAUNCHER_VARIABLES) {
            const char* value = ::getenv(name);
            saved.push_back(value != nullptr ? dstd::String(value)
                                             : dstd::String());
            was_set.push_back(value != nullptr);
            ::unsetenv(name);
        }
    }
    ~CleanEnvironment() {
        std::size_t at = 0;
        for (const char* name : LAUNCHER_VARIABLES) {
            if (was_set[at]) ::setenv(name, saved[at].c_str(), 1);
            ++at;
        }
    }
    dstd::Vector<dstd::String> saved;
    dstd::Vector<bool> was_set;
};

}  // namespace

TEST_SUITE("CommLoader") {
    TEST_CASE("with no launcher variable the backend is local") {
        CleanEnvironment env;
        CHECK_FALSE(comm::backend::launched_by_mpi());
        CHECK(comm::backend::resolve_kind(comm::BackendKind::Auto) ==
              comm::BackendKind::Local);
        auto started = comm::Communicator::start(comm::BackendKind::Auto);
        REQUIRE(started.has_value());
        CHECK(started->size() == 1);
        CHECK(started->rank() == 0);
        Runtime rt(2);
        auto errors = dftu_comm_test::run_ranks(
            rt, 1,
            [&started](std::uint32_t) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group world = started->world(0);
                auto joined = co_await world.barrier();
                co_return joined;
            });
        CHECK_FALSE(errors[0].has_value());
    }

    TEST_CASE("a launcher world size above one makes a launched process") {
        CleanEnvironment env;
        for (const char* name :
             {"OMPI_COMM_WORLD_SIZE", "PMIX_SIZE", "PMI_SIZE",
              "MV2_COMM_WORLD_SIZE", "MPI_LOCALNRANKS"}) {
            ::setenv(name, "2", 1);
            CHECK(comm::backend::launched_by_mpi());
            CHECK(comm::backend::resolve_kind(comm::BackendKind::Auto) ==
                  comm::BackendKind::Mpi);
            ::setenv(name, "1", 1);
            CHECK_FALSE(comm::backend::launched_by_mpi());
            ::unsetenv(name);
        }
        ::setenv("SLURM_STEP_NUM_TASKS", "1", 1);
        ::setenv("SLURM_PROCID", "0", 1);
        CHECK_FALSE(comm::backend::launched_by_mpi());
        ::setenv("SLURM_STEP_NUM_TASKS", "4", 1);
        CHECK(comm::backend::launched_by_mpi());
    }

    TEST_CASE("a launcher of one task, as mpirun -n 1 starts it, is local") {
        CleanEnvironment env;
        // What Open MPI and PMIx set for one rank: ranks and a namespace, and
        // a world size of 1.
        ::setenv("PMIX_RANK", "0", 1);
        ::setenv("PMIX_NAMESPACE", "prterun-host-1@1", 1);
        ::setenv("OMPI_COMM_WORLD_SIZE", "1", 1);
        CHECK_FALSE(comm::backend::launched_by_mpi());
        auto started = comm::Communicator::start(comm::BackendKind::Auto);
        REQUIRE(started.has_value());
        CHECK(started->size() == 1);
    }

    TEST_CASE("an explicit mpi names the paths it tried when none holds it") {
        CleanEnvironment env;
        dftu_utils_test::ScopedTestDir dir("comm_loader_empty");
        ::setenv("DFTRACER_UTILS_COMM_PATH",
                 dstd::String(dir.path().str()).c_str(), 1);
        auto started = comm::Communicator::start(comm::BackendKind::Mpi);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().is(dstd::Condition::NotFound));
        CHECK(dftu_comm_test::has_text(started.error(),
                                       "libdftracer_utils_comm_mpi"));
        CHECK(dftu_comm_test::has_text(started.error(),
                                       dstd::String(dir.path().str()).c_str()));
    }

    TEST_CASE("a launched process without the library is an error, not local") {
        CleanEnvironment env;
        dftu_utils_test::ScopedTestDir dir("comm_loader_launched");
        ::setenv("DFTRACER_UTILS_COMM_PATH",
                 dstd::String(dir.path().str()).c_str(), 1);
        ::setenv("OMPI_COMM_WORLD_SIZE", "4", 1);
        auto started = comm::Communicator::start(comm::BackendKind::Auto);
        REQUIRE_FALSE(started.has_value());
        CHECK(dftu_comm_test::has_text(started.error(), "unrelated copies"));
    }

    TEST_CASE("a library built for another comm is refused") {
        CleanEnvironment env;
        const char* fake = ::getenv("DFTU_TEST_FAKE_BACKEND_DIR");
        if (fake == nullptr) return;
        ::setenv("DFTRACER_UTILS_COMM_PATH", fake, 1);
        auto started = comm::Communicator::start(comm::BackendKind::Mpi);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().is(dstd::Condition::Unsupported));
        CHECK(dftu_comm_test::has_text(started.error(),
                                       "a-stamp-of-another-build"));
    }

    TEST_CASE("a library of another api version is refused") {
        CleanEnvironment env;
        const char* fake = ::getenv("DFTU_TEST_FAKE_API_BACKEND_DIR");
        if (fake == nullptr) return;
        ::setenv("DFTRACER_UTILS_COMM_PATH", fake, 1);
        auto started = comm::Communicator::start(comm::BackendKind::Mpi);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().is(dstd::Condition::Unsupported));
        CHECK(dftu_comm_test::has_text(started.error(), "api 9.9.9"));
    }
}
