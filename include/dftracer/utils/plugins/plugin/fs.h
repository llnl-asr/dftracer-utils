// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_FS_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_FS_H

#include <dftracer/utils/dataframe/plugin_error.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/async.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <cstdint>

namespace dftracer::utils::plugins {

/// One entry of a @ref PathList. `path` is valid until the list is freed.
struct PathEntry {
    dstd::StringView path;
    std::uint64_t size = 0;
    std::uint64_t mtime_ns = 0;
    dftu_file_kind kind = DFTU_FILE_UNKNOWN;
};

/// Owns the host list that `list_dir` or `find_all` fills and frees it when
/// destroyed. Pass it to the call, `co_await` the call, then read it.
class PathList {
   public:
    PathList(const dftu_svc_fs* fs, dftu_ctx* ctx) noexcept
        : fs_(fs), ctx_(ctx) {}
    PathList(PathList&& other) noexcept
        : fs_(other.fs_), ctx_(other.ctx_), list_(other.list_) {
        other.list_ = nullptr;
    }
    PathList(const PathList&) = delete;
    PathList& operator=(const PathList&) = delete;
    PathList& operator=(PathList&&) = delete;
    ~PathList() {
        if (list_ && fs_) fs_->list_free(list_);
    }

    /// The slot a call fills; it must outlive the call's task.
    dftu_path_list** slot() noexcept { return &list_; }

    std::uint64_t size() const noexcept {
        return list_ && fs_ ? fs_->list_count(list_) : 0;
    }
    /// DFTU_E_INVALID for an index past the end.
    dstd::Result<PathEntry> entry(std::uint64_t i) const {
        dftu_path_entry raw{};
        const dftu_status st = list_ && fs_
                                   ? fs_->list_entry(ctx_, list_, i, &raw)
                                   : DFTU_E_INVALID;
        if (st < 0) return dataframe::error_of(st);
        return PathEntry{dstd::StringView(raw.path, raw.path_len), raw.size,
                         raw.mtime_ns, raw.kind};
    }

   private:
    const dftu_svc_fs* fs_;
    dftu_ctx* ctx_;
    dftu_path_list* list_ = nullptr;
};

/// Owns the bytes `read_file` gives and frees them through their `free_fn`.
class FileBytes {
   public:
    FileBytes() = default;
    FileBytes(FileBytes&& other) noexcept : bytes_(other.bytes_) {
        other.bytes_ = dftu_bytes{};
    }
    FileBytes(const FileBytes&) = delete;
    FileBytes& operator=(const FileBytes&) = delete;
    FileBytes& operator=(FileBytes&&) = delete;
    ~FileBytes() {
        if (bytes_.free_fn)
            bytes_.free_fn(const_cast<void*>(bytes_.data), bytes_.ud);
    }

    /// The slot `read_file` fills; it must outlive the call's task.
    dftu_bytes* slot() noexcept { return &bytes_; }
    dstd::StringView view() const noexcept {
        return dstd::StringView(static_cast<const char*>(bytes_.data),
                                static_cast<std::size_t>(bytes_.len));
    }

   private:
    dftu_bytes bytes_{};
};

/// Thin typed wrapper over the fs service; each call starts the operation and
/// returns an @ref AsyncOp to `co_await`. A call the host refuses, or a host
/// without the fs group, gives an AsyncOp that fails when awaited, as does a
/// failed operation, with the status of its error and the message
/// "<op>: <text>". Paths are copied when called; buffers and result slots stay
/// valid until the await ends.
class Fs {
   public:
    Fs(const dftu_plugin_host* h, dftu_ctx* ctx)
        : h_(h),
          ctx_(ctx),
          fs_(h && h->get_service ? static_cast<const dftu_svc_fs*>(
                                        h->get_service(h->h, DFTU_SVC_FS))
                                  : nullptr) {}

    PathList new_list() const { return PathList(fs_, ctx_); }

    AsyncOp read_file(const char* path, FileBytes& out) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->read_file(ctx_, h_->h, path, out.slot(), &t), t);
    }
    AsyncOp write_file(const char* path, const void* data,
                       std::uint64_t len) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->write_file(ctx_, h_->h, path, data, len, &t), t);
    }
    AsyncOp read_at(const char* path, std::uint64_t off, void* buf,
                    std::uint64_t len, std::int64_t* out_n) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->read_at(ctx_, h_->h, path, off, buf, len, out_n, &t),
                    t);
    }
    AsyncOp write_at(const char* path, std::uint64_t off, const void* buf,
                     std::uint64_t len, std::int64_t* out_n) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->write_at(ctx_, h_->h, path, off, buf, len, out_n, &t),
                    t);
    }
    AsyncOp list_dir(const char* path, PathList& out) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->list_dir(ctx_, h_->h, path, out.slot(), &t), t);
    }
    /// `options` may be null for every file, recursive, with stat.
    AsyncOp find_all(const char* root, const dftu_find_options* options,
                     PathList& out) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->find_all(ctx_, h_->h, root, options, out.slot(), &t),
                    t);
    }
    AsyncOp exists(const char* path, std::int32_t* out) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->exists(ctx_, h_->h, path, out, &t), t);
    }
    AsyncOp create_directories(const char* path,
                               std::int32_t* out_created) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->create_directories(ctx_, h_->h, path, out_created, &t),
                    t);
    }
    AsyncOp remove(const char* path, std::int32_t* out_removed) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->remove(ctx_, h_->h, path, out_removed, &t), t);
    }
    /// One stat and one status per path; a path that cannot be stat'ed fails
    /// only its own slot. `paths`, `out` and `out_status` hold `n` entries.
    AsyncOp stat_batch(const char* const* paths, std::uint64_t n,
                       dftu_fs_stat* out, dftu_status* out_status) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->stat_batch(ctx_, h_->h, paths, n, out, out_status, &t),
                    t);
    }

   private:
    static AsyncOp unsupported() {
        return AsyncOp{
            make_error(dstd::Condition::Unsupported, "host lacks dftu.svc.fs")};
    }
    // `t` by reference: callers write done(call(..., &t), t), and the order of
    // argument evaluation is unspecified, so a copy could predate the call.
    AsyncOp done(dftu_status st, dftu_task* const& t) const {
        if (st < 0) return AsyncOp{dataframe::error_of(st)};
        return AsyncOp{t, h_};
    }

    const dftu_plugin_host* h_;
    dftu_ctx* ctx_;
    const dftu_svc_fs* fs_;
};

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_FS_H */
