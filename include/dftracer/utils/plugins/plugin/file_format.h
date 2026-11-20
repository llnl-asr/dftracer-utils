// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_FILE_FORMAT_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_FILE_FORMAT_H

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi/file_formats.h>
#include <dftracer/utils/plugins/plugin/frame.h>
#include <dftracer/utils/plugins/plugin/source.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace dftracer::utils::plugins {

/// A cursor over one chunk of a file. Derived by the plugin.
class FormatCursor {
   public:
    virtual ~FormatCursor() = default;
    /// The next frame, owned by the caller; dstd::nullopt at the end of the
    /// chunk, or the error that ended the read.
    virtual dstd::Result<dstd::Optional<OwnedDataFrame>> next(
        std::int64_t max_rows) = 0;
};

/// One open file of a format: the C++ side of dftu_file_format_vt's `file`
/// handle. Derived by the plugin.
class FormatFile {
   public:
    virtual ~FormatFile() = default;
    /// Append the file's columns, in order, to @p out. The host requires at
    /// least one.
    virtual void schema(SchemaBuilder& out) const = 0;
    /// The number of independently scannable chunks, at least 1. scan() runs
    /// once per chunk, in order.
    virtual std::uint64_t chunks() const = 0;
    /// Rows in @p chunk, or dstd::nullopt when that is not knowable without
    /// reading it. The host reports -1 for nullopt.
    virtual dstd::Optional<std::int64_t> chunk_rows(std::uint64_t chunk) const {
        (void)chunk;
        return dstd::nullopt;
    }
    /// A cursor over @p chunk honoring @p view. A non-empty view.projection
    /// means every frame the cursor yields carries exactly those columns, in
    /// that order.
    virtual dstd::Result<ScanResult<FormatCursor>> scan(
        std::uint64_t chunk, const ScanView& view) = 0;
};

/// A file format written as a C++ class. F derives from FileFormat and
/// provides what the class below declares, plus
///   dstd::Result<dstd::UniquePtr<FormatFile>> open(dstd::StringView path);
/// Register it with PluginBuilder::file_format<F>(). The extension hint of
/// dftu_file_format_vt stays NULL for a C++ format: a host matches on probe().
///
/// A format whose chunks have a stable identity (such as the byte offset of the
/// chunk's first row) names its file class and gives it a token per chunk:
///   using File = MyFile;  // derives from FormatFile, and open() returns it
///   dstd::Result<std::uint64_t> MyFile::chunk_token(std::uint64_t chunk);
/// The host then sets dftu_file_format_vt::chunk_token, which an index of the
/// file needs to skip chunks; tokens must rise strictly with the chunk number.
/// A format without both stays one granule per file to the index.
///
/// As everywhere else in the SDK, a callback reports failure as a dstd::Result
/// or a std::int32_t score, and the trampolines hold no catch, so a callback
/// that throws terminates.
class FileFormat {
   public:
    virtual ~FileFormat() = default;
    /// How well the first bytes of a file match this format: 0 (not mine) to
    /// 100 (certain). @p head is the file's first bytes, borrowed for the call
    /// and possibly empty; @p path is the file's path, borrowed for the call,
    /// for a format that decides by name. Called without a host, from any
    /// thread.
    virtual std::int32_t probe(dstd::ByteView head, dstd::CStringView path) = 0;
    /// Open `path`; the returned file is closed when the last plan or cursor
    /// holding it is gone.
    virtual dstd::Result<dstd::UniquePtr<FormatFile>> open(
        dstd::StringView path) = 0;
};

/// What one bound output is for.
struct BindView {
    dstd::Vector<dstd::StringView> names;  ///< output columns, in order
    dstd::Vector<TypeId> types;            ///< one type per name, same order
    /// The caller's config for this output; a null @ref Config when it gave
    /// none.
    Config options;
};

/// A writer for one bound output. Derived by the plugin.
class SinkWriter {
   public:
    virtual ~SinkWriter() = default;
    /// Write one frame. @p batch is borrowed for the call, and every frame of
    /// one output carries the bound columns in that order.
    virtual dstd::Result<void> write(FrameView batch) = 0;
    /// Finish the output. Called once, after the last write().
    virtual dstd::Result<void> finalize() = 0;
    /// Give up on the output. Called once instead of finalize(), and never
    /// beside it.
    virtual void abort() {}
};

