// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/common/test_probes.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/os.h>
#include <dftracer/utils/runtime/fs/filesystem.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/fs/fs_probes.h>
#include <dftracer/utils/runtime/fs/local_stat.h>
#include <dftracer/utils/runtime/io/ops.h>

#include <atomic>
#include <cerrno>
#include <mutex>
#include <type_traits>
#include <utility>

namespace dftracer::utils::runtime::fs {

namespace {

using dstd::fs::FileKind;

#if DFTRACER_UTILS_TEST_PROBES
std::atomic<std::size_t> g_stat_batches{0};
std::atomic<std::size_t> g_stat_threads{0};
std::atomic<std::size_t> g_stat_epoch{1};
thread_local std::size_t t_stat_epoch = 0;

void note_stat_batch() noexcept {
    g_stat_batches.fetch_add(1, std::memory_order_relaxed);
    const std::size_t epoch = g_stat_epoch.load(std::memory_order_relaxed);
    if (t_stat_epoch == epoch) return;
    t_stat_epoch = epoch;
    g_stat_threads.fetch_add(1, std::memory_order_relaxed);
}
#endif

constexpr dstd::StringView FILE_PREFIX = "file://";

dstd::PathView local_path(dstd::PathView path) noexcept {
    dstd::StringView text = path.str();
    if (text.starts_with(FILE_PREFIX)) text.remove_prefix(FILE_PREFIX.size());
    return dstd::PathView(text);
}

// A later layer that asks for this input through common::stat_input makes no
// second syscall.
void keep_input(dstd::PathView path, const dstd::fs::FileStat& found) noexcept {
    common::InputFileStat kept;
    kept.size = found.size;
    kept.mtime = static_cast<std::int64_t>(found.mtime_ns / NANOS_PER_SECOND);
    kept.mtime_ns = static_cast<std::int64_t>(found.mtime_ns);
    kept.is_directory = found.kind == FileKind::Directory;
    kept.is_regular_file = found.kind == FileKind::File;
    common::remember_input(path.str(), kept);
}

dstd::Expected<dstd::fs::FileStat, dstd::Error> stat_and_keep(
    dstd::PathView path) noexcept {
    auto found = dstd::fs::stat(path);
    if (found) keep_input(path, *found);
    return found;
}

template <typename Fn>
struct IoCall {
    Fn* fn;
    dstd::Optional<std::invoke_result_t<Fn&>> result;

    static ssize_t run(void* context) noexcept {
        auto* call = static_cast<IoCall*>(context);
        call->result.emplace((*call->fn)());
        return 0;
    }
};

// A call with no I/O thread free stays queued and holds no worker.
template <typename Fn>
coro::CoroTask<std::invoke_result_t<Fn&>> on_io(Fn fn) {
    IoCall<Fn> call{&fn, dstd::nullopt};
    runtime::io::IoAwaitable op =
        runtime::io::call(&IoCall<Fn>::run, static_cast<void*>(&call));
    co_await op;
    co_return std::move(*call.result);
}

dstd::Expected<dstd::Vector<Entry>, dstd::Error> list_sync(
    dstd::PathView dir) noexcept {
    auto opened = dstd::fs::Dir::open(dir);
    if (!opened) return dstd::Unexpected(opened.error());
    auto entries = opened->entries();
    if (!entries) return dstd::Unexpected(entries.error());
    dstd::Vector<Entry> listing;
    for (const dstd::fs::DirEntry& entry : *entries) {
        FileKind kind = entry.kind;
        if (kind == FileKind::Unknown) {
            const auto found = opened->lstat(entry.name);
            if (found) kind = found->kind;
        }
        listing.push_back(Entry{dir / entry.name, kind});
    }
    if (entries->error() != 0) {
        return dstd::Unexpected(
            dstd::Error::from_format<"Cannot read directory: {}">(
                dstd::Condition::Io, dir.str()));
    }
    return listing;
}

class LocalFile final : public FileSystemFile {
   public:
    LocalFile(dstd::File file, dstd::PathView path)
        : file_(std::move(file)), path_(path.str()) {}

    coro::CoroTask<dstd::Expected<std::size_t, dstd::Error>> read_at(
        std::uint64_t offset, dstd::Span<std::uint8_t> out) override {
        for (;;) {
            ssize_t got = co_await runtime::io::pread(
                file_.native_handle(), out.data(), out.size(), offset);
            // A pipe has no offsets, so read it in order.
            if (got == -ESPIPE) {
                got = co_await runtime::io::read(file_.native_handle(),
                                                 out.data(), out.size());
            }
            if (got == -EINTR) continue;
            if (got < 0) {
                co_return dstd::Unexpected(
                    dstd::internal::errno_error(static_cast<int>(-got), path_));
            }
            co_return static_cast<std::size_t>(got);
        }
    }

    coro::CoroTask<dstd::Expected<std::size_t, dstd::Error>> write_at(
        std::uint64_t offset, dstd::ByteView data) override {
        for (;;) {
            const ssize_t put = co_await runtime::io::pwrite(
                file_.native_handle(), data.data(), data.size(), offset);
            if (put == -EINTR) continue;
            if (put < 0) {
                co_return dstd::Unexpected(
                    dstd::internal::errno_error(static_cast<int>(-put), path_));
            }
            co_return static_cast<std::size_t>(put);
        }
    }

