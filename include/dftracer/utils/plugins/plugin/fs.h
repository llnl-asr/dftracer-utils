// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_FS_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_FS_H

#include <dftracer/utils/dataframe/plugin_error.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/async.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <array>
#include <cstdint>

namespace dftracer::utils::plugins {

/// One entry of a @ref PathList. `path` is valid until the list is freed.
struct PathEntry {
    dstd::StringView path;
    std::uint64_t size = 0;
    std::uint64_t mtime_ns = 0;
    FileKind kind = FileKind::Unknown;
};

/// The one bool a call reports: the caller owns it and passes it to the call,
/// which fills it during the await.
class Flag {
   public:
    /// False until the call fills it.
    bool set() const noexcept { return raw_ != 0; }

   private:
    friend class Fs;

    std::int32_t raw_ = 0;
};

namespace detail {

struct StatStatusAccess;

/// The `const char*` array a C call takes for a list of paths or names. The
/// common short list stays on the stack, so the call allocates nothing.
template <std::size_t Inline = 16>
class CStrArray {
   public:
    CStrArray() = default;
    CStrArray(const CStrArray&) = delete;
    CStrArray& operator=(const CStrArray&) = delete;

    /// Hold `views`; they must outlive the array.
    void reset(dstd::Span<const dstd::CStringView> views) {
        heap_.clear();
        count_ = views.size();
        if (count_ <= Inline) {
            for (std::size_t i = 0; i < count_; ++i)
                stack_[i] = views[i].c_str();
            data_ = count_ == 0 ? nullptr : stack_.data();
            return;
        }
        heap_.reserve(count_);
        for (const dstd::CStringView view : views)
            heap_.push_back(view.c_str());
        data_ = heap_.data();
    }

    const char* const* data() const noexcept { return data_; }
    std::size_t count() const noexcept { return count_; }

   private:
    std::array<const char*, Inline> stack_{};
    dstd::Vector<const char*> heap_;
    const char* const* data_ = nullptr;
    std::size_t count_ = 0;
};

}  // namespace detail

/// The outcome of one @ref Fs::stat_batch slot: @ref ok when the path was
/// stat'ed, else the condition of its failure. The caller owns the array of
/// them.
class StatStatus {
   public:
    bool ok() const noexcept { return raw_ >= 0; }
    /// @ref dstd::Condition "Condition::Unknown" when @ref ok, since a success
    /// carries no condition.
    dstd::Condition condition() const noexcept {
        return ok() ? dstd::Condition::Unknown
                    : dataframe::error_of(raw_).condition();
    }

   private:
    friend struct detail::StatStatusAccess;