/// A sink written as a C++ class. Register it with PluginBuilder::sink<S>().
/// Nothing calls a registered sink yet.
class Sink {
   public:
    virtual ~Sink() = default;
    /// Bind one output to `view`; the writer's destructor releases it.
    virtual dstd::Result<dstd::UniquePtr<SinkWriter>> bind(
        const BindView& view) = 0;
};

namespace detail {

struct FormatBox {
    dstd::UniquePtr<FileFormat> format;
};

struct FormatFileBox {
    dstd::UniquePtr<FormatFile> file;
};

struct SinkBox {
    dstd::UniquePtr<Sink> sink;
};

struct SinkWriterBox {
    dstd::UniquePtr<SinkWriter> writer;
};

template <class F>
concept HasChunkToken = requires(typename F::File& file, std::uint64_t chunk) {
    requires std::is_base_of_v<FormatFile, typename F::File>;
    { file.chunk_token(chunk) } -> std::same_as<dstd::Result<std::uint64_t>>;
};

/// A file that says what scan() of a chunk would do for a request:
///   using File = MyFile;  // derives from FormatFile, and open() returns it
///   dstd::Result<void> MyFile::explain(const ScanView&, const ExplainOut&);
/// The host then sets dftu_file_format_vt::explain and prints its lines after
/// its own (the chunks the index keeps).
template <class F>
concept HasFileExplain = requires(typename F::File& file, const ScanView& view,
                                  const ExplainOut& out) {
    requires std::is_base_of_v<FormatFile, typename F::File>;
    { file.explain(view, out) } -> std::same_as<dstd::Result<void>>;
};

/// A file that plans a scan of its chunks before the first one opens:
///   using File = MyFile;  // derives from FormatFile, and open() returns it
///   dstd::Result<PlanResult> MyFile::plan(const ScanView&);
/// The host then sets dftu_file_format_vt::plan; the votes and the token hold
/// for every chunk of the scan (see @ref dftracer::utils::plugins::PlanResult).
template <class F>
concept HasFilePlan = requires(typename F::File& file, const ScanView& view) {
    requires std::is_base_of_v<FormatFile, typename F::File>;
    { file.plan(view) } -> std::same_as<dstd::Result<PlanResult>>;
};

/// As HasFilePlan, for a plan that reads the filters as one duql text:
///   dstd::Result<PlanResult> MyFile::plan_duql(const ScanView&,
///       dstd::StringView text, dstd::Span<const std::int32_t> filter_index);
template <class F>
concept HasFilePlanDuql =
    requires(typename F::File& file, const ScanView& view,
             dstd::StringView text, dstd::Span<const std::int32_t> index) {
        requires std::is_base_of_v<FormatFile, typename F::File>;
        {
            file.plan_duql(view, text, index)
        } -> std::same_as<dstd::Result<PlanResult>>;
    };

template <class F>
struct FileFormatVt {
    using Box = FormatBox;

    static std::int32_t probe(void* self, const std::uint8_t* head,
                              std::uint64_t n, const char* path) noexcept {
        return static_cast<Box*>(self)->format->probe(
            dstd::ByteView(head, static_cast<std::size_t>(n)),
            dstd::CStringView(path != nullptr ? path : ""));
    }

    static dftu_status open(dftu_ctx* ctx, void* self, const char* path,
                            void** file) noexcept {
        return to_status(ctx, [&]() -> dftu_status {
            auto opened =
                static_cast<Box*>(self)->format->open(path ? path : "");
            if (!opened.has_value()) return report(ctx, opened.error());
            if (!*opened)
                return dftu_fail(ctx, DFTU_E_INTERNAL, "open returned no file");
            *file = new FormatFileBox{std::move(*opened)};
            return DFTU_OK;
        });
    }

    static dftu_status schema(dftu_ctx* ctx, void* file,
                              dftu_schema* out) noexcept {
        return to_status(ctx, [&]() -> dftu_status {
            SchemaBuilder builder(out);
            static_cast<FormatFileBox*>(file)->file->schema(builder);
            return DFTU_OK;
        });
    }

    static dftu_status chunks(dftu_ctx* ctx, void* file,
                              std::uint64_t* out) noexcept {
        return to_status(ctx, [&]() -> dftu_status {
            *out = static_cast<FormatFileBox*>(file)->file->chunks();
            return DFTU_OK;
        });
    }

