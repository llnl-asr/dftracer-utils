// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/runtime/fs/filesystem.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

using namespace dftracer::utils;

namespace {

using runtime::fs::Entry;
using runtime::fs::FileSystem;
using runtime::fs::FileSystemFile;
using runtime::fs::FoundFile;

// Files by full path, directories implied by the paths.
struct MemoryStore {
    std::mutex mutex;
    dstd::HashMap<dstd::String, dstd::String> files;
    int closes = 0;
    int stats = 0;
    int lists = 0;
};

class MemoryFile final : public FileSystemFile {
   public:
    MemoryFile(MemoryStore& store, dstd::String path)
        : store_(store), path_(std::move(path)) {}

    coro::CoroTask<dstd::Expected<std::size_t, dstd::Error>> read_at(
        std::uint64_t offset, dstd::Span<std::uint8_t> out) override {
        std::lock_guard<std::mutex> lock(store_.mutex);
        const dstd::String& text = store_.files[path_];
        if (offset >= text.size()) co_return std::size_t{0};
        const std::size_t count = std::min<std::size_t>(
            std::min<std::size_t>(out.size(), 3), text.size() - offset);
        std::memcpy(out.data(), text.data() + offset, count);
        co_return count;
    }

    coro::CoroTask<dstd::Expected<std::size_t, dstd::Error>> write_at(
        std::uint64_t offset, dstd::ByteView data) override {
        std::lock_guard<std::mutex> lock(store_.mutex);
        dstd::String& text = store_.files[path_];
        const std::size_t count = std::min<std::size_t>(data.size(), 3);
        text.resize(std::max<std::size_t>(text.size(), offset + count));
        std::memcpy(text.data() + offset, data.data(), count);
        co_return count;
    }

    coro::CoroTask<dstd::Expected<void, dstd::Error>> close() override {
        std::lock_guard<std::mutex> lock(store_.mutex);
        ++store_.closes;
        co_return dstd::Expected<void, dstd::Error>{};
    }

   private:
    MemoryStore& store_;
    dstd::String path_;
};

class MemoryFileSystem final : public FileSystem {
   public:
    explicit MemoryFileSystem(MemoryStore& store) : store_(store) {}

    coro::CoroTask<dstd::Expected<dstd::UniquePtr<FileSystemFile>, dstd::Error>>
    open(dstd::PathView path, dstd::OpenOptions options) override {
        std::lock_guard<std::mutex> lock(store_.mutex);
        const dstd::String key(path.str());
        if (options.creation == dstd::Creation::AlwaysNew) {
            store_.files[key].clear();
        } else if (store_.files.find(key) == store_.files.end()) {
            co_return dstd::Unexpected(
                dstd::Error(dstd::Condition::NotFound, "gone"));
        }
        dstd::UniquePtr<FileSystemFile> file =
            dstd::make_unique<MemoryFile>(store_, key);
        co_return file;
    }

    coro::CoroTask<dstd::Expected<dstd::fs::FileStat, dstd::Error>> stat(
        dstd::PathView path) override {
        std::lock_guard<std::mutex> lock(store_.mutex);
        ++store_.stats;
        const dstd::String key(path.str());
        dstd::fs::FileStat info;
        const auto found = store_.files.find(key);
        if (found != store_.files.end()) {
            info.kind = dstd::fs::FileKind::File;
            info.size = found->second.size();
            co_return info;
        }
        const dstd::String prefix = dstd::fmt<"{}/">(key);
        for (const auto& item : store_.files) {
            if (dstd::StringView(item.first).starts_with(prefix)) {
                info.kind = dstd::fs::FileKind::Directory;
                co_return info;
            }
        }
        co_return dstd::Unexpected(
            dstd::Error(dstd::Condition::NotFound, "gone"));
    }

    coro::CoroTask<dstd::Expected<dstd::Vector<Entry>, dstd::Error>> list(
        dstd::PathView path) override {
        std::lock_guard<std::mutex> lock(store_.mutex);
        ++store_.lists;
        const dstd::String prefix = dstd::fmt<"{}/">(path.str());
        dstd::Vector<Entry> entries;
        for (const auto& item : store_.files) {
            dstd::StringView rest(item.first);
            if (!rest.starts_with(prefix)) continue;
            rest.remove_prefix(prefix.size());
            const std::size_t slash = rest.find('/');
            const bool is_dir = slash != dstd::StringView::npos;
            dstd::String child =
                dstd::fmt<"{}{}">(prefix, dstd::String(rest.substr(0, slash)));
            const bool seen = std::any_of(
                entries.begin(), entries.end(), [&](const Entry& entry) {
                    return entry.path.str() == dstd::StringView(child);
                });
            if (seen) continue;
            entries.push_back(Entry{dstd::Path(std::move(child)),
                                    is_dir ? dstd::fs::FileKind::Directory
                                           : dstd::fs::FileKind::File});
        }
        co_return entries;
    }

   private:
    MemoryStore& store_;
};

struct Mounted {
    MemoryStore store;
    Mounted() {
        runtime::fs::register_file_system(
            "mem", dstd::make_shared<MemoryFileSystem>(store));
    }
    ~Mounted() { runtime::fs::unregister_file_system("mem"); }
};

dstd::Vector<dstd::String> sorted_paths(const dstd::Vector<FoundFile>& found) {
    dstd::Vector<dstd::String> paths;
    for (const FoundFile& file : found) {
        paths.push_back(dstd::String(file.path.str()));
    }
    return paths;
}

}  // namespace

