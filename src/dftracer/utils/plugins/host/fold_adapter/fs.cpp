// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/host_status.h>
#include <dftracer/utils/plugins/task_abi.h>
#include <dftracer/utils/runtime/fs/filesystem.h>
#include <dftracer/utils/runtime/fs/fs.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace dftracer::utils::plugins::host {

namespace {

static_assert(static_cast<int>(dstd::fs::FileKind::Unknown) ==
              DFTU_FILE_UNKNOWN);
static_assert(static_cast<int>(dstd::fs::FileKind::File) == DFTU_FILE_FILE);
static_assert(static_cast<int>(dstd::fs::FileKind::Directory) ==
              DFTU_FILE_DIRECTORY);
static_assert(static_cast<int>(dstd::fs::FileKind::Symlink) ==
              DFTU_FILE_SYMLINK);
static_assert(static_cast<int>(dstd::fs::FileKind::Other) == DFTU_FILE_OTHER);

// The scheme of `path` ("s3" for "s3://b/k"), empty for a local path.
dstd::StringView scheme_of(dstd::StringView path) {
    const std::size_t at = path.find("://");
    return at == dstd::StringView::npos ? dstd::StringView{}
                                        : path.substr(0, at);
}

struct PathItem {
    dstd::String path;
    std::uint64_t size = 0;
    std::uint64_t mtime_ns = 0;
    dftu_file_kind kind = DFTU_FILE_UNKNOWN;
};

struct PathList {
    dstd::Vector<PathItem> items;
};

PathList* list_of(::dftu_path_list* list) {
    return reinterpret_cast<PathList*>(list);
}

const PathList* list_of(const ::dftu_path_list* list) {
    return reinterpret_cast<const PathList*>(list);
}

void fail_with(plugins::AbiTask* task, const char* op,
               const dstd::Error& error) {
    task->fail(common::to_status(error.condition()),
               dstd::fmt<"{}: {}">(dstd::StringView(op), error.message()));
}

template <class Make>
dftu_status start_task(::dftu_ctx* ctx, ::dftu_task** out, Make&& make) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "the task slot is NULL");
    *out = plugins::make_abi_task(std::forward<Make>(make));
    return DFTU_OK;
}

dftu_status check_path(::dftu_ctx* ctx, const char* op, const char* path) {
    if (path != nullptr) return DFTU_OK;
    return host_fail(ctx, DFTU_E_INVALID,
                     dstd::fmt<"{}: path is NULL">(dstd::StringView(op)));
}

void free_owned_string(void*, void* ud) {
    dstd::UniquePtr<dstd::String> owned(static_cast<dstd::String*>(ud));
}

coro::CoroTask<void> read_file_body(plugins::AbiTask* task, dstd::String path,
                                    dftu_bytes* out) {
    auto reading = runtime::fs::read_file(dstd::PathView(path));
    auto read = co_await std::move(reading);
    if (!read) {
        fail_with(task, "read_file", read.error());
        co_return;
    }
    if (out == nullptr) co_return;
    auto owned = dstd::make_unique<dstd::String>(std::move(*read));
    out->data = owned->data();
    out->len = owned->size();
    out->free_fn = free_owned_string;
    out->ud = owned.release();
}

dftu_status fs_read_file(::dftu_ctx* ctx, void*, const char* path,
                         dftu_bytes* out, ::dftu_task** task) {
    if (dftu_status st = check_path(ctx, "read_file", path); st < 0) return st;
    if (out != nullptr) *out = dftu_bytes{nullptr, 0, nullptr, nullptr};
    const dstd::String copy(path);
    return start_task(ctx, task, [=](plugins::AbiTask* t) {
        return read_file_body(t, copy, out);
    });
}

coro::CoroTask<void> write_file_body(plugins::AbiTask* task, dstd::String path,
                                     const void* data, std::uint64_t len) {
    auto writing = runtime::fs::write_file(
        dstd::PathView(path), dstd::StringView(static_cast<const char*>(data),
                                               static_cast<std::size_t>(len)));
    auto written = co_await std::move(writing);
    if (!written) fail_with(task, "write_file", written.error());
}

dftu_status fs_write_file(::dftu_ctx* ctx, void*, const char* path,
                          const void* data, std::uint64_t len,
                          ::dftu_task** task) {
    if (dftu_status st = check_path(ctx, "write_file", path); st < 0) return st;
    if (data == nullptr && len != 0)
        return host_fail(ctx, DFTU_E_INVALID, "write_file: data is NULL");
    const dstd::String copy(path);
    return start_task(ctx, task, [=](plugins::AbiTask* t) {
        return write_file_body(t, copy, data, len);
    });
}

