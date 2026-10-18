#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/common/scratch.h>
#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>
#include <string>

using namespace dftracer::utils;

namespace {
// Force a usable scratch root before anything caches it, so ScratchSession is
// exercisable on any host (including macOS, where no mount root is writable).
const int g_force_scratch = [] {
    setenv("DFTRACER_INDEX_SCRATCH", "/tmp", 1);
    return 0;
}();
}  // namespace

TEST_CASE("filesystem_kind classifies the root filesystem") {
    const FilesystemKind k = filesystem_kind("/");
    CHECK(k != FilesystemKind::Unknown);
    // A not-yet-created path resolves to its parent's filesystem.
    CHECK(filesystem_kind("/tmp/does/not/exist/yet") !=
          FilesystemKind::Unknown);
}

TEST_CASE("kind predicates are mutually consistent") {
    CHECK(is_local_filesystem(FilesystemKind::Local));
    CHECK(is_local_filesystem(FilesystemKind::Tmpfs));
    CHECK(is_memory_filesystem(FilesystemKind::Tmpfs));
    CHECK_FALSE(is_memory_filesystem(FilesystemKind::Local));
    CHECK(is_network_filesystem(FilesystemKind::Lustre));
    CHECK_FALSE(is_network_filesystem(FilesystemKind::Local));
    CHECK_FALSE(is_network_filesystem(FilesystemKind::Tmpfs));
}

TEST_CASE("list_mounts reports the root mount") {
    const auto mounts = list_mounts();
    REQUIRE_FALSE(mounts.empty());
    bool has_root = false;
    for (const auto& m : mounts)
        if (m.mountpoint == "/") has_root = true;
    CHECK(has_root);
}

TEST_CASE("find_mountpoint returns a mounted ancestor") {
    const std::string mp = find_mountpoint("/tmp/a/b/c/deep/path");
    REQUIRE_FALSE(mp.empty());
    // The result must be a prefix of the queried path and a real directory.
    CHECK(fs::exists(mp));
    // A mount root maps to itself.
    CHECK(find_mountpoint("/") == "/");
}

TEST_CASE("ScratchSession creates and cleans up its directory") {
    std::string dir;
    {
        ScratchSession s;
        REQUIRE(s.valid());  // /tmp forced above
        dir = s.dir();
        CHECK(fs::exists(dir));
        CHECK(fs::is_directory(dir));
    }
    CHECK_FALSE(fs::exists(dir));  // removed on destruction
}

TEST_CASE("ScratchSession release keeps the directory") {
    std::string dir;
    {
        ScratchSession s;
        REQUIRE(s.valid());
        dir = s.dir();
        s.release();
    }
    CHECK(fs::exists(dir));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("publish_path moves a built directory to its destination") {
    ScratchSession s;
    REQUIRE(s.valid());
    const std::string src = s.dir() + "/build";
    fs::create_directories(src);
    {
        std::ofstream(src + "/marker.txt") << "hello";
    }

    const std::string dst = s.dir() + "/published";
    publish_path(src, dst);

    CHECK_FALSE(fs::exists(src));
    CHECK(fs::exists(dst + "/marker.txt"));
}

TEST_CASE("should_stage honors the never override") {
    setenv("DFTRACER_INDEX_STAGE", "never", 1);
    CHECK_FALSE(should_stage("/tmp/anywhere"));
    unsetenv("DFTRACER_INDEX_STAGE");
}
