// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>

using namespace dftracer::utils;
using dftu_utils_test::ScopedTestDir;
using dstd::Path;
using dstd::PathView;

namespace dfs = dstd::fs;

namespace {

Path in(const ScopedTestDir& dir, const dstd::String& name) {
    return Path(dir.file(name));
}

dstd::String read_text(const Path& path) {
    // keep std: independent of dstd::fs::read_file
    std::ifstream stream(path.c_str(), std::ios::binary);
    return dstd::String(std::istreambuf_iterator<char>(stream),
                        std::istreambuf_iterator<char>());
}

bool running_as_root() { return ::geteuid() == 0; }

bool contains(dstd::StringView text, dstd::StringView part) {
    return text.find(part) != dstd::StringView::npos;
}

}  // namespace

TEST_CASE("stat reports size, kind and mtime") {
    ScopedTestDir dir("dstd_fs_stat");
    dir.create_file("a.txt", "hello");
    dir.create_dir("sub");

    const Path file = in(dir, "a.txt");
    const auto info = dfs::stat(file.view());
    REQUIRE(info.has_value());
    CHECK(info->size == 5);
    CHECK(info->kind == dfs::FileKind::File);
    CHECK(info->mtime_ns > 0);
    CHECK(info->ino != 0);
    CHECK(info->dev != 0);
    CHECK(dfs::file_size(file.view()).value() == 5);
    CHECK(dfs::mtime_ns(file.view()).value() == info->mtime_ns);

    const auto sub = dfs::stat(in(dir, "sub").view());
    REQUIRE(sub.has_value());
    CHECK(sub->kind == dfs::FileKind::Directory);

    const dstd::String link = dir.file("link");
    REQUIRE(::symlink("a.txt", link.c_str()) == 0);
    const auto via_lstat = dfs::lstat(Path(link).view());
    REQUIRE(via_lstat.has_value());
    CHECK(via_lstat->kind == dfs::FileKind::Symlink);
    const auto via_stat = dfs::stat(Path(link).view());
    REQUIRE(via_stat.has_value());
    CHECK(via_stat->kind == dfs::FileKind::File);
    CHECK(via_stat->size == 5);
}

TEST_CASE("a failure carries the errno text and its condition") {
    ScopedTestDir dir("dstd_fs_errors");

    const auto missing = dfs::stat(in(dir, "nope").view());
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().is(dstd::Condition::NotFound));
    CHECK(contains(missing.error().message(), "nope"));
    CHECK(contains(missing.error().message(), "No such file"));

    const auto too_long = dfs::stat(in(dir, dstd::String(300, 'a')).view());
    REQUIRE_FALSE(too_long.has_value());
    CHECK(too_long.error().is(dstd::Condition::InvalidArgument));

    const auto over_path_max =
        dfs::stat(in(dir, dstd::String(5000, 'b')).view());
    REQUIRE_FALSE(over_path_max.has_value());
    CHECK(over_path_max.error().is(dstd::Condition::InvalidArgument));

    const auto on_file = dfs::Dir::open(in(dir, "nope").view());
    REQUIRE_FALSE(on_file.has_value());
    CHECK(on_file.error().is(dstd::Condition::NotFound));
}

TEST_CASE("exists is false only for a missing entry") {
    ScopedTestDir dir("dstd_fs_exists");
    dir.create_file("a.txt", "x");

    CHECK(dfs::exists(in(dir, "a.txt").view()).value());
    CHECK(dfs::exists(in(dir, ".").view()).value());
    CHECK_FALSE(dfs::exists(in(dir, "nope").view()).value());
    CHECK_FALSE(dfs::exists(in(dir, "a.txt/child").view()).value());

    if (running_as_root()) return;
    dir.create_file("locked/inner.txt", "x");
    const Path locked = in(dir, "locked");
    REQUIRE(::chmod(locked.c_str(), 0) == 0);
    const auto denied = dfs::exists(in(dir, "locked/inner.txt").view());
    ::chmod(locked.c_str(), 0700);
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().is(dstd::Condition::Io));
    CHECK(contains(denied.error().message(), "Permission denied"));
}

TEST_CASE("create_directory and create_directories") {
    ScopedTestDir dir("dstd_fs_mkdir");

    const Path nested = in(dir, "a/b/c");
    const auto made = dfs::create_directories(nested.view());
    REQUIRE(made.has_value());
    CHECK(*made);
    CHECK(dfs::stat(nested.view()).value().kind == dfs::FileKind::Directory);

    const auto again = dfs::create_directories(nested.view());
    REQUIRE(again.has_value());
    CHECK_FALSE(*again);

    const auto single = dfs::create_directory(in(dir, "d").view());
    REQUIRE(single.has_value());
    CHECK(*single);
    const auto single_again = dfs::create_directory(in(dir, "d").view());
    REQUIRE(single_again.has_value());
    CHECK_FALSE(*single_again);

    dir.create_file("a/file.txt", "x");
    const auto over_file =
        dfs::create_directories(in(dir, "a/file.txt/under").view());
    REQUIRE_FALSE(over_file.has_value());
    CHECK(over_file.error().is(dstd::Condition::Io));

    const auto file_target =
        dfs::create_directory(in(dir, "a/file.txt").view());
    REQUIRE_FALSE(file_target.has_value());
    CHECK(file_target.error().is(dstd::Condition::Io));
}