coro::CoroTask<void> read_at_body(plugins::AbiTask* task, dstd::String path,
                                  std::uint64_t off, void* buf,
                                  std::uint64_t len, std::int64_t* out_n) {
    auto file_system = runtime::fs::file_system_for(dstd::PathView(path));
    if (!file_system) {
        fail_with(task, "read_at", file_system.error());
        co_return;
    }
    auto opening =
        (*file_system)->open(dstd::PathView(path), dstd::OpenOptions{});
    auto opened = co_await std::move(opening);
    if (!opened) {
        fail_with(task, "read_at", opened.error());
        co_return;
    }
    runtime::fs::FileSystemFile& file = **opened;
    auto reading = file.read_at(
        off, dstd::Span<std::uint8_t>(static_cast<std::uint8_t*>(buf),
                                      static_cast<std::size_t>(len)));
    auto got = co_await std::move(reading);
    auto closing = file.close();
    auto closed = co_await std::move(closing);
    if (!got) {
        fail_with(task, "read_at", got.error());
    } else if (!closed) {
        fail_with(task, "read_at", closed.error());
    } else if (out_n != nullptr) {
        *out_n = static_cast<std::int64_t>(*got);
    }
}

dftu_status fs_read_at(::dftu_ctx* ctx, void*, const char* path,
                       std::uint64_t off, void* buf, std::uint64_t len,
                       std::int64_t* out_n, ::dftu_task** task) {
    if (dftu_status st = check_path(ctx, "read_at", path); st < 0) return st;
    if (buf == nullptr && len != 0)
        return host_fail(ctx, DFTU_E_INVALID, "read_at: buf is NULL");
    const dstd::String copy(path);
    return start_task(ctx, task, [=](plugins::AbiTask* t) {
        return read_at_body(t, copy, off, buf, len, out_n);
    });
}

coro::CoroTask<void> write_at_body(plugins::AbiTask* task, dstd::String path,
                                   std::uint64_t off, const void* buf,
                                   std::uint64_t len, std::int64_t* out_n) {
    auto file_system = runtime::fs::file_system_for(dstd::PathView(path));
    if (!file_system) {
        fail_with(task, "write_at", file_system.error());
        co_return;
    }
    dstd::OpenOptions options;
    options.access = dstd::Access::Write;
    options.creation = dstd::Creation::IfNeeded;
    auto opening = (*file_system)->open(dstd::PathView(path), options);
    auto opened = co_await std::move(opening);
    if (!opened) {
        fail_with(task, "write_at", opened.error());
        co_return;
    }
    runtime::fs::FileSystemFile& file = **opened;
    auto writing =
        file.write_at(off, dstd::ByteView(static_cast<const std::uint8_t*>(buf),
                                          static_cast<std::size_t>(len)));
    auto put = co_await std::move(writing);
    auto closing = file.close();
    auto closed = co_await std::move(closing);
    if (!put) {
        fail_with(task, "write_at", put.error());
    } else if (!closed) {
        fail_with(task, "write_at", closed.error());
    } else if (out_n != nullptr) {
        *out_n = static_cast<std::int64_t>(*put);
    }
}

dftu_status fs_write_at(::dftu_ctx* ctx, void*, const char* path,
                        std::uint64_t off, const void* buf, std::uint64_t len,
                        std::int64_t* out_n, ::dftu_task** task) {
    if (dftu_status st = check_path(ctx, "write_at", path); st < 0) return st;
    if (buf == nullptr && len != 0)
        return host_fail(ctx, DFTU_E_INVALID, "write_at: buf is NULL");
    const dstd::String copy(path);
    return start_task(ctx, task, [=](plugins::AbiTask* t) {
        return write_at_body(t, copy, off, buf, len, out_n);
    });
}

dftu_file_kind kind_of(dstd::fs::FileKind kind) {
    return static_cast<dftu_file_kind>(kind);
}