    static dftu_status chunk_stats(dftu_ctx* ctx, void* file,
                                   std::uint64_t chunk,
                                   dftu_chunk_stats* out) noexcept {
        return to_status(ctx, [&]() -> dftu_status {
            const auto rows =
                static_cast<FormatFileBox*>(file)->file->chunk_rows(chunk);
            out->rows = rows.has_value() ? *rows : -1;
            return DFTU_OK;
        });
    }

    static dftu_status scan(dftu_ctx* ctx, void* file, std::uint64_t chunk,
                            const dftu_plugin_host* host,
                            const dftu_scan_request* req,
                            std::int32_t* out_pushed, void** out_cursor_self,
                            const dftu_cursor_vt** out_vt) noexcept {
        return to_status(ctx, [&]() -> dftu_status {
            const ScanView view = scan_view_of(*req);
            auto scanned =
                static_cast<FormatFileBox*>(file)->file->scan(chunk, view);
            if (!scanned.has_value()) return report(ctx, scanned.error());
            auto& result = *scanned;
            if (!result.cursor)
                return dftu_fail(ctx, DFTU_E_INTERNAL,
                                 "scan returned no cursor");
            const std::size_t n =
                std::min<std::size_t>(result.filters.size(),
                                      static_cast<std::size_t>(req->n_filters));
            for (std::size_t i = 0; i < n; ++i)
                out_pushed[i] = static_cast<std::int32_t>(result.filters[i]);
            *out_cursor_self =
                new CursorBox<FormatCursor>{std::move(result.cursor), host};
            *out_vt = CursorVt<FormatCursor>::vt();
            return DFTU_OK;
        });
    }

    static dftu_status chunk_token(dftu_ctx* ctx, void* file,
                                   std::uint64_t chunk,
                                   std::uint64_t* token) noexcept {
        if constexpr (HasChunkToken<F>) {
            return to_status(ctx, [&]() -> dftu_status {
                auto got = static_cast<typename F::File*>(
                               static_cast<FormatFileBox*>(file)->file.get())
                               ->chunk_token(chunk);
                if (!got.has_value()) return report(ctx, got.error());
                *token = *got;
                return DFTU_OK;
            });
        } else {
            (void)ctx;
            (void)file;
            (void)chunk;
            (void)token;
            return DFTU_E_INTERNAL;
        }
    }

    static dftu_status explain(dftu_ctx* ctx, void* file,
                               const dftu_scan_request* req,
                               const dftu_explain_out* out) noexcept {
        if constexpr (HasFileExplain<F>) {
            return to_status(ctx, [&]() {
                return static_cast<typename F::File*>(
                           static_cast<FormatFileBox*>(file)->file.get())
                    ->explain(scan_view_of(*req), ExplainOut(out));
            });
        } else {
            (void)ctx;
            (void)file;
            (void)req;
            (void)out;
            return DFTU_E_INTERNAL;
        }
    }

    static dftu_status plan(dftu_ctx* ctx, void* file,
                            const dftu_scan_request* req,
                            dftu_plan_result* out) noexcept {
        if constexpr (HasFilePlan<F>) {
            return to_status(ctx, [&]() -> dftu_status {
                auto planned =
                    static_cast<typename F::File*>(
                        static_cast<FormatFileBox*>(file)->file.get())
                        ->plan(scan_view_of(*req));
                if (!planned.has_value()) return report(ctx, planned.error());
                write_plan(*planned, *req, out);
                return DFTU_OK;
            });
        } else {
            (void)ctx;
            (void)file;
            (void)req;
            (void)out;
            return DFTU_E_INTERNAL;
        }
    }

    static dftu_status plan_duql(dftu_ctx* ctx, void* file,
                                 const dftu_scan_request* req,
                                 const dftu_plan_duql* q,
                                 dftu_plan_result* out) noexcept {
        if constexpr (HasFilePlanDuql<F>) {
            return to_status(ctx, [&]() -> dftu_status {
                auto planned =
                    static_cast<typename F::File*>(
                        static_cast<FormatFileBox*>(file)->file.get())
                        ->plan_duql(
                            scan_view_of(*req),
                            dstd::StringView(q->text, q->text_len),
                            dstd::Span<const std::int32_t>(
                                q->filter_index, abi_count(q->n_filters)));
                if (!planned.has_value()) return report(ctx, planned.error());
                write_plan(*planned, *req, out);
                return DFTU_OK;
            });
        } else {
            (void)ctx;
            (void)file;
            (void)req;
            (void)q;
            (void)out;
            return DFTU_E_INTERNAL;
        }
    }

    static void close(void* file) { delete static_cast<FormatFileBox*>(file); }