    coro::CoroTask<dstd::Expected<void, dstd::Error>> close() override {
        const dstd::NativeHandle handle = file_.release();
        const ssize_t closed = co_await runtime::io::close(handle);
        if (closed < 0) {
            co_return dstd::Unexpected(
                dstd::internal::errno_error(static_cast<int>(-closed), path_));
        }
        co_return dstd::Expected<void, dstd::Error>{};
    }

   private:
    dstd::File file_;
    dstd::String path_;
};

class LocalFileSystem final : public FileSystem {
   public:
    coro::CoroTask<dstd::Expected<dstd::UniquePtr<FileSystemFile>, dstd::Error>>
    open(dstd::PathView path, dstd::OpenOptions options) override {
        const dstd::PathView local = local_path(path);
        auto opening = runtime::io::open_file(local, options);
        auto opened = co_await std::move(opening);
        if (!opened) co_return dstd::Unexpected(opened.error());
        dstd::UniquePtr<FileSystemFile> file =
            dstd::make_unique<LocalFile>(std::move(*opened), local);
        co_return file;
    }

    coro::CoroTask<dstd::Expected<dstd::fs::FileStat, dstd::Error>> stat(
        dstd::PathView path) override {
        const dstd::PathView local = local_path(path);
#ifdef __linux__
        auto call = local_stat(local, true);
        auto found = co_await std::move(call);
        if (found) keep_input(local, *found);
        co_return found;
#else
        auto fn = [local]() noexcept { return stat_and_keep(local); };
        auto call = on_io(fn);
        co_return co_await std::move(call);
#endif
    }

    coro::CoroTask<dstd::Expected<dstd::Vector<Entry>, dstd::Error>> list(
        dstd::PathView path) override {
        const dstd::PathView local = local_path(path);
        auto fn = [local]() noexcept { return list_sync(local); };
        auto call = on_io(fn);
        co_return co_await std::move(call);
    }

    coro::CoroTask<void> stat_batch(
        dstd::Span<const dstd::PathView> paths,
        dstd::Span<dstd::Expected<dstd::fs::FileStat, dstd::Error>> out)
        override {
        auto fn = [paths, out]() noexcept {
#if DFTRACER_UTILS_TEST_PROBES
            note_stat_batch();
#endif
            for (std::size_t i = 0; i < paths.size(); ++i) {
                out[i] = stat_and_keep(local_path(paths[i]));
            }
            return true;
        };
        auto call = on_io(fn);
        co_await std::move(call);
    }
};

const dstd::SharedPtr<FileSystem>& local_file_system() {
    static const dstd::SharedPtr<FileSystem> local =
        dstd::make_shared<LocalFileSystem>();
    return local;
}

struct Registry {
    std::mutex mutex;
    dstd::HashMap<dstd::String, dstd::SharedPtr<FileSystem>> by_scheme;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

}  // namespace

coro::CoroTask<void> FileSystem::stat_batch(
    dstd::Span<const dstd::PathView> paths,
    dstd::Span<dstd::Expected<dstd::fs::FileStat, dstd::Error>> out) {
    for (std::size_t i = 0; i < paths.size(); ++i) {
        auto call = stat(paths[i]);
        out[i] = co_await std::move(call);
    }
}

void register_file_system(dstd::StringView scheme,
                          dstd::SharedPtr<FileSystem> file_system) {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    reg.by_scheme.insert_or_assign(dstd::String(scheme),
                                   std::move(file_system));
}

void unregister_file_system(dstd::StringView scheme) {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    reg.by_scheme.erase(dstd::String(scheme));
}

dstd::Expected<dstd::SharedPtr<FileSystem>, dstd::Error> file_system_for(
    dstd::PathView path) {
    const dstd::StringView text = path.str();
    const std::size_t separator = text.find("://");
    if (separator == dstd::StringView::npos || separator == 0) {
        return local_file_system();
    }
    const dstd::StringView scheme = text.substr(0, separator);
    if (scheme == "file") return local_file_system();
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const auto found = reg.by_scheme.find(dstd::String(scheme));
    if (found == reg.by_scheme.end()) {
        return dstd::Unexpected(
            dstd::Error::from_format<"No file system for scheme: {}">(
                dstd::Condition::InvalidArgument, scheme));
    }
    return found->second;
}

#if DFTRACER_UTILS_TEST_PROBES
std::size_t stat_batch_count() noexcept {
    return g_stat_batches.load(std::memory_order_relaxed);
}

std::size_t stat_thread_count() noexcept {
    return g_stat_threads.load(std::memory_order_relaxed);
}

bool has_run_stat_batch() noexcept {
    return t_stat_epoch == g_stat_epoch.load(std::memory_order_relaxed);
}

void reset_stat_threads() noexcept {
    g_stat_epoch.fetch_add(1, std::memory_order_relaxed);
    g_stat_threads.store(0, std::memory_order_relaxed);
}
#endif

}  // namespace dftracer::utils::runtime::fs