TEST_CASE("remove takes a file, a link or an empty directory") {
    ScopedTestDir dir("dstd_fs_remove");
    dir.create_file("a.txt", "x");
    dir.create_dir("emptydir");
    dir.create_file("full/b.txt", "x");

    CHECK(*dfs::remove(in(dir, "a.txt").view()));
    CHECK_FALSE(dfs::exists(in(dir, "a.txt").view()).value());
    CHECK_FALSE(*dfs::remove(in(dir, "a.txt").view()));
    CHECK(*dfs::remove(in(dir, "emptydir").view()));

    const auto busy = dfs::remove(in(dir, "full").view());
    REQUIRE_FALSE(busy.has_value());
    CHECK(busy.error().is(dstd::Condition::Io));
    CHECK(dfs::exists(in(dir, "full/b.txt").view()).value());
}

TEST_CASE("remove_all counts what it removed and never follows a link") {
    ScopedTestDir dir("dstd_fs_remove_all");
    dir.create_file("root/sub/a.txt", "x");
    dir.create_file("root/b.txt", "x");
    dir.create_file("root/sub/nested/c.txt", "x");
    dir.create_file("keep/d.txt", "x");
    const dstd::String link = dir.file("root/link");
    REQUIRE(::symlink(dir.file("keep").c_str(), link.c_str()) == 0);

    const auto removed = dfs::remove_all(in(dir, "root").view());
    REQUIRE(removed.has_value());
    CHECK(*removed == 7);
    CHECK_FALSE(dfs::exists(in(dir, "root").view()).value());
    CHECK(dfs::exists(in(dir, "keep/d.txt").view()).value());

    const auto missing = dfs::remove_all(in(dir, "root").view());
    REQUIRE(missing.has_value());
    CHECK(*missing == 0);

    dir.create_file("one.txt", "x");
    const auto single = dfs::remove_all(in(dir, "one.txt").view());
    REQUIRE(single.has_value());
    CHECK(*single == 1);
}

TEST_CASE("rename") {
    ScopedTestDir dir("dstd_fs_rename");
    dir.create_file("a.txt", "hello");

    REQUIRE(dfs::rename(in(dir, "a.txt").view(), in(dir, "b.txt").view())
                .has_value());
    CHECK_FALSE(dfs::exists(in(dir, "a.txt").view()).value());
    CHECK(read_text(in(dir, "b.txt")) == "hello");

    const auto missing =
        dfs::rename(in(dir, "nope").view(), in(dir, "c.txt").view());
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().is(dstd::Condition::NotFound));
}

TEST_CASE("copy_file honours the mode") {
    ScopedTestDir dir("dstd_fs_copy");
    dir.create_file("from.txt", "hello");
    dir.create_file("target.txt", "old");

    REQUIRE(dfs::copy_file(in(dir, "from.txt").view(), in(dir, "copy").view(),
                           dfs::CopyMode::FailIfExists)
                .has_value());
    CHECK(read_text(in(dir, "copy")) == "hello");

    const auto twice =
        dfs::copy_file(in(dir, "from.txt").view(), in(dir, "copy").view(),
                       dfs::CopyMode::FailIfExists);
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().is(dstd::Condition::Io));
    CHECK(read_text(in(dir, "copy")) == "hello");

    REQUIRE(dfs::copy_file(in(dir, "from.txt").view(),
                           in(dir, "target.txt").view(),
                           dfs::CopyMode::Overwrite)
                .has_value());
    CHECK(read_text(in(dir, "target.txt")) == "hello");

    const auto from_missing =
        dfs::copy_file(in(dir, "nope").view(), in(dir, "other").view(),
                       dfs::CopyMode::FailIfExists);
    REQUIRE_FALSE(from_missing.has_value());
    CHECK(from_missing.error().is(dstd::Condition::NotFound));
}

