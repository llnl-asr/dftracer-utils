#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/filesystem_info.h>
#include <dftracer/utils/core/common/spill_dir.h>
#include <dftracer/utils/core/common/spill_dir_internal.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

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
    CHECK_FALSE(dftracer::utils::is_memory_filesystem(
        dftracer::utils::filesystem_kind(*dir)));
    const fs::path probe = fs::path(*dir) / "dftu_probe";
    std::ofstream out(probe);
    CHECK(out.good());
    out.close();
    fs::remove(probe);
}

TEST_CASE("without a variable or a local disk the first usable fallback wins") {
    using dftracer::utils::detail::choose_spill_dir;
    ScopedTestDir d("spill_dir_fallback");
    const fs::path file = d.path() / "file";
    std::ofstream(file) << "x";
    const std::string good = (d.path() / "cache" / "spill").string();
    auto dir = choose_spill_dir("", "", {(file / "sub").string(), good});
    REQUIRE(dir.has_value());
    CHECK(*dir == good);
    CHECK(fs::is_directory(good));
}

TEST_CASE("a RAM-backed fallback is never the spill directory") {
    using dftracer::utils::detail::choose_spill_dir;
    if (dftracer::utils::filesystem_kind("/dev/shm") != FilesystemKind::TMPFS) {
        MESSAGE("/dev/shm is not tmpfs here; nothing RAM-backed to refuse");
        return;
    }
    ScopedTestDir d("spill_dir_ram");
    const std::string disk = (d.path() / "spill").string();
    const std::string ram = "/dev/shm/dftu_spill_dir_test";
    auto dir = choose_spill_dir("", "", {ram, disk});
    REQUIRE(dir.has_value());
    CHECK(*dir == disk);
    auto none = choose_spill_dir("", "", {ram});
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().message.find("DFTRACER_UTILS_SPILL_DIR") !=
          std::string::npos);
    CHECK_FALSE(fs::exists(ram));
}

TEST_CASE("no usable spill directory is an error naming the variable") {
    using dftracer::utils::detail::choose_spill_dir;
    ScopedTestDir d("spill_dir_none");
    const fs::path file = d.path() / "file";
    std::ofstream(file) << "x";
    auto dir = choose_spill_dir("", "", {(file / "sub").string()});
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().message.find("DFTRACER_UTILS_SPILL_DIR") !=
          std::string::npos);
}

TEST_CASE("the sweep removes dead directories of this host only") {
    ScopedTestDir root("sweep_host");
    EnvGuard env(root.path().c_str());
    char host[256] = {};
    ::gethostname(host, sizeof(host) - 1);
    std::string tag(host);
    for (char& c : tag)
        if (c == '/' || c == '_') c = '-';
    const auto dead_pid = "2147483646";
    const auto mine =
        root.path() / ("sweeptest_" + tag + "_" + dead_pid + "_0");
    const auto other =
        root.path() / (std::string("sweeptest_otherhost_") + dead_pid + "_0");
    std::filesystem::create_directories(mine);
    std::filesystem::create_directories(other);
    auto sub = dftracer::utils::ScopedSpillSubdir::create("sweeptest");
    REQUIRE(sub);
    CHECK_FALSE(std::filesystem::exists(mine));
    CHECK(std::filesystem::exists(other));
}
