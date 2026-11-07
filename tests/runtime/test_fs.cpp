// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <sys/stat.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>

using namespace dftracer::utils;
using namespace dftracer::utils::runtime::fs;

namespace {

using dstd::fs::FileKind;

template <typename Fn>
void run_in_scope(Fn fn) {
    Runtime rt;
    rt.block_on(coro::named("fs test", coro::with_scope(std::move(fn))));
}

void write_file_at(const dstd::Path& path, dstd::StringView text) {
    const auto parent = dstd::fs::create_directories(path.view().parent());
    REQUIRE(parent.has_value());
    const auto written = dstd::fs::write_file(path.view(), text);
    REQUIRE(written.has_value());
}

dstd::Vector<dstd::String> names_below(const dstd::Path& root,
                                       const dstd::Vector<FoundFile>& files) {
    dstd::Vector<dstd::String> names;
    for (const FoundFile& file : files) {
        dstd::StringView text = file.path.str();
        text.remove_prefix(root.str().size() + 1);
        names.push_back(dstd::String(text));
    }
    return names;
}

dstd::Vector<dstd::String> expected_names(
    std::initializer_list<const char*> names) {
    dstd::Vector<dstd::String> out;
    for (const char* name : names) out.push_back(dstd::String(name));
    return out;
}

// a.pfw.gz, b.txt, sub/c.pfw.gz, sub/deep/d.pfw.gz, split/e.pfw.gz,
// .dftindex/f.pfw.gz
dstd::Path make_tree(dftu_utils_test::ScopedTestDir& dir) {
    const dstd::Path root = dir.path() / "tree";
    write_file_at(root / "a.pfw.gz", "aaaa");
    write_file_at(root / "b.txt", "bb");
    write_file_at(root / "sub" / "c.pfw.gz", "c");
    write_file_at(root / "sub" / "deep" / "d.pfw.gz", "dd");
    write_file_at(root / "split" / "e.pfw.gz", "e");
    write_file_at(root / ".dftindex" / "f.pfw.gz", "f");
    return root;
}

FindOptions trace_options() {
    FindOptions options;
    options.patterns.push_back(dstd::String("*.gz"));
    options.skip_dirs.push_back(dstd::String(".dftindex*"));
    options.skip_dirs.push_back(dstd::String("split"));
    return options;
}

dstd::Vector<dstd::String> find_names(
    const dstd::Path& root, std::initializer_list<const char*> patterns,
    bool recursive = false) {
    FindOptions options;
    for (const char* pattern : patterns) {
        options.patterns.push_back(dstd::String(pattern));
    }
    options.recursive = recursive;
    dstd::Vector<FoundFile> files;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        auto found = co_await find_all(scope, root, std::move(options));
        REQUIRE(found.has_value());
        files = std::move(*found);
    });
    return names_below(root, files);
}

}  // namespace

TEST_CASE("find_all - filter and skip and sort and stat fields") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = make_tree(dir);
    dstd::Vector<FoundFile> files;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        FindOptions options_found = trace_options();
        auto found = co_await find_all(scope, root, std::move(options_found));
        REQUIRE(found.has_value());
        files = std::move(*found);
    });
    CHECK(names_below(root, files) ==
          expected_names({"a.pfw.gz", "sub/c.pfw.gz", "sub/deep/d.pfw.gz"}));
    REQUIRE(files.size() == 3);
    CHECK(files[0].size == 4);
    const auto truth = dstd::fs::stat(files[0].path.view());
    REQUIRE(truth.has_value());
    CHECK(files[0].mtime_ns == truth->mtime_ns);
}

