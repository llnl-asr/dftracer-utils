// SPDX-License-Identifier: MIT
// The adapter from a registered dftu_file_format_vt to the dataframe
// FileFormatFactory, Source and Cursor interfaces, and from a dftu_sink_vt to
// SinkFactory. The frame marshalling and checks a plugin cursor needs are the
// provider ones (provider_source.h), shared rather than copied.

#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>
#include <dftracer/utils/dataframe/internal/file_format_registry.h>
#include <dftracer/utils/dataframe/internal/schema_types.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi/file_formats.h>
#include <dftracer/utils/plugins/abi/value.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/cursor_host.h>
#include <dftracer/utils/plugins/host/file_format_source.h>
#include <dftracer/utils/plugins/host/plugin_abi.h>
#include <dftracer/utils/plugins/host/plugin_task.h>
#include <dftracer/utils/plugins/host/provider_source.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace dftracer::utils::plugins::host {
namespace {

// The vtable, `self` and `name` of one file format, shared by the registry
// entry and every file opened through it. `self` is destroyed once, when the
// last of them is gone, if this handle owns it.
struct FormatHandle {
    // First, so it is released after destroy() has run.
    dstd::SharedPtr<PluginLibrary> library;
    dstd::SharedPtr<PluginMemory> memory;
    dstd::String name;
    ::dftu_file_format_vt vt;
    void* self;
    bool owned;

    FormatHandle(dstd::SharedPtr<PluginLibrary> l,
                 dstd::SharedPtr<PluginMemory> m, dstd::StringView n,
                 const ::dftu_file_format_vt& v, void* s, bool o)
        : library(std::move(l)),
          memory(std::move(m)),
          name(n),
          vt(v),
          self(s),
          owned(o) {}
    FormatHandle(const FormatHandle&) = delete;
    FormatHandle& operator=(const FormatHandle&) = delete;
    ~FormatHandle() {
        if (owned && vt.destroy) vt.destroy(self);
    }

    void close_file(void* file) const {
        if (file && vt.close) vt.close(file);
    }
};

// One open file of a format: the plan and every cursor scanning it hold this,
// so the handle stays open until the last of them is gone. The file is closed
// before the format handle, and with it the plugin library, is released.
struct FormatFile {
    dstd::SharedPtr<FormatHandle> handle;
    void* file;
    std::uint64_t chunks;
    dataframe::Schema schema;

    FormatFile(dstd::SharedPtr<FormatHandle> h, void* f, std::uint64_t c,
               dataframe::Schema s)
        : handle(std::move(h)), file(f), chunks(c), schema(std::move(s)) {}
    FormatFile(const FormatFile&) = delete;
    FormatFile& operator=(const FormatFile&) = delete;
    ~FormatFile() {
        if (file) handle->close_file(file);
    }
};

// One chunk's scan, already opened by the format.
struct OpenedChunk {
    dstd::UniquePtr<CursorHost> host;
    void* cursor_self = nullptr;
    const ::dftu_cursor_vt* vt = nullptr;
};

class FormatCursor;

class FormatSource final : public dataframe::Source,
                           public std::enable_shared_from_this<FormatSource> {
   public:
    explicit FormatSource(dstd::SharedPtr<FormatFile> file)
        : file_(std::move(file)) {}
    FormatSource(const FormatSource&) = delete;
    FormatSource& operator=(const FormatSource&) = delete;

    dataframe::Schema schema() const override { return file_->schema; }

    dstd::Result<dataframe::ScanResult> scan(
        const dataframe::ScanRequest& req) const override;

   private:
    friend class FormatCursor;

    // Chunk 0's votes are the scan's; every later chunk must agree with them,
    // so a filter counts as pushed only when every chunk voted it.
    dstd::Result<OpenedChunk> scan_chunk(
        std::uint64_t chunk, const dataframe::ScanRequest& req,
        dstd::Vector<dataframe::Pushed>* votes,
        const dstd::Vector<dataframe::Pushed>* expect) const;

    dstd::SharedPtr<FormatFile> file_;
};

// Walks the chunks of one file in order: the cursor of chunk 0, then the one
// chunk 1 opens when chunk 0 ends, and so on. Each chunk gets its own cursor
// from the format, whose frames go through the provider frame checks.
class FormatCursor final : public dataframe::Cursor {
   public:
    FormatCursor(dstd::SharedPtr<FormatFile> file,
                 dstd::SharedPtr<const dataframe::Source> source,
                 dataframe::ScanRequest req,
                 dstd::Vector<dataframe::Pushed> votes,
                 dstd::Vector<dataframe::TypeId> expected_types,
                 OpenedChunk first)
        : source_(std::move(source)),
          file_(std::move(file)),
          req_(std::move(req)),
          votes_(std::move(votes)),
          expected_types_(std::move(expected_types)) {
        adopt(std::move(first));
    }