    dftu_status raw_ = 0;
};
static_assert(sizeof(StatStatus) == sizeof(dftu_status));

/// Options of @ref Fs::find_all; the defaults find every file below the root,
/// recursively, with a stat. The two lists are borrowed for the call, which
/// copies every name.
struct FindOptions {
    dstd::Span<const dstd::CStringView> patterns;
    dstd::Span<const dstd::CStringView> skip_dirs;
    /// The stat calls in flight at most; 0 for the executor's concurrency.
    std::uint64_t limit = 0;
    bool recursive = true;
    bool with_stat = true;
    bool skip_unreadable = false;
};

namespace detail {

struct StatStatusAccess {
    static dftu_status* raw(StatStatus& status) noexcept {
        return &status.raw_;
    }
    /// A @ref StatStatus holds nothing but its status, so a span of them is the
    /// one contiguous array the ABI fills.
    static dftu_status* raw(dstd::Span<StatStatus> statuses) noexcept {
        return statuses.empty()
                   ? nullptr
                   : reinterpret_cast<dftu_status*>(statuses.data());
    }
};

}  // namespace detail

/// Owns the host list that `list_dir` or `find_all` fills and frees it when
/// destroyed. Pass it to the call, `co_await` the call, then read it.
class PathList {
   public:
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
                         raw.mtime_ns, static_cast<FileKind>(raw.kind)};
    }

   private:
    friend class Fs;

    PathList(const dftu_svc_fs* fs, dftu_ctx* ctx) noexcept
        : fs_(fs), ctx_(ctx) {}

    /// The slot a call fills; it must outlive the call's task.
    dftu_path_list** slot() noexcept { return &list_; }

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

    /// The bytes of the file read; empty until the call fills them.
    dstd::StringView view() const noexcept {
        return dstd::StringView(static_cast<const char*>(bytes_.data),
                                static_cast<std::size_t>(bytes_.len));
    }

   private:
    friend class Fs;

    /// The slot a call fills; it must outlive the call's task.
    dftu_bytes* slot() noexcept { return &bytes_; }

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
    PathList new_list() const { return PathList(fs_, ctx_); }

    AsyncOp read_file(dstd::CStringView path, FileBytes& out) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->read_file(ctx_, h_->h, path.c_str(), out.slot(), &t),
                    t);
    }
    AsyncOp write_file(dstd::CStringView path, dstd::ByteView data) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->write_file(ctx_, h_->h, path.c_str(), data.data(),
                                    data.size(), &t),
                    t);
    }
    AsyncOp read_at(dstd::CStringView path, std::uint64_t off,
                    dstd::MutableByteView buf, std::int64_t* out_n) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->read_at(ctx_, h_->h, path.c_str(), off, buf.data(),
                                 buf.size(), out_n, &t),
                    t);
    }
    AsyncOp write_at(dstd::CStringView path, std::uint64_t off,
                     dstd::ByteView buf, std::int64_t* out_n) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->write_at(ctx_, h_->h, path.c_str(), off, buf.data(),
                                  buf.size(), out_n, &t),
                    t);
    }
    AsyncOp list_dir(dstd::CStringView path, PathList& out) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->list_dir(ctx_, h_->h, path.c_str(), out.slot(), &t),
                    t);
    }
    /// `options` may be null for every file, recursive, with stat.
    AsyncOp find_all(dstd::CStringView root, const FindOptions* options,
                     PathList& out) const {
        if (!fs_) return unsupported();
        dftu_find_options raw{};
        detail::CStrArray<16> patterns;
        detail::CStrArray<16> skip_dirs;
        const dftu_find_options* given = nullptr;
        if (options != nullptr) {
            patterns.reset(options->patterns);
            skip_dirs.reset(options->skip_dirs);
            raw.struct_size = sizeof(raw);
            raw.pattern_count = static_cast<std::uint32_t>(patterns.count());
            raw.patterns = patterns.data();
            raw.skip_dir_count = static_cast<std::uint32_t>(skip_dirs.count());
            raw.skip_dirs = skip_dirs.data();
            raw.limit = options->limit;
            raw.recursive = options->recursive ? 1 : 0;
            raw.with_stat = options->with_stat ? 1 : 0;
            raw.skip_unreadable = options->skip_unreadable ? 1 : 0;
            given = &raw;
        }
        dftu_task* t = nullptr;
        return done(
            fs_->find_all(ctx_, h_->h, root.c_str(), given, out.slot(), &t), t);
    }
    AsyncOp exists(dstd::CStringView path, Flag& out) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->exists(ctx_, h_->h, path.c_str(), &out.raw_, &t), t);
    }
    AsyncOp create_directories(dstd::CStringView path, Flag& out) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(
            fs_->create_directories(ctx_, h_->h, path.c_str(), &out.raw_, &t),
            t);
    }
    AsyncOp remove(dstd::CStringView path, Flag& out) const {
        if (!fs_) return unsupported();
        dftu_task* t = nullptr;
        return done(fs_->remove(ctx_, h_->h, path.c_str(), &out.raw_, &t), t);
    }
    /// One stat and one status per path; a path that cannot be stat'ed fails
    /// only its own slot. `out` and `out_status` must hold `paths.size()`
    /// entries and outlive the call's task; a mismatch is an error here, since
    /// the host would write past the shorter one.
    AsyncOp stat_batch(dstd::Span<const dstd::CStringView> paths,
                       dstd::Span<FileStat> out,
                       dstd::Span<StatStatus> out_status) const {
        if (!fs_) return unsupported();
        if (out.size() != paths.size() || out_status.size() != paths.size()) {
            return AsyncOp{make_error(
                dstd::Condition::InvalidArgument,
                dstd::fmt<"stat_batch: {} paths need as many stat and status "
                          "slots, got {} and {}">(paths.size(), out.size(),
                                                  out_status.size()))};
        }
        detail::CStrArray<16> given;
        given.reset(paths);
        dftu_task* t = nullptr;
        return done(
            fs_->stat_batch(ctx_, h_->h, given.data(), paths.size(),
                            detail::FileStatAccess::raw(out),
                            detail::StatStatusAccess::raw(out_status), &t),
            t);
    }

   private:
    friend class Host;

    Fs(const dftu_plugin_host* h, dftu_ctx* ctx)
        : h_(h),
          ctx_(ctx),
          fs_(h && h->get_service ? static_cast<const dftu_svc_fs*>(
                                        h->get_service(h->h, DFTU_SVC_FS))
                                  : nullptr) {}

    static AsyncOp unsupported() {
        return AsyncOp{
            make_error(dstd::Condition::Unsupported, "host lacks dftu.svc.fs")};
    }
    // `t` by reference: callers write done(call(..., &t), t), and the order of
    // argument evaluation is unspecified, so a copy could predate the call.
    AsyncOp done(dftu_status st, dftu_task* const& t) const {
        if (st < 0) return AsyncOp{dataframe::error_of(st)};
        return detail::async_op(t, h_);
    }

    const dftu_plugin_host* h_;
    dftu_ctx* ctx_;
    const dftu_svc_fs* fs_;
};

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_FS_H */
