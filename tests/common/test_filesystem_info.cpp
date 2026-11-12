// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/common/scratch.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdlib>
#include <fstream>

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
    const dstd::String mp(
        common::find_mountpoint("/tmp/a/b/c/deep/path").str());
    REQUIRE_FALSE(mp.empty());
    // The result must be a prefix of the queried path and a real directory.
    CHECK(dstd::fs::exists(mp).value());
    // A mount root maps to itself.
    CHECK(common::find_mountpoint("/").str() == "/");
}

TEST_CASE("ScratchSession creates and cleans up its directory") {
    dstd::String dir;
    {
        common::ScratchSession s;
        REQUIRE(s.valid());  // /tmp forced above
        dir = dstd::String(s.dir().str());
        CHECK(dstd::fs::exists(dir).value());
        CHECK(dstd::fs::is_directory(dir).value());
    }
    CHECK_FALSE(dstd::fs::exists(dir).value());  // removed on destruction
}

TEST_CASE("ScratchSession release keeps the directory") {
    dstd::String dir;
    {
        common::ScratchSession s;
        REQUIRE(s.valid());
        dir = dstd::String(s.dir().str());
        s.release();
    }
    CHECK(dstd::fs::exists(dir).value());
    dstd::fs::remove_all(dir).discard();
}

TEST_CASE("publish_path moves a built directory to its destination") {
    common::ScratchSession s;
    REQUIRE(s.valid());
    const dstd::String src = dstd::fmt<"{}/build">(s.dir().str());
    REQUIRE(dstd::fs::create_directories(src).has_value());
    {
        dftu_utils_test::FileOut(dstd::fmt<"{}/marker.txt">(src)) << "hello";
    }

    const dstd::String dst = dstd::fmt<"{}/published">(s.dir().str());
    CHECK(common::publish_path(dstd::PathView(src), dstd::PathView(dst))
              .has_value());

    CHECK_FALSE(dstd::fs::exists(src).value());
    CHECK(dstd::fs::exists(dstd::fmt<"{}/marker.txt">(dst)).value());
}

TEST_CASE("should_stage honors the never override") {
    setenv("DFTRACER_UTILS_INDEX_STAGE", "never", 1);
    CHECK_FALSE(common::should_stage("/tmp/anywhere"));
    unsetenv("DFTRACER_UTILS_INDEX_STAGE");
}
