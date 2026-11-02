// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/spill_dir.h>
#include <dftracer/utils/common/spill_dir_internal.h>
#include <dftracer/utils/common/spill_file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

// A test directory under the working directory, which ctest puts in the build
// tree on disk. The system temp directory may be tmpfs, which the spill
// directory refuses as a fallback and warns about.
class DiskDir {
   public:
    explicit DiskDir(const std::string& tag)
        : path_(dftracer::utils::dstd::fs::current_path().value() /
                (tag + "_" + std::to_string(::getpid()) + "_" +
                 std::to_string(next_id()))) {
        REQUIRE(dftracer::utils::dstd::fs::remove_all(path_).has_value());
        REQUIRE(
            dftracer::utils::dstd::fs::create_directories(path_).has_value());
    }
    DiskDir(const DiskDir&) = delete;
    DiskDir& operator=(const DiskDir&) = delete;
    ~DiskDir() { (void)dftracer::utils::dstd::fs::remove_all(path_); }
    const dftracer::utils::dstd::Path& path() const { return path_; }

   private:
    static unsigned next_id() {
        static unsigned id = 0;
        return id++;
    }
    dftracer::utils::dstd::Path path_;
};

struct EnvGuard {
    explicit EnvGuard(const char* value) {
        if (value)
            ::setenv(dftracer::utils::common::constants::SPILL_DIR_ENV, value,
                     1);
        else
            ::unsetenv(dftracer::utils::common::constants::SPILL_DIR_ENV);
    }
    ~EnvGuard() {
        ::unsetenv(dftracer::utils::common::constants::SPILL_DIR_ENV);
    }
};

}  // namespace

TEST_CASE("a set variable is the spill directory and is created") {
    DiskDir d("spill_dir_env");
    const std::string want = std::string((d.path() / "nested" / "spill").str());
    EnvGuard env(want.c_str());
    auto dir = dftracer::utils::common::spill_dir();
    REQUIRE(dir.has_value());
    CHECK(dir->str() == want);
    CHECK(dftracer::utils::dstd::fs::is_directory(want).value());
}

TEST_CASE("a set variable that cannot be used is an error naming it") {
    DiskDir d("spill_dir_bad");
    const dftracer::utils::dstd::Path file = d.path() / "file";
    std::ofstream(file.c_str()) << "x";
    EnvGuard env(file.c_str());
    auto dir = dftracer::utils::common::spill_dir();
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().message().find("DFTRACER_UTILS_SPILL_DIR") !=
          std::string::npos);
}

TEST_CASE("an unset variable gives a writable directory that is not RAM") {
    EnvGuard env(nullptr);
    auto dir = dftracer::utils::common::spill_dir();
    REQUIRE(dir.has_value());
    CHECK(dftracer::utils::dstd::fs::is_directory(dir->c_str()).value());
    CHECK_FALSE(dftracer::utils::common::is_memory_filesystem(
        dftracer::utils::common::filesystem_kind(*dir)));
    const dftracer::utils::dstd::Path probe =
        dftracer::utils::dstd::Path(dir->c_str()) / "dftu_probe";
    std::ofstream out(probe.c_str());
    CHECK(out.good());
    out.close();
    (void)dftracer::utils::dstd::fs::remove(probe);
}

TEST_CASE("without a variable or a local disk the first usable fallback wins") {
    DiskDir d("spill_dir_fallback");
    const dftracer::utils::dstd::Path file = d.path() / "file";
    std::ofstream(file.c_str()) << "x";
    const std::string good = std::string((d.path() / "cache" / "spill").str());
    auto dir = dftracer::utils::common::detail::choose_spill_dir(
        "", {},
        {dftracer::utils::dstd::Path((file / "sub").str()),
         dftracer::utils::dstd::Path(good)});
    REQUIRE(dir.has_value());
    CHECK(dir->str() == good);
    CHECK(dftracer::utils::dstd::fs::is_directory(good).value());
}