TEST_CASE("Dir lists names and kinds without a stat") {
    ScopedTestDir dir("dstd_fs_dir");
    dir.create_file("root/a.txt", "hello");
    dir.create_file("root/b.txt", "x");
    dir.create_dir("root/sub");
    REQUIRE(::symlink("a.txt", dir.file("root/link").c_str()) == 0);

    auto handle = dfs::Dir::open(in(dir, "root").view());
    REQUIRE(handle.has_value());
    CHECK(handle->valid());

    auto entries = handle->entries();
    REQUIRE(entries.has_value());
    dstd::Map<dstd::String, dfs::FileKind> seen;
    for (const dfs::DirEntry& entry : *entries) {
        seen.emplace(dstd::String(entry.name), entry.kind);
    }
    CHECK(entries->error() == 0);
    REQUIRE(seen.size() == 4);
    CHECK(seen.count(".") == 0);
    CHECK(seen.count("..") == 0);
    CHECK(seen["a.txt"] == dfs::FileKind::File);
    CHECK(seen["b.txt"] == dfs::FileKind::File);
    CHECK(seen["sub"] == dfs::FileKind::Directory);
    CHECK(seen["link"] == dfs::FileKind::Symlink);

    const auto info = handle->stat("a.txt");
    REQUIRE(info.has_value());
    CHECK(info->size == 5);
    CHECK(info->kind == dfs::FileKind::File);

    const auto link_info = handle->lstat("link");
    REQUIRE(link_info.has_value());
    CHECK(link_info->kind == dfs::FileKind::Symlink);
    const auto followed = handle->stat("link");
    REQUIRE(followed.has_value());
    CHECK(followed->kind == dfs::FileKind::File);

    auto sub = handle->open_dir("sub");
    REQUIRE(sub.has_value());
    CHECK(sub->valid());
    auto nested = sub->entries();
    REQUIRE(nested.has_value());
    CHECK(nested->begin() == nested->end());

    auto opened = handle->open_file("a.txt", O_RDONLY);
    REQUIRE(opened.has_value());
    char buffer[8] = {};
    CHECK(::read(opened->get(), buffer, sizeof buffer) == 5);
    CHECK(dstd::String(buffer, 5) == "hello");

    const auto on_file = dfs::Dir::open(in(dir, "root/a.txt").view());
    REQUIRE_FALSE(on_file.has_value());
    CHECK(on_file.error().is(dstd::Condition::NotFound));
}

TEST_CASE("walk is depth first and visits a parent before its children") {
    ScopedTestDir dir("dstd_fs_walk");
    dir.create_file("root/a.txt", "x");
    dir.create_file("root/sub/b.txt", "x");
    dir.create_file("root/sub/nested/c.txt", "x");

    dstd::Vector<dstd::String> visited;
    dstd::Map<dstd::String, dfs::FileKind> kinds;
    const auto walked = dfs::walk(in(dir, "root").view(),
                                  [&](PathView path, dfs::FileKind kind) {
                                      visited.emplace_back(path.str());
                                      kinds[dstd::String(path.str())] = kind;
                                      return dfs::WalkAction::Continue;
                                  });
    REQUIRE(walked.has_value());
    CHECK(*walked == 6);
    CHECK(visited.size() == 6);
    REQUIRE_FALSE(visited.empty());
    CHECK(visited.front() == dir.file("root"));
    CHECK(dstd::Set<dstd::String>(visited.begin(), visited.end()).size() == 6);
    CHECK(kinds[dir.file("root/sub/nested/c.txt")] == dfs::FileKind::File);
    CHECK(kinds[dir.file("root/sub")] == dfs::FileKind::Directory);

    dstd::Map<dstd::String, std::size_t> position;
    for (std::size_t at = 0; at < visited.size(); ++at) {
        position[visited[at]] = at;
    }
    for (const dstd::String& path : visited) {
        const dstd::String parent(PathView(path).parent().str());
        if (position.count(parent) != 0) {
            CHECK(position[parent] < position[path]);
        }
    }
}

TEST_CASE("walk honours SkipDir and Stop") {
    ScopedTestDir dir("dstd_fs_walk_actions");
    dir.create_file("root/a.txt", "x");
    dir.create_file("root/sub/b.txt", "x");

    dstd::Vector<dstd::String> visited;
    const auto skipped = dfs::walk(
        in(dir, "root").view(), [&](PathView path, dfs::FileKind kind) {
            visited.emplace_back(path.str());
            const bool is_sub = kind == dfs::FileKind::Directory &&
                                PathView(path).filename() == "sub";
            return is_sub ? dfs::WalkAction::SkipDir
                          : dfs::WalkAction::Continue;
        });
    REQUIRE(skipped.has_value());
    CHECK(*skipped == 3);
    CHECK(std::find(visited.begin(), visited.end(), dir.file("root/a.txt")) !=
          visited.end());
    CHECK(std::find(visited.begin(), visited.end(),
                    dir.file("root/sub/b.txt")) == visited.end());

    int visits = 0;
    const auto stopped =
        dfs::walk(in(dir, "root").view(), [&](PathView, dfs::FileKind) {
            ++visits;
            return visits == 2 ? dfs::WalkAction::Stop
                               : dfs::WalkAction::Continue;
        });
    REQUIRE(stopped.has_value());
    CHECK(*stopped == 2);
}

