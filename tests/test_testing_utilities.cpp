// The shared test helpers themselves. They are used by every other test, so a
// defect here reads as a failure somewhere unrelated.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include "testing_runtime.h"
#include "testing_utilities.h"

using namespace dftu_utils_test;

TEST_SUITE("testing utilities") {
    TEST_CASE("make_unique_test_path never repeats, and carries the pid") {
        const auto a = make_unique_test_path("probe");
        const auto b = make_unique_test_path("probe");
        CHECK(a != b);
        // The pid is the component that keeps concurrent test PROCESSES apart,
        // which is the failure this path shape exists to prevent.
        const std::string pid = std::to_string(::getpid());
        CHECK(a.filename().string().find("probe_" + pid + "_") == 0);
    }

    TEST_CASE(
        "ScopedTestDir creates on construction and removes on scope exit") {
        fs::path kept;
        {
            ScopedTestDir dir("scoped_probe");
            kept = dir.path();
            REQUIRE(fs::exists(kept));
            std::ofstream(dir.file("inner.txt")) << "x";
            CHECK(fs::exists(dir.file("inner.txt")));
        }
        CHECK_FALSE(fs::exists(kept));
    }

    TEST_CASE("ScopedTestDir removes its tree even when the scope throws") {
        fs::path kept;
        try {
            ScopedTestDir dir("throwing_probe");
            kept = dir.path();
            REQUIRE(fs::exists(kept));
            throw std::runtime_error("unwind");
        } catch (const std::runtime_error&) {
        }
        CHECK_FALSE(fs::exists(kept));
    }

    TEST_CASE("run_process reports the child's exit code") {
        CHECK(run_process("/bin/sh", {"-c", "exit 0"}) == 0);
        CHECK(run_process("/bin/sh", {"-c", "exit 7"}) == 7);
        CHECK(run_process("/nonexistent/binary", {}) == 127);
    }

    TEST_CASE("run_process_capture reads stdout, and stderr only when asked") {
        int rc = -99;
        CHECK(run_process_capture("/bin/sh", {"-c", "echo out; echo err 1>&2"},
                                  false, &rc) == "out\n");
        CHECK(rc == 0);

        const std::string both = run_process_capture(
            "/bin/sh", {"-c", "echo out; echo err 1>&2"}, true, &rc);
        CHECK(both.find("out") != std::string::npos);
        CHECK(both.find("err") != std::string::npos);
        CHECK(rc == 0);

        run_process_capture("/bin/sh", {"-c", "exit 3"}, false, &rc);
        CHECK(rc == 3);
    }

    TEST_CASE("run_process_capture drains more than one pipe buffer") {
        // A child writing past the pipe capacity blocks until it is read, so
        // waiting before draining would deadlock rather than fail.
        int rc = -99;
        const std::string out = run_process_capture(
            "/bin/sh", {"-c", "yes abcdefghij | head -n 40000"}, false, &rc);
        CHECK(rc == 0);
        CHECK(out.size() == 40000 * 11);
    }

    TEST_CASE("gz_first_line returns the first non-empty line") {
        ScopedTestDir dir("gzline_probe");
        const std::string plain = dir.file("in.txt");
        {
            std::ofstream ofs(plain);
            ofs << "\n\nfirst real line\nsecond\n";
        }
        const std::string gz = dir.file("in.txt.gz");
        REQUIRE(compress_file_to_gzip(plain, gz));
        CHECK(gz_first_line(gz) == "first real line");
        CHECK(gz_first_line(dir.file("absent.gz")).empty());
    }

    TEST_CASE("run_coro runs the body and shuts the runtime down") {
        int ran = 0;
        run_coro([&ran](dftracer::utils::CoroScope&)
                     -> dftracer::utils::coro::CoroTask<void> {
            ran = 1;
            co_return;
        });
        CHECK(ran == 1);
    }
}