TEST_CASE("find_all - same files as a serial walk on a wide tree") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = dir.path() / "wide";
    for (int d = 0; d < 20; ++d) {
        for (int f = 0; f < 40; ++f) {
            write_file_at(root / dstd::to_string(d) / dstd::to_string(f), "x");
        }
    }
    dstd::Vector<dstd::String> serial;
    const auto walked =
        dstd::fs::walk(root, [&](dstd::PathView path, FileKind kind) {
            if (kind == FileKind::File)
                serial.push_back(dstd::String(path.str()));
            return dstd::fs::WalkAction::Continue;
        });
    REQUIRE(walked.has_value());
    std::sort(serial.begin(), serial.end());

    dstd::Vector<dstd::String> scanned;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        auto found = co_await find_all(scope, root);
        REQUIRE(found.has_value());
        for (const FoundFile& file : *found) {
            scanned.push_back(dstd::String(file.path.str()));
        }
    });
    CHECK(scanned.size() == 800);
    CHECK(scanned == serial);
}

TEST_CASE("find_all - recursive false stays in the root") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = make_tree(dir);
    dstd::Vector<FoundFile> files;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        FindOptions options = trace_options();
        options.recursive = false;
        auto found = co_await find_all(scope, root, std::move(options));
        REQUIRE(found.has_value());
        files = std::move(*found);
    });
    CHECK(names_below(root, files) == expected_names({"a.pfw.gz"}));
}

TEST_CASE("find_all - with_stat false leaves the fields at zero") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = make_tree(dir);
    dstd::Vector<FoundFile> files;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        FindOptions options = trace_options();
        options.with_stat = false;
        auto found = co_await find_all(scope, root, std::move(options));
        REQUIRE(found.has_value());
        files = std::move(*found);
    });
    REQUIRE(files.size() == 3);
    CHECK(files[0].size == 0);
    CHECK(files[0].mtime_ns == 0);
}

TEST_CASE("find_all - symlinks") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = dir.path() / "links";
    const dstd::Path real_file = dir.path() / "real.txt";
    const dstd::Path real_dir = dir.path() / "real_dir";
    write_file_at(real_file, "12345");
    write_file_at(real_dir / "inside.txt", "x");
    REQUIRE(dstd::fs::create_directories(root).has_value());
    REQUIRE(::symlink(real_file.c_str(), (root / "to_file").c_str()) == 0);
    REQUIRE(::symlink(real_dir.c_str(), (root / "to_dir").c_str()) == 0);
    REQUIRE(::symlink("/does/not/exist", (root / "dangling").c_str()) == 0);

    for (const bool with_stat : {true, false}) {
        dstd::Vector<FoundFile> files;
        run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            FindOptions options;
            options.with_stat = with_stat;
            auto found = co_await find_all(scope, root, std::move(options));
            REQUIRE(found.has_value());
            files = std::move(*found);
        });
        CHECK(names_below(root, files) == expected_names({"to_file"}));
        REQUIRE(files.size() == 1);
        CHECK(files[0].size == 5);
    }
}

TEST_CASE("find_all - errors of the root") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = make_tree(dir);
    dstd::Optional<dstd::Error> missing;
    dstd::Optional<dstd::Error> not_a_dir;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        const dstd::Path gone = root / "gone";
        auto first = co_await find_all(scope, gone);
        REQUIRE_FALSE(first.has_value());
        missing = first.error();
        const dstd::Path file = root / "b.txt";
        auto second = co_await find_all(scope, file);
        REQUIRE_FALSE(second.has_value());
        not_a_dir = second.error();
    });
    CHECK(missing->is(dstd::Condition::NotFound));
    CHECK(not_a_dir->is(dstd::Condition::InvalidArgument));
}

TEST_CASE("find_all - an unreadable folder ends it or is left out") {
    if (::geteuid() == 0) return;
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = make_tree(dir);
    const dstd::Path locked = root / "sub" / "deep";
    REQUIRE(::chmod(locked.c_str(), 0) == 0);

    dstd::Optional<dstd::Error> failure;
    dstd::Vector<FoundFile> files;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        FindOptions options_strict = trace_options();
        auto strict = co_await find_all(scope, root, std::move(options_strict));
        REQUIRE_FALSE(strict.has_value());
        failure = strict.error();
        FindOptions options = trace_options();
        options.skip_unreadable = true;
        auto lenient = co_await find_all(scope, root, std::move(options));
        REQUIRE(lenient.has_value());
        files = std::move(*lenient);
    });
    REQUIRE(::chmod(locked.c_str(), 0755) == 0);
    CHECK(failure->is(dstd::Condition::Io));
    CHECK(names_below(root, files) ==
          expected_names({"a.pfw.gz", "sub/c.pfw.gz"}));
}