TEST_CASE("walk reports a file and refuses a missing root") {
    ScopedTestDir dir("dstd_fs_walk_root");
    dir.create_file("a.txt", "x");

    dstd::Vector<dfs::FileKind> kinds;
    const auto walked =
        dfs::walk(in(dir, "a.txt").view(), [&](PathView, dfs::FileKind kind) {
            kinds.push_back(kind);
            return dfs::WalkAction::Continue;
        });
    REQUIRE(walked.has_value());
    CHECK(*walked == 1);
    REQUIRE(kinds.size() == 1);
    CHECK(kinds.front() == dfs::FileKind::File);

    const auto missing = dfs::walk(
        in(dir, "nope").view(),
        [](PathView, dfs::FileKind) { return dfs::WalkAction::Continue; });
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().is(dstd::Condition::NotFound));
}

TEST_CASE("walk does not follow a symlinked directory") {
    ScopedTestDir dir("dstd_fs_walk_link");
    dir.create_file("root/sub/b.txt", "x");
    REQUIRE(::symlink(dir.file("root/sub").c_str(),
                      dir.file("root/link").c_str()) == 0);

    dstd::Vector<dstd::String> visited;
    const auto walked =
        dfs::walk(in(dir, "root").view(), [&](PathView path, dfs::FileKind) {
            visited.emplace_back(path.str());
            return dfs::WalkAction::Continue;
        });
    REQUIRE(walked.has_value());
    CHECK(*walked == 4);
    CHECK(std::find(visited.begin(), visited.end(), dir.file("root/link")) !=
          visited.end());
    const auto twice =
        std::count(visited.begin(), visited.end(), dir.file("root/sub/b.txt"));
    CHECK(twice == 1);
}

TEST_CASE("absolute and canonical") {
    ScopedTestDir dir("dstd_fs_abs");
    dir.create_file("target.txt", "x");
    REQUIRE(::symlink("target.txt", dir.file("link.txt").c_str()) == 0);

    const dstd::String expected =
        dstd::String(dstd::fs::current_path().value().str()) + "/child";
    const auto relative_to_cwd = dfs::absolute(PathView("child"));
    REQUIRE(relative_to_cwd.has_value());
    CHECK(relative_to_cwd->str() == expected);

    const auto absolute_path = dfs::absolute(in(dir, "target.txt").view());
    REQUIRE(absolute_path.has_value());
    CHECK(absolute_path->str() == dir.file("target.txt"));

    const auto resolved = dfs::canonical(in(dir, "link.txt").view());
    REQUIRE(resolved.has_value());
    CHECK(resolved->str() ==
          dstd::fs::canonical(dir.file("target.txt")).value().str());

    const auto dot = dfs::canonical(in(dir, ".").view());
    REQUIRE(dot.has_value());
    CHECK(dot->str() == dstd::fs::canonical(dir.str()).value().str());

    const auto missing = dfs::canonical(in(dir, "nope").view());
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().is(dstd::Condition::NotFound));
}

TEST_CASE("space and temp_directory") {
    ScopedTestDir dir("dstd_fs_space");

    const auto info = dfs::space(in(dir, ".").view());
    REQUIRE(info.has_value());
    CHECK(info->capacity > 0);
    CHECK(info->free <= info->capacity);
    CHECK(info->available <= info->free);

    const Path temp = dfs::temp_directory();
    CHECK_FALSE(temp.empty());
    CHECK(PathView(temp).is_absolute());
    CHECK(dfs::exists(temp.view()).value());
    const char* given = std::getenv("TMPDIR");
    if (given != nullptr && given[0] != '\0') {
        CHECK(temp.str() == given);
    }
}

TEST_CASE("a trailing separator never follows a symlinked directory") {
    ScopedTestDir dir("dstd_fs_trailing_sep");
    dir.create_file("victim/keep", "x");
    dir.create_file("victim/sub/deep", "x");
    REQUIRE(::symlink(dir.file("victim").c_str(), dir.file("lnk").c_str()) ==
            0);
    const Path link(dir.file("lnk") + "/");

    dstd::Vector<dstd::String> visited;
    const auto walked =
        dfs::walk(link.view(), [&](PathView path, dfs::FileKind kind) {
            visited.emplace_back(path.str());
            CHECK(kind == dfs::FileKind::Symlink);
            return dfs::WalkAction::Continue;
        });
    REQUIRE(walked.has_value());
    CHECK(*walked == 1);
    CHECK(visited.size() == 1);

    const auto removed = dfs::remove_all(link.view());
    REQUIRE(removed.has_value());
    CHECK(*removed == 1);
    CHECK(::access(dir.file("lnk").c_str(), F_OK) != 0);
    CHECK(::access(dir.file("victim/keep").c_str(), F_OK) == 0);
    CHECK(::access(dir.file("victim/sub/deep").c_str(), F_OK) == 0);

    REQUIRE(::symlink(dir.file("victim").c_str(), dir.file("lnk2").c_str()) ==
            0);
    (void)dfs::remove(Path(dir.file("lnk2") + "/").view());
    CHECK(::access(dir.file("victim/keep").c_str(), F_OK) == 0);
}