    static void destroy(void* self) { delete static_cast<Box*>(self); }

    static const dftu_file_format_vt* vt() {
        static const dftu_file_format_vt VT = [] {
            dftu_file_format_vt t{};
            t.struct_size = sizeof(dftu_file_format_vt);
            t.probe = probe;
            t.open = open;
            t.schema = schema;
            t.chunks = chunks;
            t.chunk_stats = chunk_stats;
            t.scan = scan;
            t.close = close;
            t.destroy = destroy;
            if constexpr (HasChunkToken<F>) t.chunk_token = chunk_token;
            if constexpr (HasFileExplain<F>) t.explain = explain;
            if constexpr (HasFilePlan<F>) t.plan = plan;
            if constexpr (HasFilePlanDuql<F>) t.plan_duql = plan_duql;
            return t;
        }();
        return &VT;
    }
};

template <class S>
struct SinkVt {
    using Box = SinkBox;
    using WriterBox = SinkWriterBox;

    static dftu_status bind(dftu_ctx* ctx, void* self, const char* const* names,
                            const std::int32_t* types, std::uint32_t n_columns,
                            const dftu_value* options, void** sink) noexcept {
        return to_status(ctx, [&]() -> dftu_status {
            BindView view;
            view.names.reserve(n_columns);
            view.types.reserve(n_columns);
            for (std::uint32_t i = 0; i < n_columns; ++i) {
                view.names.emplace_back(names && names[i] ? names[i] : "");
                view.types.push_back(types != nullptr
                                         ? static_cast<TypeId>(types[i])
                                         : TypeId::Unknown);
            }
            view.options = detail::config_from_raw(options);
            auto writer = static_cast<Box*>(self)->sink->bind(view);
            if (!writer.has_value()) return report(ctx, writer.error());
            if (!*writer)
                return dftu_fail(ctx, DFTU_E_INTERNAL,
                                 "bind returned no writer");
            *sink = new WriterBox{std::move(*writer)};
            return DFTU_OK;
        });
    }

    static dftu_status write(dftu_ctx* ctx, void* sink,
                             const dftu_dataframe* batch) noexcept {
        return to_status(ctx, [&] {
            return static_cast<WriterBox*>(sink)->writer->write(
                FrameView(batch));
        });
    }

    static dftu_status finalize(dftu_ctx* ctx, void* sink) noexcept {
        return to_status(ctx, [&] {
            WriterBox* box = static_cast<WriterBox*>(sink);
            dstd::Result<void> done = box->writer->finalize();
            delete box;
            return done;
        });
    }

    static void abort(void* sink) {
        WriterBox* box = static_cast<WriterBox*>(sink);
        box->writer->abort();
        delete box;
    }

    static void destroy(void* self) { delete static_cast<Box*>(self); }

    static const dftu_sink_vt* vt() {
        static const dftu_sink_vt VT = [] {
            dftu_sink_vt t{};
            t.struct_size = sizeof(dftu_sink_vt);
            t.bind = bind;
            t.write = write;
            t.finalize = finalize;
            t.abort = abort;
            t.destroy = destroy;
            return t;
        }();
        return &VT;
    }
};

}  // namespace detail

/// The C vtable for the file format F. A `self` for it is made by
/// make_file_format_self().
template <class F>
const dftu_file_format_vt* file_format_vtable() {
    static_assert(std::is_base_of_v<FileFormat, F>,
                  "F must derive from dftracer::utils::plugins::FileFormat");
    return detail::FileFormatVt<F>::vt();
}

/// Wrap `format` as the `self` its vtable expects. Ownership passes to whoever
/// calls the vtable's destroy() on it.
template <class F>
void* make_file_format_self(dstd::UniquePtr<F> format) {
    return new detail::FormatBox{std::move(format)};
}

/// The C vtable for the sink S. A `self` for it is made by make_sink_self().
template <class S>
const dftu_sink_vt* sink_vtable() {
    static_assert(std::is_base_of_v<Sink, S>,
                  "S must derive from dftracer::utils::plugins::Sink");
    return detail::SinkVt<S>::vt();
}

/// Wrap `sink` as the `self` its vtable expects. Ownership passes to whoever
/// calls the vtable's destroy() on it.
template <class S>
void* make_sink_self(dstd::UniquePtr<S> sink) {
    return new detail::SinkBox{std::move(sink)};
}

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_PLUGIN_FILE_FORMAT_H