TEST_CASE("a RAM-backed fallback is never the spill directory") {
    if (dftracer::utils::common::filesystem_kind("/dev/shm") !=
        dftracer::utils::common::FilesystemKind::Tmpfs) {
        MESSAGE("/dev/shm is not tmpfs here; nothing RAM-backed to refuse");
        return;
    }
    DiskDir d("spill_dir_ram");
    const std::string disk = std::string((d.path() / "spill").str());
    const std::string ram = "/dev/shm/dftu_spill_dir_test";
    auto dir = dftracer::utils::common::detail::choose_spill_dir(
        "", {},
        {dftracer::utils::dstd::Path(ram), dftracer::utils::dstd::Path(disk)});
    REQUIRE(dir.has_value());
    CHECK(dir->str() == disk);
    auto none = dftracer::utils::common::detail::choose_spill_dir(
        "", {}, {dftracer::utils::dstd::Path(ram)});
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().message().find("DFTRACER_UTILS_SPILL_DIR") !=
          std::string::npos);
    CHECK_FALSE(dftracer::utils::dstd::fs::exists(ram).value());
}

TEST_CASE("no usable spill directory is an error naming the variable") {
    DiskDir d("spill_dir_none");
    const dftracer::utils::dstd::Path file = d.path() / "file";
    std::ofstream(file.c_str()) << "x";
    auto dir = dftracer::utils::common::detail::choose_spill_dir(
        "", {}, {dftracer::utils::dstd::Path((file / "sub").str())});
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().message().find("DFTRACER_UTILS_SPILL_DIR") !=
          std::string::npos);
}

TEST_CASE("the sweep removes dead directories of this host only") {
    DiskDir root("sweep_host");
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
    REQUIRE(dftracer::utils::dstd::fs::create_directories(mine).has_value());
    REQUIRE(dftracer::utils::dstd::fs::create_directories(other).has_value());
    auto sub = dftracer::utils::common::ScopedSpillSubdir::create("sweeptest");
    REQUIRE(sub);
    CHECK_FALSE(dftracer::utils::dstd::fs::exists(mine).value());
    CHECK(dftracer::utils::dstd::fs::exists(other).value());
}

TEST_CASE("a spill dir on tmpfs warns once") {
    if (dftracer::utils::common::filesystem_kind("/dev/shm") !=
        dftracer::utils::common::FilesystemKind::Tmpfs) {
        MESSAGE("/dev/shm is not tmpfs here (Linux only); test not run");
        return;
    }
    if (!DFTRACER_UTILS_LOGGER_WARN_ENABLED) {
        MESSAGE("warnings are compiled out; test not run");
        return;
    }
    std::FILE* sink = std::tmpfile();
    REQUIRE(sink != nullptr);
    dftracer::utils::common::logger::Config cfg;
    cfg.level = dftracer::utils::common::logger::Level::Warn;
    cfg.sink = sink;
    dftracer::utils::common::logger::init(cfg);
    const std::string ram =
        "/dev/shm/dftu_spill_warn_" + std::to_string(::getpid());
    {
        EnvGuard env(ram.c_str());
        REQUIRE(dftracer::utils::common::spill_dir().has_value());
        CHECK(dftracer::utils::common::spill_dir_in_memory(ram.c_str()));
        REQUIRE(dftracer::utils::common::spill_dir().has_value());
        auto file = dftracer::utils::common::SpillFile::create();
        REQUIRE(file.has_value());
        const std::vector<char> bytes(4096, 'x');
        REQUIRE((*file)->append(bytes.data(), bytes.size()).has_value());
        auto sub =
            dftracer::utils::common::ScopedSpillSubdir::create("dftu_warn");
        REQUIRE(sub.has_value());
    }
    dftracer::utils::common::logger::init(
        dftracer::utils::common::logger::Config{});
    std::fflush(sink);
    std::rewind(sink);
    std::string text;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), sink)) > 0;)
        text.append(buf, n);
    std::fclose(sink);
    (void)dftracer::utils::dstd::fs::remove_all(ram);

    std::size_t warnings = 0;
    for (std::size_t at = text.find("RAM-backed"); at != std::string::npos;
         at = text.find("RAM-backed", at + 1))
        ++warnings;
    CHECK(warnings == 1);
    CHECK(text.find(ram) != std::string::npos);
    CHECK(text.find("DFTRACER_UTILS_SPILL_DIR") != std::string::npos);
}