    FormatCursor(const FormatCursor&) = delete;
    FormatCursor& operator=(const FormatCursor&) = delete;

    coro::CoroTask<dataframe::MorselResult> next(
        std::int64_t max_rows) override {
        while (cursor_) {
            dataframe::MorselResult read = co_await cursor_->next(max_rows);
            if (!read) co_return std::move(read);
            if (read->has_value()) co_return std::move(read);
            dstd::Optional<dstd::Error> failed = open_next_chunk();
            if (failed) co_return *failed;
        }
        co_return dataframe::MorselResult(dstd::nullopt);
    }

    bool try_next(std::int64_t max_rows,
                  dataframe::MorselResult& out) override {
        while (cursor_) {
            if (!cursor_->try_next(max_rows, out)) return false;
            if (!out) return true;
            if (out->has_value()) return true;
            dstd::Optional<dstd::Error> failed = open_next_chunk();
            if (failed) {
                out = *failed;
                return true;
            }
        }
        out = dataframe::MorselResult(dstd::nullopt);
        return true;
    }

   private:
    void adopt(OpenedChunk chunk) {
        host_ = std::move(chunk.host);
        cursor_ = make_plugin_cursor(
            chunk.vt, chunk.cursor_self, std::move(host_), req_.projection,
            expected_types_, source_,
            dstd::fmt<"file format '{}' next">(file_->handle->name));
    }

    // Releases the chunk just ended and opens the next one. No error means
    // either the next chunk is ready or the file is done (cursor_ null).
    dstd::Optional<dstd::Error> open_next_chunk() {
        cursor_.reset();
        const auto* source = static_cast<const FormatSource*>(source_.get());
        if (next_chunk_ >= file_->chunks) return dstd::nullopt;
        auto opened = source->scan_chunk(next_chunk_++, req_, nullptr, &votes_);
        if (!opened) return opened.error();
        adopt(std::move(*opened));
        return dstd::nullopt;
    }

    // Before the file, so the plugin's cursors are destroyed before it closes.
    dstd::SharedPtr<const dataframe::Source> source_;
    dstd::SharedPtr<FormatFile> file_;
    dataframe::ScanRequest req_;
    dstd::Vector<dataframe::Pushed> votes_;
    dstd::Vector<dataframe::TypeId> expected_types_;
    dstd::UniquePtr<CursorHost> host_;
    // The cursor of the chunk being read; null when the file is done.
    dstd::UniquePtr<dataframe::Cursor> cursor_;
    std::uint64_t next_chunk_ = 1;
};

class VtableFormat final : public dataframe::FileFormatFactory {
   public:
    explicit VtableFormat(dstd::SharedPtr<FormatHandle> handle)
        : handle_(std::move(handle)) {}

    dstd::StringView name() const override { return handle_->name; }

    int probe(const std::uint8_t* head, std::uint64_t n,
              dstd::StringView path) const override {
        const dstd::String path_str(path);
        return handle_->vt.probe(handle_->self, head, n, path_str.c_str());
    }