coro::CoroTask<void> list_dir_body(plugins::AbiTask* task, dstd::String path,
                                   ::dftu_path_list** out) {
    auto listing = runtime::fs::list_dir(dstd::PathView(path));
    auto listed = co_await std::move(listing);
    if (!listed) {
        fail_with(task, "list_dir", listed.error());
        co_return;
    }
    auto list = dstd::make_unique<PathList>();
    list->items.reserve(listed->size());
    for (const runtime::fs::Entry& entry : *listed) {
        PathItem item;
        item.path.assign(entry.path.str().data(), entry.path.str().size());
        item.kind = kind_of(entry.kind);
        list->items.push_back(std::move(item));
    }
    if (out != nullptr) {
        *out = reinterpret_cast<::dftu_path_list*>(list.release());
    }
}

dftu_status fs_list_dir(::dftu_ctx* ctx, void*, const char* path,
                        ::dftu_path_list** out, ::dftu_task** task) {
    if (dftu_status st = check_path(ctx, "list_dir", path); st < 0) return st;
    if (out != nullptr) *out = nullptr;
    const dstd::String copy(path);
    return start_task(ctx, task, [=](plugins::AbiTask* t) {
        return list_dir_body(t, copy, out);
    });
}

using FoundResult =
    dstd::Expected<dstd::Vector<runtime::fs::FoundFile>, dstd::Error>;

coro::CoroTask<void> find_all_body(plugins::AbiTask* task, dstd::String root,
                                   runtime::fs::FindOptions options,
                                   ::dftu_path_list** out) {
    auto body = [&](coro::Scope& scope) -> coro::CoroTask<FoundResult> {
        auto finding = runtime::fs::find_all(scope, dstd::PathView(root),
                                             std::move(options));
        co_return co_await std::move(finding);
    };
    auto scoped = coro::with_scope(body);
    auto found = co_await std::move(scoped);
    if (!found) {
        fail_with(task, "find_all", found.error());
        co_return;
    }
    auto list = dstd::make_unique<PathList>();
    list->items.reserve(found->size());
    for (const runtime::fs::FoundFile& file : *found) {
        PathItem item;
        item.path.assign(file.path.str().data(), file.path.str().size());
        item.size = file.size;
        item.mtime_ns = file.mtime_ns;
        item.kind = DFTU_FILE_FILE;
        list->items.push_back(std::move(item));
    }
    if (out != nullptr) {
        *out = reinterpret_cast<::dftu_path_list*>(list.release());
    }
}

dstd::Vector<dstd::String> copy_names(const char* const* names,
                                      std::uint32_t count) {
    dstd::Vector<dstd::String> out;
    for (std::uint32_t i = 0; names != nullptr && i < count; ++i) {
        if (names[i] != nullptr) out.push_back(dstd::String(names[i]));
    }
    return out;
}

dftu_status fs_find_all(::dftu_ctx* ctx, void*, const char* root,
                        const dftu_find_options* options,
                        ::dftu_path_list** out, ::dftu_task** task) {
    if (dftu_status st = check_path(ctx, "find_all", root); st < 0) return st;
    dftu_find_options given{};
    given.recursive = 1;
    given.with_stat = 1;
    if (options != nullptr &&
        !common::copy_struct(&given, sizeof(given), options,
                             options->struct_size,
                             offsetof(dftu_find_options, patterns))) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "find_all: the options struct is too small");
    }
    runtime::fs::FindOptions find;
    find.patterns = copy_names(given.patterns, given.pattern_count);
    find.skip_dirs = copy_names(given.skip_dirs, given.skip_dir_count);
    find.recursive = given.recursive != 0;
    find.with_stat = given.with_stat != 0;
    find.limit = static_cast<std::size_t>(given.limit);
    find.skip_unreadable = given.skip_unreadable != 0;
    if (out != nullptr) *out = nullptr;
    const dstd::String copy(root);
    return start_task(ctx, task, [=](plugins::AbiTask* t) {
        return find_all_body(t, copy, find, out);
    });
}

std::uint64_t fs_list_count(const ::dftu_path_list* list) {
    return list == nullptr ? 0 : list_of(list)->items.size();
}

dftu_status fs_list_entry(::dftu_ctx* ctx, const ::dftu_path_list* list,
                          std::uint64_t i, dftu_path_entry* out) {
    if (list == nullptr || out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "list_entry: a pointer is NULL");
    const PathList* items = list_of(list);
    if (i >= items->items.size())
        return host_fail(ctx, DFTU_E_INVALID, "list_entry: index out of range");
    const PathItem& item = items->items[static_cast<std::size_t>(i)];
    out->path = item.path.c_str();
    out->size = item.size;
    out->mtime_ns = item.mtime_ns;
    out->path_len = static_cast<std::uint32_t>(item.path.size());
    out->kind = item.kind;
    return DFTU_OK;
}