TEST_CASE("Dir::entries can be listed again and listings do not interfere") {
    ScopedTestDir dir("dstd_fs_entries_twice");
    dir.create_file("root/a", "x");
    dir.create_file("root/b", "x");
    dir.create_file("root/c", "x");
    auto handle = dfs::Dir::open(in(dir, "root").view());
    REQUIRE(handle.has_value());

    const auto count = [](dfs::DirEntries& listing) {
        std::size_t seen = 0;
        for (const dfs::DirEntry& entry : listing) {
            (void)entry;
            ++seen;
        }
        return seen;
    };
    auto first = handle->entries();
    REQUIRE(first.has_value());
    CHECK(count(*first) == 3);
    auto second = handle->entries();
    REQUIRE(second.has_value());
    CHECK(count(*second) == 3);

    auto left = handle->entries();
    auto right = handle->entries();
    REQUIRE(left.has_value());
    REQUIRE(right.has_value());
    std::size_t left_seen = 0;
    std::size_t right_seen = 0;
    auto li = left->begin();
    auto ri = right->begin();
    while (li != left->end() || ri != right->end()) {
        if (li != left->end()) {
            ++left_seen;
            ++li;
        }
        if (ri != right->end()) {
            ++right_seen;
            ++ri;
        }
    }
    CHECK(left_seen == 3);
    CHECK(right_seen == 3);
}

TEST_CASE("copy_file refuses to copy a file onto itself") {
    ScopedTestDir dir("dstd_fs_copy_self");
    dir.create_file("f", "hello");
    REQUIRE(::symlink(dir.file("f").c_str(), dir.file("sl").c_str()) == 0);
    REQUIRE(::link(dir.file("f").c_str(), dir.file("hard").c_str()) == 0);

    const auto same = dfs::copy_file(in(dir, "f").view(), in(dir, "f").view(),
                                     dfs::CopyMode::Overwrite);
    REQUIRE_FALSE(same.has_value());
    CHECK(same.error().is(dstd::Condition::InvalidArgument));
    CHECK(read_text(in(dir, "f")) == "hello");

    const auto via_link = dfs::copy_file(
        in(dir, "f").view(), in(dir, "sl").view(), dfs::CopyMode::Overwrite);
    REQUIRE_FALSE(via_link.has_value());
    CHECK(via_link.error().is(dstd::Condition::InvalidArgument));
    CHECK(read_text(in(dir, "f")) == "hello");

    const auto via_hard = dfs::copy_file(
        in(dir, "hard").view(), in(dir, "f").view(), dfs::CopyMode::Overwrite);
    REQUIRE_FALSE(via_hard.has_value());
    CHECK(via_hard.error().is(dstd::Condition::InvalidArgument));
    CHECK(read_text(in(dir, "f")) == "hello");
}

TEST_CASE("a failed copy leaves no target this call created") {
    ScopedTestDir dir("dstd_fs_copy_cleanup");
    dir.create_dir("srcdir");
    const auto failed =
        dfs::copy_file(in(dir, "srcdir").view(), in(dir, "target").view(),
                       dfs::CopyMode::FailIfExists);
    CHECK_FALSE(failed.has_value());
    CHECK(::access(dir.file("target").c_str(), F_OK) != 0);

    const auto failed_overwrite =
        dfs::copy_file(in(dir, "srcdir").view(), in(dir, "target2").view(),
                       dfs::CopyMode::Overwrite);
    CHECK_FALSE(failed_overwrite.has_value());
    CHECK(::access(dir.file("target2").c_str(), F_OK) != 0);
}

TEST_CASE("walk and remove_all hold one descriptor per level") {
    ScopedTestDir dir("dstd_fs_deep");
    dstd::String relative = "root";
    for (int level = 0; level < 200; ++level) relative += "/d";
    REQUIRE(dfs::create_directories(in(dir, relative).view()).has_value());

    struct rlimit saved;
    if (::getrlimit(RLIMIT_NOFILE, &saved) != 0) return;
    struct rlimit lowered = saved;
    lowered.rlim_cur = 256;
    if (::setrlimit(RLIMIT_NOFILE, &lowered) != 0) return;

    const auto walked = dfs::walk(
        in(dir, "root").view(),
        [](PathView, dfs::FileKind) { return dfs::WalkAction::Continue; });
    const auto removed = dfs::remove_all(in(dir, "root").view());
    ::setrlimit(RLIMIT_NOFILE, &saved);

    REQUIRE(walked.has_value());
    CHECK(*walked == 201);
    REQUIRE(removed.has_value());
    CHECK(*removed == 201);
    CHECK(::access(dir.file("root").c_str(), F_OK) != 0);
}