TEST_CASE("find - a stream gives each file once and can be dropped") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = dir.path() / "many";
    for (int d = 0; d < 30; ++d) {
        for (int f = 0; f < 200; ++f) {
            write_file_at(root / dstd::to_string(d) / dstd::to_string(f), "x");
        }
    }
    std::size_t total = 0;
    std::size_t before_drop = 0;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        {
            auto stream = find(scope, root);
            for (;;) {
                auto item = co_await stream.next();
                if (!item) break;
                REQUIRE(item->has_value());
                ++total;
            }
        }
        // Dropped with producers parked on a full channel: the join below
        // must still end.
        auto stream = find(scope, root);
        for (int i = 0; i < 3; ++i) {
            auto item = co_await stream.next();
            REQUIRE(item.has_value());
            ++before_drop;
        }
    });
    CHECK(total == 6000);
    CHECK(before_drop == 3);
}

TEST_CASE("walk - reports folders and symlinks without entering a link") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = make_tree(dir);
    const dstd::Path outside = dir.path() / "outside";
    write_file_at(outside / "hidden.txt", "x");
    REQUIRE(::symlink(outside.c_str(), (root / "link").c_str()) == 0);

    dstd::Vector<dstd::String> names;
    dstd::Vector<FileKind> kinds;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        WalkOptions options;
        options.skip_dirs.push_back(dstd::String(".dftindex*"));
        auto stream = walk(scope, root, std::move(options));
        for (;;) {
            auto item = co_await stream.next();
            if (!item) break;
            REQUIRE(item->has_value());
            dstd::StringView text = (*item)->path.str();
            text.remove_prefix(root.str().size() + 1);
            names.push_back(dstd::String(text));
            kinds.push_back((*item)->kind);
        }
    });
    CHECK(std::count(names.begin(), names.end(), "sub") == 1);
    CHECK(std::count(names.begin(), names.end(), "sub/deep/d.pfw.gz") == 1);
    CHECK(std::count(names.begin(), names.end(), "link") == 1);
    CHECK(std::count(names.begin(), names.end(), "link/hidden.txt") == 0);
    CHECK(std::count(names.begin(), names.end(), ".dftindex") == 0);
    const auto link = std::find(names.begin(), names.end(), "link");
    REQUIRE(link != names.end());
    CHECK(kinds[static_cast<std::size_t>(link - names.begin())] ==
          FileKind::Symlink);
}

TEST_CASE("single calls and stat_many") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = make_tree(dir);
    run_in_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        const dstd::Path written = root / "written.txt";
        auto put = co_await write_file(written, "hello");
        CHECK(put.has_value());
        auto got = co_await read_file(written);
        REQUIRE(got.has_value());
        CHECK(*got == "hello");
        auto one = co_await stat(written);
        REQUIRE(one.has_value());
        CHECK(one->size == 5);
        const dstd::Path gone = root / "gone";
        auto none = co_await stat(gone);
        REQUIRE_FALSE(none.has_value());
        CHECK(none.error().is(dstd::Condition::NotFound));

        auto listed = co_await list_dir(root);
        REQUIRE(listed.has_value());
        CHECK(listed->size() == 6);

        dstd::Vector<dstd::Path> paths;
        for (int i = 0; i < 50; ++i) {
            paths.push_back(i == 37 ? gone : written);
        }
        auto many = co_await stat_many(
            dstd::Span<const dstd::Path>(paths.data(), paths.size()), 4);
        REQUIRE(many.size() == 50);
        for (std::size_t i = 0; i < many.size(); ++i) {
            CHECK(many[i].has_value() == (i != 37));
        }
        CHECK(many[37].error().is(dstd::Condition::NotFound));
        CHECK(many[0]->size == 5);
    });
}

