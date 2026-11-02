// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace dftracer::utils;
using dftu_utils_test::ScopedTestDir;
using dstd::Path;
using dstd::PathView;

namespace dfs = dftracer::utils::dstd::fs;

namespace {

Path in(const ScopedTestDir& dir, const std::string& name) {
    return Path(dir.file(name));
}

std::string read_text(const Path& path) {
    std::ifstream stream(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

bool running_as_root() { return ::geteuid() == 0; }

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
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

    const std::string link = dir.file("link");
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

    const auto too_long = dfs::stat(in(dir, std::string(300, 'a')).view());
    REQUIRE_FALSE(too_long.has_value());
    CHECK(too_long.error().is(dstd::Condition::InvalidArgument));

    const auto over_path_max =
        dfs::stat(in(dir, std::string(5000, 'b')).view());
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
    const std::string link = dir.file("root/link");
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
    std::map<std::string, dfs::FileKind> seen;
    for (const dfs::DirEntry& entry : *entries) {
        seen.emplace(std::string(entry.name), entry.kind);
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
    CHECK(std::string(buffer, 5) == "hello");

    const auto on_file = dfs::Dir::open(in(dir, "root/a.txt").view());
    REQUIRE_FALSE(on_file.has_value());
    CHECK(on_file.error().is(dstd::Condition::NotFound));
}

TEST_CASE("walk is depth first and visits a parent before its children") {
    ScopedTestDir dir("dstd_fs_walk");
    dir.create_file("root/a.txt", "x");
    dir.create_file("root/sub/b.txt", "x");
    dir.create_file("root/sub/nested/c.txt", "x");

    std::vector<std::string> visited;
    std::map<std::string, dfs::FileKind> kinds;
    const auto walked = dfs::walk(in(dir, "root").view(),
                                  [&](PathView path, dfs::FileKind kind) {
                                      visited.emplace_back(path.str());
                                      kinds[std::string(path.str())] = kind;
                                      return dfs::WalkAction::Continue;
                                  });
    REQUIRE(walked.has_value());
    CHECK(*walked == 6);
    CHECK(visited.size() == 6);
    REQUIRE_FALSE(visited.empty());
    CHECK(visited.front() == dir.file("root"));
    CHECK(std::set<std::string>(visited.begin(), visited.end()).size() == 6);
    CHECK(kinds[dir.file("root/sub/nested/c.txt")] == dfs::FileKind::File);
    CHECK(kinds[dir.file("root/sub")] == dfs::FileKind::Directory);

    std::map<std::string, std::size_t> position;
    for (std::size_t at = 0; at < visited.size(); ++at) {
        position[visited[at]] = at;
    }
    for (const std::string& path : visited) {
        const std::string parent(PathView(path).parent().str());
        if (position.count(parent) != 0) {
            CHECK(position[parent] < position[path]);
        }
    }
}

TEST_CASE("walk honours SkipDir and Stop") {
    ScopedTestDir dir("dstd_fs_walk_actions");
    dir.create_file("root/a.txt", "x");
    dir.create_file("root/sub/b.txt", "x");

    std::vector<std::string> visited;
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

    std::vector<dfs::FileKind> kinds;
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

    std::vector<std::string> visited;
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

    const std::string expected =
        std::filesystem::current_path().string() + "/child";
    const auto relative_to_cwd = dfs::absolute(PathView("child"));
    REQUIRE(relative_to_cwd.has_value());
    CHECK(relative_to_cwd->str() == expected);

    const auto absolute_path = dfs::absolute(in(dir, "target.txt").view());
    REQUIRE(absolute_path.has_value());
    CHECK(absolute_path->str() == dir.file("target.txt"));

    const auto resolved = dfs::canonical(in(dir, "link.txt").view());
    REQUIRE(resolved.has_value());
    CHECK(resolved->str() ==
          std::filesystem::canonical(dir.file("target.txt")).string());

    const auto dot = dfs::canonical(in(dir, ".").view());
    REQUIRE(dot.has_value());
    CHECK(dot->str() == std::filesystem::canonical(dir.str()).string());

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
    CHECK(PathView(temp.view()).is_absolute());
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

    std::vector<std::string> visited;
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
    std::string relative = "root";
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