TEST_CASE("is_directory and is_regular_file are false for a missing entry") {
    ScopedTestDir dir("dstd_fs_kinds");
    dir.create_file("a.txt", "x");
    dir.create_dir("sub");
    REQUIRE(::symlink("sub", dir.file("dlink").c_str()) == 0);
    REQUIRE(::symlink("a.txt", dir.file("flink").c_str()) == 0);

    CHECK(dfs::is_directory(in(dir, "sub").view()).value());
    CHECK_FALSE(dfs::is_directory(in(dir, "a.txt").view()).value());
    CHECK(dfs::is_directory(in(dir, "dlink").view()).value());
    CHECK_FALSE(dfs::is_directory(in(dir, "nope").view()).value());
    CHECK_FALSE(dfs::is_directory(in(dir, "a.txt/x").view()).value());

    CHECK(dfs::is_regular_file(in(dir, "a.txt").view()).value());
    CHECK(dfs::is_regular_file(in(dir, "flink").view()).value());
    CHECK_FALSE(dfs::is_regular_file(in(dir, "sub").view()).value());
    CHECK_FALSE(dfs::is_regular_file(in(dir, "nope").view()).value());

    if (!running_as_root()) {
        dir.create_dir("locked");
        dir.create_file("locked/inner", "x");
        REQUIRE(::chmod(dir.file("locked").c_str(), 0) == 0);
        const auto denied = dfs::is_directory(in(dir, "locked/inner").view());
        ::chmod(dir.file("locked").c_str(), 0700);
        REQUIRE_FALSE(denied.has_value());
        CHECK(denied.error().is(dstd::Condition::Io));
    }
}

TEST_CASE(
    "weakly_canonical resolves the existing prefix and appends the rest") {
    ScopedTestDir dir("dstd_fs_weak");
    dir.create_dir("real");
    REQUIRE(::symlink("real", dir.file("link").c_str()) == 0);
    const dstd::String base =
        dstd::String(dstd::fs::canonical(dir.str()).value().str());

    const auto existing = dfs::weakly_canonical(in(dir, "link").view());
    REQUIRE(existing.has_value());
    CHECK(existing->str() == base + "/real");

    const auto partial = dfs::weakly_canonical(in(dir, "link/a/b").view());
    REQUIRE(partial.has_value());
    CHECK(partial->str() == base + "/real/a/b");

    const auto dotdot = dfs::weakly_canonical(in(dir, "link/a/../b").view());
    REQUIRE(dotdot.has_value());
    CHECK(dotdot->str() == base + "/real/b");

    const auto trailing = dfs::weakly_canonical(in(dir, "link/a/").view());
    REQUIRE(trailing.has_value());
    CHECK(trailing->str() == base + "/real/a");

    const auto relative = dfs::weakly_canonical(PathView("no_such_dir/x"));
    REQUIRE(relative.has_value());
    CHECK(relative->str() ==
          dstd::String(dstd::fs::canonical(".").value().str()) +
              "/no_such_dir/x");

    const auto empty = dfs::weakly_canonical(PathView());
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("copy_tree copies files, directories and symlinks") {
    ScopedTestDir dir("dstd_fs_copy_tree");
    dir.create_dir("src");
    dir.create_dir("src/sub");
    dir.create_file("src/a.txt", "alpha");
    dir.create_file("src/sub/b.txt", "beta");
    REQUIRE(::symlink("a.txt", dir.file("src/ln").c_str()) == 0);

    REQUIRE(dfs::copy_tree(in(dir, "src").view(), in(dir, "dst").view(),
                           dfs::CopyMode::FailIfExists)
                .has_value());
    CHECK(read_text(in(dir, "dst/a.txt")) == "alpha");
    CHECK(read_text(in(dir, "dst/sub/b.txt")) == "beta");
    const auto link = dfs::lstat(in(dir, "dst/ln").view());
    REQUIRE(link.has_value());
    CHECK(link->kind == dfs::FileKind::Symlink);
    char target[64] = {};
    const auto got = ::readlink(dir.file("dst/ln").c_str(), target, 63);
    REQUIRE(got > 0);
    CHECK(dstd::String(target, static_cast<std::size_t>(got)) == "a.txt");

    const auto again =
        dfs::copy_tree(in(dir, "src").view(), in(dir, "dst").view(),
                       dfs::CopyMode::FailIfExists);
    CHECK_FALSE(again.has_value());

    dir.create_file("src/a.txt", "changed");
    REQUIRE(dfs::copy_tree(in(dir, "src").view(), in(dir, "dst").view(),
                           dfs::CopyMode::Overwrite)
                .has_value());
    CHECK(read_text(in(dir, "dst/a.txt")) == "changed");
    CHECK(read_text(in(dir, "dst/sub/b.txt")) == "beta");

    REQUIRE(dfs::copy_tree(in(dir, "src/a.txt").view(),
                           in(dir, "single").view(),
                           dfs::CopyMode::FailIfExists)
                .has_value());
    CHECK(read_text(in(dir, "single")) == "changed");

    const auto missing =
        dfs::copy_tree(in(dir, "nope").view(), in(dir, "out").view(),
                       dfs::CopyMode::FailIfExists);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().is(dstd::Condition::NotFound));
}

