// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_FILEIO_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_FILEIO_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/async.h>
#include <dftracer/utils/plugins/plugin/map.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <coroutine>
#include <cstdint>
#include <utility>

namespace dftracer::utils::plugins {

/// An @ref AsyncOp whose awaited result is built from the task's status.
/// Awaiting it fails the @ref Task as the op does; `co_await v.expected()`
/// gives a @ref dstd::Result instead.
template <class T, class Finish>
class [[nodiscard]] AsyncValue {
   public:
    AsyncValue(AsyncOp op, Finish finish)
        : op_(std::move(op)), finish_(std::move(finish)) {}

    detail::Settled<AsyncValue> expected() & noexcept {
        return detail::Settled<AsyncValue>(*this);
    }
    detail::Settled<AsyncValue> expected() && noexcept {
        return detail::Settled<AsyncValue>(*this);
    }

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) noexcept {
        op_.await_suspend(h);
    }
    T await_resume() { return finish_(op_.await_resume()); }

    bool resolved() const noexcept { return op_.resolved(); }
    dstd::Result<T> resume_expected() {
        return op_.resume_expected().transform(
            [this](dftu_status status) { return finish_(status); });
    }

   private:
    AsyncOp op_;
    Finish finish_;
};

/// Up to `max_lines` lines of a @ref LineReader, borrowed from the host until
/// the next `next()` or the close of that reader.
class LineBatch {
   public:
    class Iterator {
       public:
        explicit Iterator(const dftu_bytes* p) : p_(p) {}
        dstd::StringView operator*() const {
            return dstd::StringView(static_cast<const char*>(p_->data),
                                    static_cast<std::size_t>(p_->len));
        }
        Iterator& operator++() {
            ++p_;
            return *this;
        }
        bool operator!=(const Iterator& other) const { return p_ != other.p_; }

       private:
        const dftu_bytes* p_;
    };

    explicit LineBatch(const dftu_line_batch& raw) : raw_(raw) {}

    /// The 1-based number of the first line.
    std::uint64_t first_line() const noexcept { return raw_.first_line; }
    std::size_t size() const noexcept { return raw_.count; }
    dstd::StringView operator[](std::size_t i) const {
        return *Iterator(raw_.lines + i);
    }
    Iterator begin() const { return Iterator(raw_.lines); }
    Iterator end() const { return Iterator(raw_.lines + raw_.count); }

   private:
    dftu_line_batch raw_;
};

/// Reads the lines of a plain or gzip file in batches. `co_await next()` gives
/// the next batch, or nullopt at the end of the file. Closes in the
/// destructor. One `next()` at a time.
class LineReader {
   public:
    LineReader() = default;
    LineReader(LineReader&& o) noexcept
        : h_(o.h_),
          ctx_(o.ctx_),
          e_(o.e_),
          r_(std::exchange(o.r_, nullptr)),
          slot_(std::move(o.slot_)) {}
    LineReader& operator=(LineReader&& o) noexcept {
        if (this != &o) {
            close();
            h_ = o.h_;
            ctx_ = o.ctx_;
            e_ = o.e_;
            r_ = std::exchange(o.r_, nullptr);
            slot_ = std::move(o.slot_);
        }
        return *this;
    }
    LineReader(const LineReader&) = delete;
    LineReader& operator=(const LineReader&) = delete;
    ~LineReader() { close(); }

    /// The batch is valid until the next `next()` or `close()`. A read error
    /// fails the await.
    auto next(std::uint32_t max_lines = 0) {
        dftu_task* t = nullptr;
        const dftu_status st =
            e_->lines_next(ctx_, h_->h, r_, max_lines, slot_.get(), &t);
        const dftu_line_batch* slot = slot_.get();
        auto finish = [slot](dftu_status status) {
            return status == DFTU_END
                       ? dstd::Optional<LineBatch>()
                       : dstd::Optional<LineBatch>(LineBatch(*slot));
        };
        return AsyncValue<dstd::Optional<LineBatch>, decltype(finish)>(
            detail::started(h_, st, t), finish);
    }

    void close() noexcept {
        if (r_ != nullptr) e_->lines_close(h_->h, r_);
        r_ = nullptr;
    }

    explicit operator bool() const noexcept { return r_ != nullptr; }

   private:
    friend class Fileio;
    LineReader(const dftu_plugin_host* h, dftu_ctx* ctx,
               const dftu_svc_fileio* e, dftu_line_reader* r)
        : h_(h),
          ctx_(ctx),
          e_(e),
          r_(r),
          slot_(dstd::make_unique<dftu_line_batch>()) {}

    const dftu_plugin_host* h_ = nullptr;
    dftu_ctx* ctx_ = nullptr;
    const dftu_svc_fileio* e_ = nullptr;
    dftu_line_reader* r_ = nullptr;
    dstd::UniquePtr<dftu_line_batch> slot_;
};

