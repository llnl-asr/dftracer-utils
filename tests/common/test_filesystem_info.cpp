// SPDX-License-Identifier: MIT
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
    setenv("DFTRACER_UTILS_INDEX_SCRATCH", "/tmp", 1);
    return 0;
}();
}  // namespace

TEST_CASE("filesystem_kind classifies the root filesystem") {
    const common::FilesystemKind k = common::filesystem_kind("/");
    CHECK(k != common::FilesystemKind::Unknown);
    // A not-yet-created path resolves to its parent's filesystem.
    CHECK(common::filesystem_kind("/tmp/does/not/exist/yet") !=
          common::FilesystemKind::Unknown);
}

TEST_CASE("kind predicates are mutually consistent") {
    CHECK(common::is_local_filesystem(common::FilesystemKind::Local));
    CHECK(common::is_local_filesystem(common::FilesystemKind::Tmpfs));
    CHECK(common::is_memory_filesystem(common::FilesystemKind::Tmpfs));
    CHECK_FALSE(common::is_memory_filesystem(common::FilesystemKind::Local));
    CHECK(common::is_network_filesystem(common::FilesystemKind::Lustre));
    CHECK_FALSE(common::is_network_filesystem(common::FilesystemKind::Local));
    CHECK_FALSE(common::is_network_filesystem(common::FilesystemKind::Tmpfs));
}

TEST_CASE("list_mounts reports the root mount") {
    const auto mounts = common::list_mounts();
    REQUIRE_FALSE(mounts.empty());
    bool has_root = false;
    for (const auto& m : mounts)
        if (m.mountpoint.str() == "/") has_root = true;
    CHECK(has_root);
}

TEST_CASE("find_mountpoint returns a mounted ancestor") {
    const std::string mp(common::find_mountpoint("/tmp/a/b/c/deep/path").str());
    REQUIRE_FALSE(mp.empty());
    // The result must be a prefix of the queried path and a real directory.
    CHECK(common::fs::exists(mp));
    // A mount root maps to itself.
    CHECK(common::find_mountpoint("/").str() == "/");
}

TEST_CASE("ScratchSession creates and cleans up its directory") {
    std::string dir;
    {
        common::ScratchSession s;
        REQUIRE(s.valid());  // /tmp forced above
        dir = std::string(s.dir().str());
        CHECK(common::fs::exists(dir));
        CHECK(common::fs::is_directory(dir));
    }
    CHECK_FALSE(common::fs::exists(dir));  // removed on destruction
}

TEST_CASE("ScratchSession release keeps the directory") {
    std::string dir;
    {
        common::ScratchSession s;
        REQUIRE(s.valid());
        dir = std::string(s.dir().str());
        s.release();
    }
    CHECK(common::fs::exists(dir));
    std::error_code ec;
    common::fs::remove_all(dir, ec);
}

TEST_CASE("publish_path moves a built directory to its destination") {
    common::ScratchSession s;
    REQUIRE(s.valid());
    const std::string src = std::string(s.dir().str()) + "/build";
    common::fs::create_directories(src);
    {
        std::ofstream(src + "/marker.txt") << "hello";
    }

    const std::string dst = std::string(s.dir().str()) + "/published";
    common::publish_path(dftracer::utils::dstd::PathView(src),
                         dftracer::utils::dstd::PathView(dst));

    CHECK_FALSE(common::fs::exists(src));
    CHECK(common::fs::exists(dst + "/marker.txt"));
}

TEST_CASE("should_stage honors the never override") {
    setenv("DFTRACER_UTILS_INDEX_STAGE", "never", 1);
    CHECK_FALSE(common::should_stage("/tmp/anywhere"));
    unsetenv("DFTRACER_UTILS_INDEX_STAGE");
}