TEST_CASE("file system: find runs on a registered file system") {
    Mounted mounted;
    mounted.store.files["mem://bucket/a.pfw"] = "aaaa";
    mounted.store.files["mem://bucket/b.txt"] = "b";
    mounted.store.files["mem://bucket/sub/c.pfw"] = "cc";
    Runtime rt(ExecutorConfig{.thread_count = 2}, false);
    dstd::Vector<FoundFile> found;
    dftu_utils_test::ok(rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            runtime::fs::FindOptions options;
            options.patterns.push_back(dstd::String("*.pfw"));
            auto all = co_await runtime::fs::find_all(
                scope, dstd::PathView("mem://bucket"), std::move(options));
            REQUIRE(all.has_value());
            found = std::move(*all);
        })));
    const auto paths = sorted_paths(found);
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == "mem://bucket/a.pfw");
    CHECK(paths[1] == "mem://bucket/sub/c.pfw");
    CHECK(found[0].size == 4);
    CHECK(found[1].size == 2);
    // The base stat_batch calls stat, so every matched file came through it.
    CHECK(mounted.store.stats >= 3);
    CHECK(mounted.store.lists == 2);
}

TEST_CASE("file system: read_file and write_file take short transfers") {
    Mounted mounted;
    Runtime rt(ExecutorConfig{.thread_count = 2}, false);
    const dstd::String text = "more bytes than one transfer moves";
    auto body = [&]() -> coro::CoroTask<void> {
        auto written = co_await runtime::fs::write_file(
            dstd::PathView("mem://bucket/out"), text);
        REQUIRE(written.has_value());
        auto read =
            co_await runtime::fs::read_file(dstd::PathView("mem://bucket/out"));
        REQUIRE(read.has_value());
        CHECK(*read == text);
    };
    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(mounted.store.closes == 2);
}

TEST_CASE("file system: a missing file reports NotFound through the seam") {
    Mounted mounted;
    Runtime rt(ExecutorConfig{.thread_count = 2}, false);
    auto body = [&]() -> coro::CoroTask<void> {
        auto read =
            co_await runtime::fs::read_file(dstd::PathView("mem://bucket/no"));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().is(dstd::Condition::NotFound));
    };
    dftu_utils_test::ok(rt.block_on(body()));
}

TEST_CASE("file system: an unknown scheme is an error everywhere") {
    Runtime rt(ExecutorConfig{.thread_count = 2}, false);
    const dstd::PathView path("none://x");
    CHECK_FALSE(runtime::fs::file_system_for(path).has_value());
    auto body = [&]() -> coro::CoroTask<void> {
        auto read = co_await runtime::fs::read_file(path);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().is(dstd::Condition::InvalidArgument));
        auto stat = co_await runtime::fs::stat(path);
        CHECK_FALSE(stat.has_value());
    };
    dftu_utils_test::ok(rt.block_on(body()));
    dftu_utils_test::ok(rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            auto found = co_await runtime::fs::find_all(scope, path);
            REQUIRE_FALSE(found.has_value());
            CHECK(found.error().is(dstd::Condition::InvalidArgument));
        })));
}

TEST_CASE("file system: a file:// path and a plain path are one local file") {
    char name[] = "/tmp/dftu_fs_XXXXXX";
    const int fd = ::mkstemp(name);
    REQUIRE(fd >= 0);
    ::close(fd);
    const dstd::String uri = dstd::fmt<"file://{}">(name);
    Runtime rt(ExecutorConfig{.thread_count = 2}, false);
    auto body = [&]() -> coro::CoroTask<void> {
        auto written = co_await runtime::fs::write_file(dstd::PathView(uri),
                                                        "through the uri");
        REQUIRE(written.has_value());
        auto read = co_await runtime::fs::read_file(dstd::PathView(name));
        REQUIRE(read.has_value());
        CHECK(*read == "through the uri");
    };
    dftu_utils_test::ok(rt.block_on(body()));
    ::unlink(name);
}

TEST_CASE(
    "file system: exists, create_directories and remove run off the worker") {
    char root[] = "/tmp/dftu_fs_ops_XXXXXX";
    REQUIRE(::mkdtemp(root) != nullptr);
    const dstd::String dir = dstd::fmt<"{}/a/b">(root);
    const dstd::String file = dstd::fmt<"{}/file">(root);
    Runtime rt(ExecutorConfig{.thread_count = 2}, false);
    auto body = [&]() -> coro::CoroTask<void> {
        auto missing = co_await runtime::fs::exists(dstd::PathView(dir));
        REQUIRE(missing.has_value());
        CHECK_FALSE(*missing);

        auto made =
            co_await runtime::fs::create_directories(dstd::PathView(dir));
        REQUIRE(made.has_value());
        CHECK(*made);
        auto present = co_await runtime::fs::exists(dstd::PathView(dir));
        REQUIRE(present.has_value());
        CHECK(*present);
        auto again =
            co_await runtime::fs::create_directories(dstd::PathView(dir));
        REQUIRE(again.has_value());
        CHECK_FALSE(*again);

        auto written =
            co_await runtime::fs::write_file(dstd::PathView(file), "x");
        REQUIRE(written.has_value());
        auto under_file = co_await runtime::fs::create_directories(
            dstd::PathView(dstd::fmt<"{}/sub">(file)));
        CHECK_FALSE(under_file.has_value());

        auto removed = co_await runtime::fs::remove(dstd::PathView(file));
        REQUIRE(removed.has_value());
        CHECK(*removed);
        auto gone = co_await runtime::fs::remove(dstd::PathView(file));
        REQUIRE(gone.has_value());
        CHECK_FALSE(*gone);
        auto not_empty = co_await runtime::fs::remove(
            dstd::PathView(dstd::fmt<"{}/a">(root)));
        CHECK_FALSE(not_empty.has_value());
    };
    dftu_utils_test::ok(rt.block_on(body()));
    ::rmdir(dir.c_str());
    ::rmdir((dstd::fmt<"{}/a">(root)).c_str());
    ::rmdir(root);
}