    dstd::Result<dstd::SharedPtr<const dataframe::Source>> open(
        dstd::StringView path) override {
        const dstd::String path_str(path);
        void* file = nullptr;
        plugins::AbiContext ctx;
        dftu_status status =
            handle_->vt.open(ctx.get(), handle_->self, path_str.c_str(), &file);
        if (status < 0) return plugin_error(ctx, status, "file format open");
        if (!file)
            return dstd::Error::from_format<
                "file format '{}': open returned no file handle">(
                dstd::Condition::Internal, handle_->name);

        ::dftu_schema declared;
        plugins::AbiContext schema_ctx;
        status = handle_->vt.schema(schema_ctx.get(), file, &declared);
        if (status < 0) {
            handle_->close_file(file);
            return plugin_error(schema_ctx, status, "file format schema");
        }
        if (declared.top.empty()) {
            handle_->close_file(file);
            return dstd::Error::from_format<
                "file format '{}': it declared no columns">(
                dstd::Condition::Internal, handle_->name);
        }
        dataframe::Schema schema;
        schema.fields = std::move(declared.top);

        std::uint64_t chunks = 0;
        plugins::AbiContext chunks_ctx;
        status = handle_->vt.chunks(chunks_ctx.get(), file, &chunks);
        if (status < 0) {
            handle_->close_file(file);
            return plugin_error(chunks_ctx, status, "file format chunks");
        }
        if (chunks == 0) {
            handle_->close_file(file);
            return dstd::Error::from_format<
                "file format '{}': it reported 0 chunks, and a file has at "
                "least 1">(dstd::Condition::Internal, handle_->name);
        }

        auto opened = dstd::make_shared<FormatFile>(handle_, file, chunks,
                                                    std::move(schema));
        return dstd::SharedPtr<const dataframe::Source>(
            dstd::make_shared<FormatSource>(std::move(opened)));
    }