TEST_CASE("find_all - on a plain thread with no IO backend") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = make_tree(dir);
    dstd::Vector<FoundFile> files;
    coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        FindOptions options_found = trace_options();
        auto found = co_await find_all(scope, root, std::move(options_found));
        REQUIRE(found.has_value());
        files = std::move(*found);
    }).get();
    CHECK(files.size() == 3);
}

TEST_CASE("find_all - an empty folder finds nothing") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = dir.path() / "empty";
    REQUIRE(dstd::fs::create_directories(root).has_value());
    CHECK(find_names(root, {}).empty());
    CHECK(find_names(root, {"*.txt"}, true).empty());
}

TEST_CASE("find_all - hidden and odd names and exact sizes") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = dir.path() / "names";
    write_file_at(root / ".hidden_file", "");
    write_file_at(root / "file with spaces.txt", "x");
    write_file_at(root / "file-with-dashes.txt", dstd::String(100, 'y'));
    write_file_at(root / "file_with_underscores.txt", dstd::String(1000, 'z'));
    dstd::Vector<FoundFile> files;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        auto found = co_await find_all(scope, root);
        REQUIRE(found.has_value());
        files = std::move(*found);
    });
    CHECK(
        names_below(root, files) ==
        expected_names({".hidden_file", "file with spaces.txt",
                        "file-with-dashes.txt", "file_with_underscores.txt"}));
    REQUIRE(files.size() == 4);
    CHECK(files[0].size == 0);
    CHECK(files[1].size == 1);
    CHECK(files[2].size == 100);
    CHECK(files[3].size == 1000);
}

TEST_CASE("find_all - many files in one folder") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = dir.path() / "flat";
    for (int i = 0; i < 100; ++i) {
        write_file_at(root / ("file" + dstd::to_string(i) + ".txt"), "x");
    }
    CHECK(find_names(root, {}).size() == 100);
}

TEST_CASE("find_all - the input constants list gzip files and skip folders") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = dir.path() / "inputs";
    write_file_at(root / "trace.pfw.gz", "t");
    write_file_at(root / "plain.pfw", "p");
    write_file_at(root / "lines.jsonl", "j");
    write_file_at(root / "nested" / "more.gz", "m");
    write_file_at(root / "split" / "copy.pfw.gz", "c");
    write_file_at(root / ".dftindex" / "view.pfw.gz", "v");
    write_file_at(root / ".dftindex-cache" / "view.pfw.gz", "v");
    write_file_at(root / "schemas" / "spec.json.gz", "s");
    dstd::Vector<FoundFile> files;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        FindOptions options;
        options.patterns =
            names_of(common::constants::indexer::INPUT_FILE_PATTERNS);
        options.skip_dirs =
            names_of(common::constants::indexer::SCAN_SKIP_DIRS);
        auto found = co_await find_all(scope, root, std::move(options));
        REQUIRE(found.has_value());
        files = std::move(*found);
    });
    CHECK(names_below(root, files) ==
          expected_names({"nested/more.gz", "trace.pfw.gz"}));
}

TEST_CASE("find_all - patterns pick files by name") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = dir.path() / "patterns";
    for (const char* name :
         {"file1.txt", "file2.txt", "file3.dat", "file.TXT", "file.v1.0.txt",
          "file.text", "file.tx", "archive.tar.gz", "data.pfw.gz", "file.gz",
          "README.md", "LICENSE", "README"}) {
        write_file_at(root / name, "x");
    }
    CHECK(find_names(root, {}).size() == 13);
    CHECK(find_names(root, {"*.txt"}) ==
          expected_names({"file.v1.0.txt", "file1.txt", "file2.txt"}));
    CHECK(find_names(root, {"*.txt", "*.dat"}) ==
          expected_names(
              {"file.v1.0.txt", "file1.txt", "file2.txt", "file3.dat"}));
    CHECK(find_names(root, {"*.tar.gz", "*.pfw.gz"}) ==
          expected_names({"archive.tar.gz", "data.pfw.gz"}));
    CHECK(find_names(root, {"README.md"}) == expected_names({"README.md"}));
    CHECK(find_names(root, {"README", "LICENSE"}) ==
          expected_names({"LICENSE", "README"}));
    CHECK(find_names(root, {"*.log"}).empty());
    CHECK(find_names(root, {"*.TXT"}) == expected_names({"file.TXT"}));
    CHECK(find_names(root, {"*.txt", "README.md", "*.pfw.gz"}) ==
          expected_names({"README.md", "data.pfw.gz", "file.v1.0.txt",
                          "file1.txt", "file2.txt"}));
}

