// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <sys/stat.h>
#include <testing_utilities.h>
#include <unistd.h>

using namespace dftracer::utils;

namespace {

// Makes a directory read-only for the scope and restores it afterwards so the
// test environment can remove it.
class ReadOnlyDir {
   public:
    explicit ReadOnlyDir(const dstd::String& path) : path_(path) {
        ::chmod(path_.c_str(), 0500);
    }
    ~ReadOnlyDir() { ::chmod(path_.c_str(), 0700); }
    ReadOnlyDir(const ReadOnlyDir&) = delete;
    ReadOnlyDir& operator=(const ReadOnlyDir&) = delete;

   private:
    dstd::String path_;
};

// A tool whose index cannot be written reports "Error: ..." and exits 1.
void check_fails_on_read_only_dir(const char* env_name, const char* tool) {
    if (::geteuid() == 0) {
        MESSAGE(
            "running as root, a read-only directory is writable, skipping.");
        return;
    }
    const auto binary = dftu_utils_test::find_binary_by_name(env_name, tool);
    if (binary.empty()) {
        MESSAGE(tool << " binary not found, skipping.");
        return;
    }

    dftu_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());
    const auto trace_gz = env.create_dft_test_gzip_file(20);
    REQUIRE(!trace_gz.empty());
    const dstd::String trace = env.dir() + "/trace.pfw.gz";
    REQUIRE(dstd::fs::rename(trace_gz, trace).has_value());

    ReadOnlyDir read_only(env.dir());
    dftu_utils_test::set_test_library_path(binary);
    int rc = 0;
    const auto output =
        dftu_utils_test::run_process_capture(binary, {env.dir()}, true, &rc);
    CHECK(rc == 1);
    CHECK(output.find("Error:") != dstd::String::npos);
}

}  // namespace

TEST_CASE("dftracer_info reports an unwritable index directory") {
    check_fails_on_read_only_dir("DFTRACER_UTILS_INFO_PATH", "dftracer_info");
}

TEST_CASE("dftracer_index reports an unwritable index directory") {
    check_fails_on_read_only_dir("DFTRACER_UTILS_INDEX_PATH", "dftracer_index");
}