   private:
    dstd::SharedPtr<FormatHandle> handle_;
};

dstd::Result<OpenedChunk> FormatSource::scan_chunk(
    std::uint64_t chunk, const dataframe::ScanRequest& req,
    dstd::Vector<dataframe::Pushed>* votes,
    const dstd::Vector<dataframe::Pushed>* expect) const {
    const ::dftu_file_format_vt& vt = file_->handle->vt;

    dstd::Vector<const char*> projection;
    projection.reserve(req.projection.size());
    for (const dstd::String& name : req.projection)
        projection.push_back(name.c_str());

    dstd::Vector<dftu_expr*> owned_filters;
    owned_filters.reserve(req.filters.size());
    dstd::Vector<const dftu_expr*> filters;
    filters.reserve(req.filters.size());
    for (const dataframe::Expr& f : req.filters) {
        dftu_expr* h = dataframe::expr_handle_wrap(f);
        owned_filters.push_back(h);
        filters.push_back(h);
    }

    ::dftu_scan_request creq{};
    creq.projection = projection.empty() ? nullptr : projection.data();
    creq.n_projection = static_cast<std::int32_t>(projection.size());
    creq.filters = filters.empty() ? nullptr : filters.data();
    creq.n_filters = static_cast<std::int32_t>(filters.size());
    creq.limit = req.limit;
    creq.memory_budget = req.memory_budget;

    // Pre-filled DFTU_PUSHED_NO: a format that writes nothing, or writes only
    // some entries, degrades to "the engine re-applies", which is always sound.
    dstd::Vector<std::int32_t> out_pushed(filters.size(), DFTU_PUSHED_NO);

    auto host = dstd::make_unique<CursorHost>(file_->handle->memory,
                                              "file format", req.memory_budget);
    void* cursor_self = nullptr;
    const ::dftu_cursor_vt* cursor_vt = nullptr;
    plugins::AbiContext ctx;
    const dftu_status status =
        vt.scan(ctx.get(), file_->file, chunk, host->host(), &creq,
                out_pushed.data(), &cursor_self, &cursor_vt);

    for (dftu_expr* h : owned_filters) dftu_expr_free(h);

    if (status < 0) return plugin_error(ctx, status, "file format scan");

    // A cursor trusts `host` until its destroy() returns, so every error after
    // a successful scan destroys the cursor first and releases the host after.
    const bool cursor_ok =
        cursor_vt &&
        plugin_struct_size_ok(
            PluginStruct::CursorVt, cursor_vt->struct_size,
            static_cast<std::uint32_t>(sizeof(::dftu_cursor_vt))) &&
        cursor_vt->next;
    auto discard_cursor = [&] {
        if (cursor_vt &&
            plugin_struct_size_ok(
                PluginStruct::CursorVt, cursor_vt->struct_size,
                static_cast<std::uint32_t>(sizeof(::dftu_cursor_vt))) &&
            cursor_vt->destroy)
            cursor_vt->destroy(cursor_self);
        host.reset();
    };

    if (!cursor_ok) {
        discard_cursor();
        return dstd::Error::from_format<
            "file format '{}': chunk {} gave no valid cursor vtable">(
            dstd::Condition::Internal, file_->handle->name, chunk);
    }

    dstd::Vector<dataframe::Pushed> chunk_votes;
    chunk_votes.reserve(out_pushed.size());
    for (std::int32_t v : out_pushed) chunk_votes.push_back(plugin_pushed(v));
    if (expect && *expect != chunk_votes) {
        discard_cursor();
        return dstd::Error::from_format<
            "file format '{}': chunk {} reported other pushed filters than "
            "chunk 0; every chunk of one scan must report the same votes">(
            dstd::Condition::InvalidArgument, file_->handle->name, chunk);
    }
    if (votes) *votes = std::move(chunk_votes);

    return OpenedChunk{std::move(host), cursor_self, cursor_vt};
}

dstd::Result<dataframe::ScanResult> FormatSource::scan(
    const dataframe::ScanRequest& req) const {
    dstd::Vector<dataframe::Pushed> votes;
    auto first = scan_chunk(0, req, &votes, nullptr);
    if (!first) return first.error();
    auto expected_types = declared_types(file_->schema, req.projection);
    dataframe::ScanResult r;
    r.filters = votes;
    r.cursor = dstd::make_unique<FormatCursor>(
        file_, shared_from_this(), req, std::move(votes),
        std::move(expected_types), std::move(*first));
    return r;
}

// The vtable and `self` of one sink, shared by the registry entry and every
// output bound from it.
struct SinkHandle {
    // First, so it is released after destroy() has run.
    dstd::SharedPtr<PluginLibrary> library;
    dstd::String name;
    ::dftu_sink_vt vt;
    void* self;
    bool owned;

    SinkHandle(dstd::SharedPtr<PluginLibrary> l, dstd::StringView n,
               const ::dftu_sink_vt& v, void* s, bool o)
        : library(std::move(l)), name(n), vt(v), self(s), owned(o) {}
    SinkHandle(const SinkHandle&) = delete;
    SinkHandle& operator=(const SinkHandle&) = delete;
    ~SinkHandle() {
        if (owned && vt.destroy) vt.destroy(self);
    }
};

// The options tree one bind() call lends its plugin: every string stays put and
// NUL terminated while the call runs.
class OptionsTree {
   public:
    explicit OptionsTree(
        const dstd::Vector<dstd::Pair<dstd::String, dstd::String>>& options) {
        const std::size_t n = options.size();
        strings_.reserve(2 * n);
        for (const dstd::Pair<dstd::String, dstd::String>& option : options) {
            strings_.push_back(option.first);
            strings_.push_back(option.second);
        }
        values_.resize(n);
        members_.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const dstd::String& key = strings_[2 * i];
            const dstd::String& value = strings_[2 * i + 1];
            values_[i].kind = DFTU_VAL_STR;
            values_[i].count = static_cast<std::uint32_t>(value.size());
            values_[i].as.str = value.c_str();
            members_[i].key = key.c_str();
            members_[i].key_len = static_cast<std::uint32_t>(key.size());
            members_[i].value = &values_[i];
        }
        root_.kind = DFTU_VAL_OBJECT;
        root_.count = static_cast<std::uint32_t>(n);
        root_.as.members = members_.empty() ? nullptr : members_.data();
    }