TEST_CASE("read_file returns the whole file") {
    ScopedTestDir dir("dstd_fs_read_file");
    dir.create_file("a.txt", "hello\nworld\n");
    dir.create_file("empty.txt", "");
    dstd::String big(200000, 'x');
    dir.create_file("big.txt", big);

    const auto text = dfs::read_file(in(dir, "a.txt").view());
    REQUIRE(text.has_value());
    CHECK(*text == "hello\nworld\n");

    const auto empty = dfs::read_file(in(dir, "empty.txt").view());
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    const auto large = dfs::read_file(in(dir, "big.txt").view());
    REQUIRE(large.has_value());
    CHECK(large->size() == big.size());

    const auto missing = dfs::read_file(in(dir, "nope").view());
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().is(dstd::Condition::NotFound));

    const auto directory = dfs::read_file(in(dir, ".").view());
    CHECK_FALSE(directory.has_value());
}

TEST_CASE("absolute only joins the current directory") {
    char cwd[4096];
    REQUIRE(::getcwd(cwd, sizeof cwd) != nullptr);
    const dstd::String base(cwd);

    const auto dotted = dfs::absolute(PathView("a/../b"));
    REQUIRE(dotted.has_value());
    CHECK(dotted->str() == base + "/a/../b");

    const auto trailing = dfs::absolute(PathView("x/"));
    REQUIRE(trailing.has_value());
    CHECK(trailing->str() == base + "/x/");

    const auto kept = dfs::absolute(PathView("/p/./q/../r"));
    REQUIRE(kept.has_value());
    CHECK(kept->str() == "/p/./q/../r");
}

TEST_CASE("weakly_canonical ends when the current directory is gone") {
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        char templ[] = "/tmp/dstd_fs_gone_XXXXXX";
        if (::mkdtemp(templ) == nullptr) ::_exit(2);
        if (::chdir(templ) != 0) ::_exit(3);
        if (::rmdir(templ) != 0) ::_exit(4);
        const auto result = dfs::weakly_canonical(PathView("x/y"));
        ::_exit(result.has_value() ? 5 : 0);
    }
    int status = 0;
    bool done = false;
    for (int waited = 0; waited < 50; ++waited) {
        if (::waitpid(child, &status, WNOHANG) == child) {
            done = true;
            break;
        }
        ::usleep(100000);
    }
    if (!done) {
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
    }
    REQUIRE(done);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("set_permissions replaces the mode bits it is given") {
    ScopedTestDir dir("dstd_fs_permissions");
    dir.create_file("a.txt", "x");
    const Path file = in(dir, "a.txt");

    REQUIRE(dfs::set_permissions(file.view(), 0600).has_value());
    const auto first = dfs::stat(file.view());
    REQUIRE(first.has_value());
    CHECK((first->mode & 07777) == 0600);

    REQUIRE(dfs::set_permissions(file.view(), 0755).has_value());
    const auto second = dfs::stat(file.view());
    REQUIRE(second.has_value());
    CHECK((second->mode & 07777) == 0755);

    if (!running_as_root()) {
        REQUIRE(dfs::set_permissions(file.view(), 0).has_value());
        const auto denied = dfs::read_file(file.view());
        REQUIRE(dfs::set_permissions(file.view(), 0600).has_value());
        REQUIRE_FALSE(denied.has_value());
        CHECK(denied.error().is(dstd::Condition::Io));
    }

    const auto missing = dfs::set_permissions(in(dir, "nope").view(), 0600);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().is(dstd::Condition::NotFound));

    const auto too_wide = dfs::set_permissions(file.view(), 0100000);
    REQUIRE_FALSE(too_wide.has_value());
    CHECK(too_wide.error().is(dstd::Condition::InvalidArgument));
}

TEST_CASE("resize_file truncates and extends a file") {
    ScopedTestDir dir("dstd_fs_resize");
    dir.create_file("a.txt", "hello");
    const Path file = in(dir, "a.txt");

    REQUIRE(dfs::resize_file(file.view(), 2).has_value());
    CHECK(dfs::file_size(file.view()).value() == 2);
    CHECK(read_text(file) == "he");

    REQUIRE(dfs::resize_file(file.view(), 6).has_value());
    CHECK(dfs::file_size(file.view()).value() == 6);
    CHECK(read_text(file) == dstd::String("he\0\0\0\0", 6));

    REQUIRE(dfs::resize_file(file.view(), 0).has_value());
    CHECK(read_text(file).empty());

    const auto missing = dfs::resize_file(in(dir, "nope").view(), 1);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().is(dstd::Condition::NotFound));

    const auto directory = dfs::resize_file(in(dir, ".").view(), 1);
    REQUIRE_FALSE(directory.has_value());
    CHECK(directory.error().is(dstd::Condition::Io));
}