/// Reads a file in chunks. `co_await next()` gives the next chunk, borrowed
/// until the next `next()` or the close, or nullopt at the end of the file.
/// Closes in the destructor. One `next()` at a time.
class ChunkReader {
   public:
    ChunkReader() = default;
    ChunkReader(ChunkReader&& o) noexcept
        : h_(o.h_),
          ctx_(o.ctx_),
          e_(o.e_),
          r_(std::exchange(o.r_, nullptr)),
          slot_(std::move(o.slot_)) {}
    ChunkReader& operator=(ChunkReader&& o) noexcept {
        if (this != &o) {
            close();
            h_ = o.h_;
            ctx_ = o.ctx_;
            e_ = o.e_;
            r_ = std::exchange(o.r_, nullptr);
            slot_ = std::move(o.slot_);
        }
        return *this;
    }
    ChunkReader(const ChunkReader&) = delete;
    ChunkReader& operator=(const ChunkReader&) = delete;
    ~ChunkReader() { close(); }

    auto next() {
        dftu_task* t = nullptr;
        const dftu_status st =
            e_->chunks_next(ctx_, h_->h, r_, slot_.get(), &t);
        const dftu_bytes* slot = slot_.get();
        auto finish = [slot](dftu_status status) {
            using Chunk = dstd::Optional<dstd::StringView>;
            return status == DFTU_END
                       ? Chunk()
                       : Chunk(dstd::StringView(
                             static_cast<const char*>(slot->data),
                             static_cast<std::size_t>(slot->len)));
        };
        return AsyncValue<dstd::Optional<dstd::StringView>, decltype(finish)>(
            detail::started(h_, st, t), finish);
    }

    void close() noexcept {
        if (r_ != nullptr) e_->chunks_close(h_->h, r_);
        r_ = nullptr;
    }

    explicit operator bool() const noexcept { return r_ != nullptr; }

   private:
    friend class Fileio;
    ChunkReader(const dftu_plugin_host* h, dftu_ctx* ctx,
                const dftu_svc_fileio* e, dftu_chunk_reader* r)
        : h_(h),
          ctx_(ctx),
          e_(e),
          r_(r),
          slot_(dstd::make_unique<dftu_bytes>()) {}

    const dftu_plugin_host* h_ = nullptr;
    dftu_ctx* ctx_ = nullptr;
    const dftu_svc_fileio* e_ = nullptr;
    dftu_chunk_reader* r_ = nullptr;
    dstd::UniquePtr<dftu_bytes> slot_;
};

/// Options of @ref Fileio::gz; 0 is the library default.
struct GzOptions {
    std::int32_t level = 0;
    std::uint64_t member_size = 0;
    std::uint32_t workers = 0;
};

/// A gzip line writer. `co_await open()` once, then `append()` whole lines and
/// `co_await close()` for the summary. The host owns the writer: one that is
/// not closed is aborted at the end of the fold and its files are removed.
class GzWriter {
   public:
    GzWriter() = default;
    GzWriter(GzWriter&&) noexcept = default;
    GzWriter(const GzWriter&) = delete;
    GzWriter& operator=(const GzWriter&) = delete;

    /// The task that opens the file; co_await it before `append()`. It can be
    /// taken once.
    AsyncOp open() {
        AsyncOp op = std::move(*open_op_);
        open_op_.reset();
        return op;
    }

    /// Copies `lines`, which must end with '\n'; DFTU_E_INVALID otherwise.
    dstd::Result<void> append(dstd::StringView lines) const {
        return detail::reported(
            e_->gz_append(ctx_, h_->h, w_, lines.data(), lines.size()));
    }

    /// Finishes the file; the awaited value is its summary.
    auto close() const {
        dftu_task* t = nullptr;
        const dftu_status st =
            e_->gz_close(ctx_, h_->h, w_, summary_.get(), &t);
        const dftu_gz_summary* summary = summary_.get();
        auto finish = [summary](dftu_status) { return *summary; };
        return AsyncValue<dftu_gz_summary, decltype(finish)>(
            detail::started(h_, st, t), finish);
    }

    dftu_gz_writer* raw() const noexcept { return w_; }
    explicit operator bool() const noexcept { return w_ != nullptr; }

   private:
    friend class Fileio;
    GzWriter(const dftu_plugin_host* h, dftu_ctx* ctx, const dftu_svc_fileio* e,
             dftu_gz_writer* w, AsyncOp open_op)
        : h_(h),
          ctx_(ctx),
          e_(e),
          w_(w),
          summary_(dstd::make_unique<dftu_gz_summary>()) {
        open_op_.emplace(std::move(open_op));
    }

    const dftu_plugin_host* h_ = nullptr;
    dftu_ctx* ctx_ = nullptr;
    const dftu_svc_fileio* e_ = nullptr;
    dftu_gz_writer* w_ = nullptr;
    dstd::UniquePtr<dftu_gz_summary> summary_;
    dstd::Optional<AsyncOp> open_op_;
};