    /// NULL when the caller gave no options.
    const ::dftu_value* root() const {
        return members_.empty() ? nullptr : &root_;
    }

   private:
    dstd::Vector<dstd::String> strings_;
    dstd::Vector<::dftu_value> values_;
    dstd::Vector<::dftu_member> members_;
    ::dftu_value root_{};
};

class VtableSinkWriter final : public dataframe::SinkWriter {
   public:
    VtableSinkWriter(dstd::SharedPtr<SinkHandle> handle, void* sink)
        : handle_(std::move(handle)), sink_(sink) {}
    VtableSinkWriter(const VtableSinkWriter&) = delete;
    VtableSinkWriter& operator=(const VtableSinkWriter&) = delete;
    // An output that never reached finalize() or abort() is abandoned here, so
    // the plugin releases the handle exactly once either way.
    ~VtableSinkWriter() override {
        if (sink_ && handle_->vt.abort) handle_->vt.abort(sink_);
    }

    dstd::Result<void> write(const dataframe::DataFrame& batch) override {
        // A borrowed frame: the columns are shared with `batch`, and the handle
        // is freed before this returns, so nothing keeps it past the call.
        dataframe::DataFrame shared;
        shared.names = batch.names;
        shared.columns.reserve(batch.columns.size());
        for (const dataframe::Series& column : batch.columns)
            shared.columns.push_back(column.share());
        ::dftu_dataframe* frame =
            dataframe::dataframe_handle_wrap(std::move(shared));
        plugins::AbiContext ctx;
        const dftu_status status = handle_->vt.write(ctx.get(), sink_, frame);
        ::dftu_dataframe_free(frame);
        if (status < 0) return plugin_error(ctx, status, "sink write");
        return {};
    }

    dstd::Result<void> finalize() override {
        // finalize() and abort() are each once per output: a second one is the
        // caller's error, never a NULL handle passed to the plugin.
        if (!sink_)
            return dstd::Error::from_format<
                "sink '{}': finalize after finalize or abort">(
                dstd::Condition::InvalidArgument, handle_->name);
        plugins::AbiContext ctx;
        void* sink = std::exchange(sink_, nullptr);
        const dftu_status status = handle_->vt.finalize(ctx.get(), sink);
        if (status < 0) return plugin_error(ctx, status, "sink finalize");
        return {};
    }

    void abort() override {
        void* sink = std::exchange(sink_, nullptr);
        if (sink && handle_->vt.abort) handle_->vt.abort(sink);
    }

   private:
    dstd::SharedPtr<SinkHandle> handle_;
    void* sink_;
};

class VtableSink final : public dataframe::SinkFactory {
   public:
    explicit VtableSink(dstd::SharedPtr<SinkHandle> handle)
        : handle_(std::move(handle)) {}

    dstd::StringView name() const override { return handle_->name; }

    dstd::Result<dstd::UniquePtr<dataframe::SinkWriter>> bind(
        const dstd::Vector<dstd::String>& names,
        const dstd::Vector<dataframe::TypeId>& types,
        const dstd::Vector<dstd::Pair<dstd::String, dstd::String>>& options)
        override {
        dstd::Vector<const char*> name_ptrs;
        name_ptrs.reserve(names.size());
        for (const dstd::String& name : names)
            name_ptrs.push_back(name.c_str());
        dstd::Vector<std::int32_t> type_codes;
        type_codes.reserve(types.size());
        for (dataframe::TypeId id : types)
            type_codes.push_back(static_cast<std::int32_t>(id));
        const OptionsTree tree(options);

        void* sink = nullptr;
        plugins::AbiContext ctx;
        const dftu_status status = handle_->vt.bind(
            ctx.get(), handle_->self,
            name_ptrs.empty() ? nullptr : name_ptrs.data(),
            type_codes.empty() ? nullptr : type_codes.data(),
            static_cast<std::uint32_t>(name_ptrs.size()), tree.root(), &sink);
        if (status < 0) return plugin_error(ctx, status, "sink bind");
        if (!sink)
            return dstd::Error::from_format<
                "sink '{}': bind returned no sink handle">(
                dstd::Condition::Internal, handle_->name);
        return dstd::UniquePtr<dataframe::SinkWriter>(
            dstd::make_unique<VtableSinkWriter>(handle_, sink));
    }