void fs_list_free(::dftu_path_list* list) {
    dstd::UniquePtr<PathList> owned(list_of(list));
}

template <class Call>
coro::CoroTask<void> flag_body(plugins::AbiTask* task, const char* op,
                               dstd::String path, std::int32_t* out,
                               Call call) {
    auto running = call(dstd::PathView(path));
    auto result = co_await std::move(running);
    if (!result.has_value()) {
        fail_with(task, op, result.error());
        co_return;
    }
    if (out != nullptr) *out = result.value() ? 1 : 0;
}

template <class Call>
dftu_status flag_start(::dftu_ctx* ctx, const char* op, const char* path,
                       std::int32_t* out, ::dftu_task** task, Call call) {
    if (dftu_status st = check_path(ctx, op, path); st < 0) return st;
    const dstd::String copy(path);
    return start_task(ctx, task, [=](plugins::AbiTask* t) {
        return flag_body(t, op, copy, out, call);
    });
}

dftu_status fs_exists(::dftu_ctx* ctx, void*, const char* path,
                      std::int32_t* out, ::dftu_task** task) {
    return flag_start(ctx, "exists", path, out, task,
                      [](dstd::PathView p) { return runtime::fs::exists(p); });
}

dftu_status fs_create_directories(::dftu_ctx* ctx, void*, const char* path,
                                  std::int32_t* out_created,
                                  ::dftu_task** task) {
    return flag_start(
        ctx, "create_directories", path, out_created, task,
        [](dstd::PathView p) { return runtime::fs::create_directories(p); });
}

dftu_status fs_remove(::dftu_ctx* ctx, void*, const char* path,
                      std::int32_t* out_removed, ::dftu_task** task) {
    return flag_start(ctx, "remove", path, out_removed, task,
                      [](dstd::PathView p) { return runtime::fs::remove(p); });
}

coro::CoroTask<void> stat_batch_body(plugins::AbiTask*,
                                     dstd::Vector<dstd::Path> paths,
                                     dftu_fs_stat* out,
                                     dftu_status* out_status) {
    auto statting = runtime::fs::stat_many(
        dstd::Span<const dstd::Path>(paths.data(), paths.size()));
    auto results = co_await std::move(statting);
    for (std::size_t i = 0; i < results.size(); ++i) {
        dftu_fs_stat stat{};
        dftu_status status = DFTU_OK;
        if (results[i]) {
            const dstd::fs::FileStat& st = *results[i];
            stat.size = st.size;
            stat.mtime_ns = st.mtime_ns;
            stat.dev = st.dev;
            stat.ino = st.ino;
            stat.mode = st.mode;
            stat.kind = kind_of(st.kind);
        } else {
            status = common::to_status(results[i].error().condition());
        }
        out[i] = stat;
        out_status[i] = status;
    }
}

dftu_status fs_stat_batch(::dftu_ctx* ctx, void*, const char* const* paths,
                          std::uint64_t n, dftu_fs_stat* out,
                          dftu_status* out_status, ::dftu_task** task) {
    if (n != 0 && (paths == nullptr || out == nullptr || out_status == nullptr))
        return host_fail(ctx, DFTU_E_INVALID,
                         "stat_batch: paths, out or out_status is NULL");
    dstd::Vector<dstd::Path> copies;
    copies.reserve(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i) {
        if (paths[i] == nullptr)
            return host_fail(ctx, DFTU_E_INVALID, "stat_batch: a path is NULL");
        if (scheme_of(paths[i]) != scheme_of(paths[0]))
            return host_fail(ctx, DFTU_E_INVALID,
                             "stat_batch: every path must use one file system");
        copies.push_back(dstd::Path(dstd::StringView(paths[i])));
    }
    return start_task(ctx, task, [=](plugins::AbiTask* t) {
        return stat_batch_body(t, copies, out, out_status);
    });
}

const dftu_svc_fs FS_VTABLE = {
    sizeof(dftu_svc_fs), fs_read_file, fs_write_file, fs_read_at,
    fs_write_at,         fs_list_dir,  fs_find_all,   fs_list_count,
    fs_list_entry,       fs_list_free, fs_exists,     fs_create_directories,
    fs_remove,           fs_stat_batch};

}  // namespace

const void* detail::fs_ext_vtable() { return &FS_VTABLE; }

}  // namespace dftracer::utils::plugins::host
