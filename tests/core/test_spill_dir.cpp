#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/filesystem_info.h>
#include <dftracer/utils/core/common/spill_dir.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdlib>
#include <fstream>

using dftracer::utils::FilesystemKind;
using dftracer::utils::spill_dir;
using dftu_utils_test::ScopedTestDir;

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* value) {
        if (value)
            ::setenv(dftracer::utils::constants::SPILL_DIR_ENV, value, 1);
        else
            ::unsetenv(dftracer::utils::constants::SPILL_DIR_ENV);
    }
    ~EnvGuard() { ::unsetenv(dftracer::utils::constants::SPILL_DIR_ENV); }
};

}  // namespace

TEST_CASE("a set variable is the spill directory and is created") {
    ScopedTestDir d("spill_dir_env");
    const std::string want = (d.path() / "nested" / "spill").string();
    EnvGuard env(want.c_str());
    auto dir = spill_dir();
    REQUIRE(dir.has_value());
    CHECK(*dir == want);
    CHECK(fs::is_directory(want));
}

TEST_CASE("a set variable that cannot be used is an error naming it") {
    ScopedTestDir d("spill_dir_bad");
    const fs::path file = d.path() / "file";
    std::ofstream(file) << "x";
    EnvGuard env(file.string().c_str());
    auto dir = spill_dir();
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().message.find("DFTRACER_UTILS_SPILL_DIR") !=
          std::string::npos);
}

TEST_CASE("an unset variable gives a writable directory that is not RAM") {
    EnvGuard env(nullptr);
    auto dir = spill_dir();
    REQUIRE(dir.has_value());
    CHECK(fs::is_directory(*dir));
    const fs::path probe = fs::path(*dir) / "dftu_probe";
    std::ofstream out(probe);
    CHECK(out.good());
    out.close();
    fs::remove(probe);
}