   private:
    dstd::SharedPtr<SinkHandle> handle_;
};

}  // namespace

dstd::Result<void> register_file_format_vtable(
    dstd::StringView name, const ::dftu_file_format_vt* vt, void* self,
    dstd::SharedPtr<PluginLibrary> library,
    dstd::SharedPtr<PluginMemory> memory) {
    if (name.empty() || !vt)
        return dstd::Error::from_format<
            "file format register: the name and the vtable must be set">(
            dstd::Condition::InvalidArgument);
    if (!plugin_struct_size_ok(
            PluginStruct::FileFormatVt, vt->struct_size,
            static_cast<std::uint32_t>(sizeof(::dftu_file_format_vt))))
        return dstd::Error::from_format<
            "file format '{}': struct_size {} is outside [{}, {}]">(
            dstd::Condition::InvalidArgument, name, vt->struct_size,
            plugin_struct_min_size(PluginStruct::FileFormatVt),
            static_cast<std::uint32_t>(sizeof(::dftu_file_format_vt)));
    ::dftu_file_format_vt vt_copy{};
    common::copy_struct(&vt_copy, sizeof(vt_copy), vt, vt->struct_size,
                        plugin_struct_min_size(PluginStruct::FileFormatVt));
    if (!vt_copy.probe || !vt_copy.open || !vt_copy.schema || !vt_copy.chunks ||
        !vt_copy.scan)
        return dstd::Error::from_format<
            "file format '{}': probe, open, schema, chunks and scan must all "
            "be set">(dstd::Condition::InvalidArgument, name);
    // Not owned until registered, so a refused registration leaves `self` to
    // the caller.
    auto handle = dstd::make_shared<FormatHandle>(
        std::move(library), std::move(memory), name, vt_copy, self, false);
    auto registered = dataframe::register_file_format(
        dstd::make_shared<VtableFormat>(handle));
    if (registered) handle->owned = true;
    return registered;
}

dstd::Result<void> register_sink_vtable(
    dstd::StringView name, const ::dftu_sink_vt* vt, void* self,
    dstd::SharedPtr<PluginLibrary> library) {
    if (name.empty() || !vt)
        return dstd::Error::from_format<
            "sink register: the name and the vtable must be set">(
            dstd::Condition::InvalidArgument);
    if (!plugin_struct_size_ok(
            PluginStruct::SinkVt, vt->struct_size,
            static_cast<std::uint32_t>(sizeof(::dftu_sink_vt))))
        return dstd::Error::from_format<
            "sink '{}': struct_size {} is outside [{}, {}]">(
            dstd::Condition::InvalidArgument, name, vt->struct_size,
            plugin_struct_min_size(PluginStruct::SinkVt),
            static_cast<std::uint32_t>(sizeof(::dftu_sink_vt)));
    ::dftu_sink_vt vt_copy{};
    common::copy_struct(&vt_copy, sizeof(vt_copy), vt, vt->struct_size,
                        plugin_struct_min_size(PluginStruct::SinkVt));
    if (!vt_copy.bind || !vt_copy.write || !vt_copy.finalize) {
        const char* missing = !vt_copy.bind    ? "bind"
                              : !vt_copy.write ? "write"
                                               : "finalize";
        return dstd::Error::from_format<"sink '{}': {} must be set">(
            dstd::Condition::InvalidArgument, name, missing);
    }
    auto handle = dstd::make_shared<SinkHandle>(std::move(library), name,
                                                vt_copy, self, false);
    auto registered =
        dataframe::register_sink(dstd::make_shared<VtableSink>(handle));
    if (registered) handle->owned = true;
    return registered;
}

}  // namespace dftracer::utils::plugins::host