/// Typed wrapper over the fileio service (see abi/fileio.h). A call the host
/// refuses, or a host without the service, returns its error; one that starts
/// a task gives an @ref AsyncOp that fails when awaited, as does a failed task.
class Fileio {
   public:
    Fileio(const dftu_plugin_host* h, dftu_ctx* ctx)
        : h_(h),
          ctx_(ctx),
          e_(h && h->get_service ? static_cast<const dftu_svc_fileio*>(
                                       h->get_service(h->h, DFTU_SVC_FILEIO))
                                 : nullptr) {}

    /// Lines `start_line` to `end_line` (1-based; 0 is the first and the last
    /// line) of a plain or gzip file. DFTU_E_NOT_FOUND for a missing file.
    dstd::Result<LineReader> lines(const char* path,
                                   std::uint64_t start_line = 0,
                                   std::uint64_t end_line = 0) const {
        if (!e_) return unsupported();
        dftu_line_reader* r = nullptr;
        DFTRACER_UTILS_TRY_VOID(detail::reported(
            e_->lines_open(ctx_, h_->h, path, start_line, end_line, &r)));
        return LineReader(h_, ctx_, e_, r);
    }

    dstd::Result<ChunkReader> chunks(const char* path,
                                     std::uint64_t chunk_size = 0) const {
        if (!e_) return unsupported();
        dftu_chunk_reader* r = nullptr;
        DFTRACER_UTILS_TRY_VOID(detail::reported(
            e_->chunks_open(ctx_, h_->h, path, chunk_size, &r)));
        return ChunkReader(h_, ctx_, e_, r);
    }

    dstd::Result<GzWriter> gz(const char* path,
                              const GzOptions& options = {}) const {
        if (!e_) return unsupported();
        dftu_gz_options raw{sizeof(dftu_gz_options), options.level,
                            options.member_size, options.workers};
        dftu_gz_writer* w = nullptr;
        dftu_task* t = nullptr;
        DFTRACER_UTILS_TRY_VOID(
            detail::reported(e_->gz_open(ctx_, h_->h, path, &raw, &w, &t)));
        return GzWriter(h_, ctx_, e_, w, AsyncOp{t, h_});
    }

    /// `*out_size` receives the output size; it must outlive the await.
    AsyncOp compress(const char* in_path, const char* out_path,
                     std::uint64_t* out_size, std::int32_t level = 0,
                     std::uint64_t member_size = 0) const {
        if (!e_) return AsyncOp{unsupported()};
        dftu_task* t = nullptr;
        return detail::started(h_,
                               e_->compress(ctx_, h_->h, in_path, out_path,
                                            level, member_size, out_size, &t),
                               t);
    }
    AsyncOp decompress(const char* in_path, const char* out_path,
                       std::uint64_t* out_size) const {
        if (!e_) return AsyncOp{unsupported()};
        dftu_task* t = nullptr;
        return detail::started(
            h_, e_->decompress(ctx_, h_->h, in_path, out_path, out_size, &t),
            t);
    }

    /// The absolute normalized path; DFTU_E_NOT_FOUND for a missing file.
    dstd::Result<dstd::String> logical_path(const char* path) const {
        if (!e_) return unsupported();
        dftu_bytes out{};
        DFTRACER_UTILS_TRY_VOID(
            detail::reported(e_->logical_path(ctx_, h_->h, path, &out)));
        dstd::String text(static_cast<const char*>(out.data),
                          static_cast<std::size_t>(out.len));
        if (out.free_fn) out.free_fn(const_cast<void*>(out.data), out.ud);
        return text;
    }
    dstd::Result<std::int64_t> mtime(const char* path) const {
        if (!e_) return unsupported();
        std::int64_t seconds = 0;
        DFTRACER_UTILS_TRY_VOID(
            detail::reported(e_->mtime(ctx_, h_->h, path, &seconds)));
        return seconds;
    }
    dstd::Result<std::uint64_t> size(const char* path) const {
        if (!e_) return unsupported();
        std::uint64_t bytes = 0;
        DFTRACER_UTILS_TRY_VOID(
            detail::reported(e_->size(ctx_, h_->h, path, &bytes)));
        return bytes;
    }
    dstd::Result<std::uint64_t> sampled_hash(const char* path) const {
        if (!e_) return unsupported();
        std::uint64_t hash = 0;
        DFTRACER_UTILS_TRY_VOID(
            detail::reported(e_->sampled_hash(ctx_, h_->h, path, &hash)));
        return hash;
    }

   private:
    static dstd::Error unsupported() noexcept {
        return make_error(DFTU_E_UNSUPPORTED, "host lacks dftu.svc.fileio");
    }

    const dftu_plugin_host* h_;
    dftu_ctx* ctx_;
    const dftu_svc_fileio* e_;
};

inline Fileio Host::fileio() const { return Fileio{h_, ctx_}; }

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_FILEIO_H */