TEST_CASE("find_all - a long list of patterns") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = dir.path() / "many_patterns";
    for (const char* name : {"file.ext1", "file.ext50", "file.ext100",
                             "file.ext101", "file.other"}) {
        write_file_at(root / name, "x");
    }
    FindOptions options;
    for (int i = 1; i <= 100; ++i) {
        options.patterns.push_back("*.ext" + dstd::to_string(i));
    }
    dstd::Vector<FoundFile> files;
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        auto found = co_await find_all(scope, root, std::move(options));
        REQUIRE(found.has_value());
        files = std::move(*found);
    });
    CHECK(files.size() == 3);
}

TEST_CASE("find_all - folders are never listed, and recursion is optional") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = dir.path() / "kinds";
    write_file_at(root / "real_file.txt", "x");
    REQUIRE(dstd::fs::create_directories(root / "test.txt").has_value());
    write_file_at(root / "test.txt" / "inner.txt", "x");
    write_file_at(root / "sub" / "nested" / "deep.txt", "x");
    write_file_at(root / "sub" / "other.dat", "x");
    CHECK(find_names(root, {"*.txt"}) == expected_names({"real_file.txt"}));
    CHECK(find_names(root, {"*.txt"}, true) ==
          expected_names(
              {"real_file.txt", "sub/nested/deep.txt", "test.txt/inner.txt"}));
    CHECK(find_names(root, {}).size() == 1);
    CHECK(find_names(root, {}, true).size() == 4);
}

TEST_CASE("find_all_on_pool - finds files and returns the error of a root") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = make_tree(dir);
    auto found = find_all_on_pool(2, 1, root, trace_options());
    REQUIRE(found.has_value());
    CHECK(names_below(root, *found) ==
          expected_names({"a.pfw.gz", "sub/c.pfw.gz", "sub/deep/d.pfw.gz"}));
    CHECK((*found)[0].size == 4);

    auto missing = find_all_on_pool(2, 1, root / "gone");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().is(dstd::Condition::NotFound));
}

TEST_CASE("mtime_seconds - drops the fraction of a second") {
    FoundFile file;
    file.mtime_ns = 3 * NANOS_PER_SECOND + 999999999;
    CHECK(file.mtime_seconds() == 3);
}

TEST_CASE("scans keep their stats for a later stat_input") {
    dftu_utils_test::ScopedTestDir dir("dftu_test_runtime_fs");
    const dstd::Path root = make_tree(dir);
    const dstd::Path scanned = root / "a.pfw.gz";
    const dstd::Path named = root / "b.txt";
    const common::InputStatScope kept_for_this_run;
    common::InputFileStat kept;
    CHECK_FALSE(common::cached_input(scanned.str(), kept));
    run_in_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        FindOptions options = trace_options();
        auto found = co_await find_all(scope, root, std::move(options));
        REQUIRE(found.has_value());
        dstd::Vector<dstd::Path> paths;
        paths.push_back(named);
        auto many = co_await stat_many(
            dstd::Span<const dstd::Path>(paths.data(), paths.size()));
        REQUIRE(many.size() == 1);
    });
    REQUIRE(common::cached_input(scanned.str(), kept));
    CHECK(kept.size == 4);
    CHECK(kept.is_regular_file);
    const auto truth = dstd::fs::stat(scanned);
    REQUIRE(truth.has_value());
    CHECK(static_cast<std::uint64_t>(kept.mtime_ns) == truth->mtime_ns);
    CHECK(common::cached_input(named.str(), kept));
    CHECK(kept.size == 2);
}