TEST_CASE("current_path names the working directory") {
    char cwd[4096];
    REQUIRE(::getcwd(cwd, sizeof cwd) != nullptr);
    const Path base(cwd);

    const auto current = dfs::current_path();
    REQUIRE(current.has_value());
    CHECK(current->str() == base.str());
    CHECK(current->view().is_absolute());
    CHECK(dfs::is_directory(current->view()).value());

    ScopedTestDir dir("dstd_fs_current_path");
    const auto resolved = dfs::canonical(in(dir, ".").view());
    REQUIRE(resolved.has_value());
    REQUIRE(::chdir(dir.str().c_str()) == 0);
    const auto moved = dfs::current_path();
    REQUIRE(::chdir(base.c_str()) == 0);
    REQUIRE(moved.has_value());
    CHECK(moved->str() == resolved->str());
}

TEST_CASE("set_mtime_ns sets the time that mtime_ns reports") {
    ScopedTestDir dir("dstd_fs_mtime");
    dir.create_file("a.txt", "x");
    const Path file = in(dir, "a.txt");
    dir.create_dir("sub");

    struct stat before;
    REQUIRE(::stat(file.c_str(), &before) == 0);
    const std::uint64_t target =
        dfs::mtime_ns(file.view()).value() + 90 * 1000000000ULL;
    REQUIRE(dfs::set_mtime_ns(file.view(), target).has_value());
    CHECK(dfs::mtime_ns(file.view()).value() == target);
    struct stat after;
    REQUIRE(::stat(file.c_str(), &after) == 0);
    CHECK(after.st_atime == before.st_atime);

    REQUIRE(dfs::set_mtime_ns(in(dir, "sub").view(), 123456789ULL).has_value());
    CHECK(dfs::mtime_ns(in(dir, "sub").view()).value() == 123456789ULL);

    const auto missing = dfs::set_mtime_ns(in(dir, "nope").view(), target);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().is(dstd::Condition::NotFound));
}

TEST_CASE("create_symlink makes a link to a target that need not exist") {
    ScopedTestDir dir("dstd_fs_symlink");
    dir.create_file("a.txt", "x");
    const Path link = in(dir, "link");

    REQUIRE(dfs::create_symlink(PathView("a.txt"), link.view()).has_value());
    const auto info = dfs::lstat(link.view());
    REQUIRE(info.has_value());
    CHECK(info->kind == dfs::FileKind::Symlink);
    CHECK(dfs::file_size(link.view()).value() == 1);
    char text[64] = {};
    const auto got = ::readlink(link.c_str(), text, 63);
    REQUIRE(got > 0);
    CHECK(dstd::String(text, static_cast<std::size_t>(got)) == "a.txt");

    REQUIRE(dfs::create_symlink(PathView("nope"), in(dir, "dangling").view())
                .has_value());
    CHECK_FALSE(dfs::exists(in(dir, "dangling").view()).value());
    const auto dangling = dfs::lstat(in(dir, "dangling").view());
    REQUIRE(dangling.has_value());
    CHECK(dangling->kind == dfs::FileKind::Symlink);

    REQUIRE(dfs::create_symlink(in(dir, ".").view(), in(dir, "dirlink").view())
                .has_value());
    CHECK(dfs::is_directory(in(dir, "dirlink").view()).value());

    const auto again = dfs::create_symlink(PathView("a.txt"), link.view());
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().is(dstd::Condition::Io));
}

TEST_CASE("copy_tree keeps file and directory modes") {
    ScopedTestDir dir("dstd_fs_copy_modes");
    dir.create_dir("src");
    dir.create_dir("src/private");
    dir.create_file("src/secret.txt", "s");
    dir.create_file("src/private/inner.txt", "i");
    REQUIRE(::chmod(dir.file("src/secret.txt").c_str(), 0600) == 0);
    REQUIRE(::chmod(dir.file("src/private").c_str(), 0700) == 0);
    REQUIRE(::chmod(dir.file("src/private/inner.txt").c_str(), 0640) == 0);

    REQUIRE(dfs::copy_tree(in(dir, "src").view(), in(dir, "dst").view(),
                           dfs::CopyMode::FailIfExists)
                .has_value());
    const auto mode_of = [&](const char* name) {
        return dfs::stat(in(dir, name).view())->mode & 07777;
    };
    CHECK(mode_of("dst/secret.txt") == 0600);
    CHECK(mode_of("dst/private") == 0700);
    CHECK(mode_of("dst/private/inner.txt") == 0640);
}
