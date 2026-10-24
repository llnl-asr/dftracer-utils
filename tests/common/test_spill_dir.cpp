#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/spill_dir.h>
#include <dftracer/utils/common/spill_dir_internal.h>
#include <dftracer/utils/common/spill_file.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using dftracer::utils::FilesystemKind;
using dftracer::utils::spill_dir;

namespace {

// A test directory under the working directory, which ctest puts in the build
// tree on disk. The system temp directory may be tmpfs, which the spill
// directory refuses as a fallback and warns about.
class DiskDir {
   public:
    explicit DiskDir(const std::string& tag)
        : path_(fs::current_path() / (tag + "_" + std::to_string(::getpid()) +
                                      "_" + std::to_string(next_id()))) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    DiskDir(const DiskDir&) = delete;
    DiskDir& operator=(const DiskDir&) = delete;
    ~DiskDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

   private:
    static unsigned next_id() {
        static unsigned id = 0;
        return id++;
    }
    fs::path path_;
};

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
    DiskDir d("spill_dir_env");
    const std::string want = (d.path() / "nested" / "spill").string();
    EnvGuard env(want.c_str());
    auto dir = spill_dir();
    REQUIRE(dir.has_value());
    CHECK(*dir == want);
    CHECK(fs::is_directory(want));
}

TEST_CASE("a set variable that cannot be used is an error naming it") {
    DiskDir d("spill_dir_bad");
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
    DiskDir d("spill_dir_fallback");
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
    if (dftracer::utils::filesystem_kind("/dev/shm") != FilesystemKind::Tmpfs) {
        MESSAGE("/dev/shm is not tmpfs here; nothing RAM-backed to refuse");
        return;
    }
    DiskDir d("spill_dir_ram");
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
    DiskDir d("spill_dir_none");
    const fs::path file = d.path() / "file";
    std::ofstream(file) << "x";
    auto dir = choose_spill_dir("", "", {(file / "sub").string()});
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().message.find("DFTRACER_UTILS_SPILL_DIR") !=
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
    std::filesystem::create_directories(mine);
    std::filesystem::create_directories(other);
    auto sub = dftracer::utils::ScopedSpillSubdir::create("sweeptest");
    REQUIRE(sub);
    CHECK_FALSE(std::filesystem::exists(mine));
    CHECK(std::filesystem::exists(other));
}

TEST_CASE("a spill dir on tmpfs warns once") {
    namespace logger = dftracer::utils::logger;
    if (dftracer::utils::filesystem_kind("/dev/shm") != FilesystemKind::Tmpfs) {
        MESSAGE("/dev/shm is not tmpfs here (Linux only); test not run");
        return;
    }
    if (!DFTRACER_UTILS_LOGGER_WARN_ENABLED) {
        MESSAGE("warnings are compiled out; test not run");
        return;
    }
    std::FILE* sink = std::tmpfile();
    REQUIRE(sink != nullptr);
    logger::Config cfg;
    cfg.level = logger::Level::Warn;
    cfg.sink = sink;
    logger::init(cfg);
    const std::string ram =
        "/dev/shm/dftu_spill_warn_" + std::to_string(::getpid());
    {
        EnvGuard env(ram.c_str());
        REQUIRE(spill_dir().has_value());
        CHECK(dftracer::utils::spill_dir_in_memory(ram));
        REQUIRE(spill_dir().has_value());
        auto file = dftracer::utils::SpillFile::create();
        REQUIRE(file.has_value());
        const std::vector<char> bytes(4096, 'x');
        REQUIRE((*file)->append(bytes.data(), bytes.size()).has_value());
        auto sub = dftracer::utils::ScopedSpillSubdir::create("dftu_warn");
        REQUIRE(sub.has_value());
    }
    logger::init(logger::Config{});
    std::fflush(sink);
    std::rewind(sink);
    std::string text;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), sink)) > 0;)
        text.append(buf, n);
    std::fclose(sink);
    std::error_code ec;
    fs::remove_all(ram, ec);

    std::size_t warnings = 0;
    for (std::size_t at = text.find("RAM-backed"); at != std::string::npos;
         at = text.find("RAM-backed", at + 1))
        ++warnings;
    CHECK(warnings == 1);
    CHECK(text.find(ram) != std::string::npos);
    CHECK(text.find("DFTRACER_UTILS_SPILL_DIR") != std::string::npos);
}
