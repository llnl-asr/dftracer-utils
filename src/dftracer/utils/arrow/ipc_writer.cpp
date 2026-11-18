// SPDX-License-Identifier: MIT
#include <dftracer/utils/arrow/array_view.h>
#include <dftracer/utils/arrow/internal.h>
#include <dftracer/utils/arrow/ipc.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <flatcc/flatcc_builder.h>
#include <nanoarrow/ipc/flatcc_generated.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>
#include <zstd.h>

#include <atomic>
#include <cstring>
#include <utility>

#define ns(x) FLATBUFFERS_WRAP_NAMESPACE(org_apache_arrow_flatbuf, x)

namespace dftracer::utils::arrow {

namespace {

using dataframe::DataFrame;
using dataframe::Series;

/// nanoarrow's IPC encoder has no schema form for the view and dictionary
/// encodings, so a frame with one is written as its plain values.
DataFrame as_written(const DataFrame& frame) {
    DataFrame out;
    out.names = frame.names;
    if (frame.row_index)
        out.row_index = dstd::make_unique<dstd::Vector<dstd::SharedString>>(
            *frame.row_index);
    out.columns.reserve(frame.columns.size());
    for (const Series& col : frame.columns)
        out.columns.push_back(col.is_flat() ? col.share() : col.materialize());
    return out;
}

/// The 8-byte magic an Arrow IPC file starts with: "ARROW1", then two zero
/// bytes of padding.
constexpr std::uint8_t FILE_HEADER_MAGIC[8] = {'A', 'R', 'R', 'O',
                                               'W', '1', 0,   0};
/// The 6-byte magic a file ends with, right after the footer length.
constexpr std::uint8_t FILE_FOOTER_MAGIC[6] = {'A', 'R', 'R', 'O', 'W', '1'};

struct BufferInfo {
    std::int64_t offset;
    std::int64_t length;
};

/// Set by a test, so one writer call fails the way a real export can.
std::atomic<bool> g_fail_next_ipc_write{false};

void collect_flat_buffers(ArrowArrayView* view,
                          dstd::Vector<ArrowBufferView>& out, bool is_root) {
    if (!is_root) {
        const std::int64_t count = ArrowArrayViewGetNumBuffers(view);
        for (std::int64_t i = 0; i < count; ++i)
            out.push_back(ArrowArrayViewGetBufferView(view, i));
    }
    for (std::int64_t i = 0; i < view->n_children; ++i)
        collect_flat_buffers(view->children[i], out, false);
}

void collect_nodes(const ArrowArrayView* view,
                   dstd::Vector<ns(FieldNode_t)>& nodes, bool is_root) {
    if (!is_root) {
        ns(FieldNode_t) node;
        node.length = view->length;
        node.null_count = ArrowArrayViewComputeNullCount(view);
        nodes.push_back(node);
    }
    for (std::int64_t i = 0; i < view->n_children; ++i)
        collect_nodes(view->children[i], nodes, false);
}

/// The compressed body: each non-empty buffer as an int64 uncompressed size
/// followed by zstd data, padded to 8 bytes.
bool build_compressed_body(ArrowArrayView* view,
                           dstd::Vector<std::uint8_t>& body,
                           dstd::Vector<BufferInfo>& info) {
    dstd::Vector<ArrowBufferView> flat;
    collect_flat_buffers(view, flat, true);
    body.clear();
    info.clear();
    std::int64_t offset = 0;
    for (const auto& buf : flat) {
        const std::int64_t size = buf.size_bytes;
        if (size == 0 || buf.data.data == nullptr) {
            info.push_back({offset, 0});
            continue;
        }
        const std::size_t bound =
            ZSTD_compressBound(static_cast<std::size_t>(size));
        const std::size_t at = body.size();
        body.resize(at + 8 + bound);
        const std::size_t written =
            ZSTD_compress(body.data() + at + 8, bound, buf.data.data,
                          static_cast<std::size_t>(size), 3);
        if (ZSTD_isError(written)) return false;
        const std::int64_t total = 8 + static_cast<std::int64_t>(written);
        body.resize(at + static_cast<std::size_t>(total));
        std::memcpy(body.data() + at, &size, 8);
        info.push_back({offset, total});
        offset += total;
        const std::int64_t padded = (offset + 7) & ~std::int64_t{7};
        body.resize(static_cast<std::size_t>(padded), 0);
        offset = padded;
    }
    return true;
}

/// A record batch message header: the encapsulated prefix, the flatbuffer and
/// its padding, so the whole thing is a multiple of 8 bytes.
bool build_message_header(ArrowArrayView* view,
                          dstd::Span<const BufferInfo> info,
                          std::int64_t body_length,
                          dstd::Vector<std::uint8_t>& header) {
    dstd::Vector<ns(FieldNode_t)> nodes;
    collect_nodes(view, nodes, true);

    flatcc_builder_t builder;
    if (flatcc_builder_init(&builder) != 0) return false;
    flatcc_builder_set_vtable_clustering(&builder, 0);

    dstd::Vector<ns(Buffer_t)> buffers;
    buffers.reserve(info.size());
    for (const auto& b : info) {
        ns(Buffer_t) entry;
        entry.offset = b.offset;
        entry.length = b.length;
        buffers.push_back(entry);
    }

    ns(BodyCompression_ref_t) compression = ns(BodyCompression_create(
        &builder, ns(CompressionType_ZSTD), ns(BodyCompressionMethod_BUFFER)));

    ns(Message_start_as_root(&builder));
    ns(Message_version_add(&builder, ns(MetadataVersion_V5)));
    ns(Message_header_RecordBatch_start(&builder));
    ns(RecordBatch_length_add(&builder, view->length));
    ns(RecordBatch_nodes_create(
        &builder, reinterpret_cast<ns(FieldNode_t)*>(nodes.data()),
        nodes.size()));
    ns(RecordBatch_buffers_create(&builder, buffers.data(), buffers.size()));
    ns(RecordBatch_compression_add(&builder, compression));
    ns(Message_header_RecordBatch_end(&builder));
    ns(Message_bodyLength_add(&builder, body_length));
    ns(Message_end_as_root(&builder));

    std::size_t message_size = 0;
    void* message = flatcc_builder_get_direct_buffer(&builder, &message_size);
    void* allocated = nullptr;
    if (message == nullptr) {
        message = flatcc_builder_finalize_buffer(&builder, &message_size);
        allocated = message;
    }
    if (message == nullptr || message_size == 0) {
        if (allocated != nullptr) flatcc_builder_free(allocated);
        flatcc_builder_clear(&builder);
        return false;
    }

    const std::int32_t continuation = -1;
    const auto size32 = static_cast<std::int32_t>(message_size);
    const std::size_t padding = (8 - (message_size % 8)) % 8;
    header.clear();
    header.resize(8 + message_size + padding);
    std::memcpy(header.data(), &continuation, 4);
    std::memcpy(header.data() + 4, &size32, 4);
    std::memcpy(header.data() + 8, message, message_size);

    if (allocated != nullptr) flatcc_builder_free(allocated);
    flatcc_builder_clear(&builder);
    return true;
}

/// An uncompressed record batch, as nanoarrow encodes it.
bool encode_batch_uncompressed(const ArrowSchema* schema,
                               const ArrowArray* array,
                               dstd::Vector<std::uint8_t>& header,
                               dstd::Vector<std::uint8_t>& body) {
    ArrowArrayView view;
    ArrowError error;
    if (ArrowArrayViewInitFromSchema(&view, schema, &error) != NANOARROW_OK) {
        DFTRACER_UTILS_LOG_ERROR("arrow ipc write: schema view: {}",
                                 error.message);
        return false;
    }
    if (ArrowArrayViewSetArray(&view, array, &error) != NANOARROW_OK) {
        DFTRACER_UTILS_LOG_ERROR("arrow ipc write: array view: {}",
                                 error.message);
        ArrowArrayViewReset(&view);
        return false;
    }

    ArrowIpcEncoder encoder;
    ArrowIpcEncoderInit(&encoder);
    ArrowBuffer body_buf;
    ArrowBufferInit(&body_buf);
    int rc = ArrowIpcEncoderEncodeSimpleRecordBatch(&encoder, &view, &body_buf,
                                                    &error);
    ArrowBuffer header_buf;
    ArrowBufferInit(&header_buf);
    if (rc == NANOARROW_OK)
        rc = ArrowIpcEncoderFinalizeBuffer(&encoder, 1, &header_buf);
    if (rc == NANOARROW_OK) {
        header.assign(header_buf.data, header_buf.data + header_buf.size_bytes);
        body.assign(body_buf.data, body_buf.data + body_buf.size_bytes);
    }
    ArrowBufferReset(&body_buf);
    ArrowBufferReset(&header_buf);
    ArrowIpcEncoderReset(&encoder);
    ArrowArrayViewReset(&view);
    return rc == NANOARROW_OK;
}

/// One record batch: its header message and its body.
bool encode_batch(const ArrowSchema* schema, const ArrowArray* array,
                  Compression compression, dstd::Vector<std::uint8_t>& header,
                  dstd::Vector<std::uint8_t>& body) {
    if (compression == Compression::None)
        return encode_batch_uncompressed(schema, array, header, body);

    ArrowArrayView view;
    if (arrow::init_array_view(view, const_cast<ArrowSchema*>(schema),
                               const_cast<ArrowArray*>(array)) != NANOARROW_OK)
        return false;
    dstd::Vector<BufferInfo> info;
    const bool ok =
        build_compressed_body(&view, body, info) &&
        build_message_header(&view, info,
                             static_cast<std::int64_t>(body.size()), header);
    ArrowArrayViewReset(&view);
    return ok;
}

}  // namespace

namespace detail {

void fail_next_ipc_write_for_test() {
    g_fail_next_ipc_write.store(true, std::memory_order_relaxed);
}

}  // namespace detail

struct Writer::Impl {
    dstd::File file;
    dstd::Vector<std::uint8_t> bytes;
    std::int64_t offset = 0;
    bool is_file = false;
    Compression compression = Compression::None;
    bool schema_written = false;
    bool finished = false;
    ArrowSchema footer_schema{};
    dstd::Vector<ArrowIpcFileBlock> blocks;
    dstd::Vector<dstd::String> names;
    /// The first frame's column types, which every later frame must match.
    dstd::Vector<dataframe::DataType> types;
    bool failed = false;
    common::ErrorCode failure_code = common::ErrorCode::Internal;
    dstd::String failure;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        if (footer_schema.release != nullptr)
            footer_schema.release(&footer_schema);
    }

    dstd::Result<void> fail(common::ErrorCode code, dstd::StringView what) {
        DFTRACER_UTILS_LOG_ERROR("arrow ipc write: {}", what);
        failed = true;
        failure_code = code;
        failure = dstd::String(what);
        return common::make_error(code, what);
    }

    dstd::Result<void> not_failed() const {
        if (!failed) return {};
        return common::make_error<"{}, and the writer failed earlier: {}">(
            failure_code, "arrow ipc write", failure);
    }

    /// Appends `data` to the sink, then `data2` when it is not empty.
    dstd::Result<void> emit(dstd::ByteView data) {
        if (!is_file) {
            bytes.insert(bytes.end(), data.as<std::uint8_t>(),
                         data.as<std::uint8_t>() + data.size());
            offset = static_cast<std::int64_t>(bytes.size());
            return {};
        }
        auto written =
            file.write_all_at(static_cast<std::uint64_t>(offset), data);
        if (!written)
            return fail(common::ErrorCode::Io, written.error().message());
        offset += static_cast<std::int64_t>(data.size());
        return {};
    }

    dstd::Result<void> write_schema(const ArrowSchema* schema) {
        if (ArrowSchemaDeepCopy(schema, &footer_schema) != NANOARROW_OK)
            return fail(common::ErrorCode::Internal,
                        "arrow ipc write: schema copy failed");
        ArrowIpcEncoder encoder;
        ArrowIpcEncoderInit(&encoder);
        ArrowError error;
        int rc = ArrowIpcEncoderEncodeSchema(&encoder, schema, &error);
        ArrowBuffer message;
        ArrowBufferInit(&message);
        if (rc == NANOARROW_OK)
            rc = ArrowIpcEncoderFinalizeBuffer(&encoder, 1, &message);
        ArrowIpcEncoderReset(&encoder);
        if (rc != NANOARROW_OK) {
            ArrowBufferReset(&message);
            return fail(common::ErrorCode::Internal, error.message);
        }
        auto out = emit(dstd::ByteView(message.data, message.size_bytes));
        ArrowBufferReset(&message);
        if (!out) return out;
        schema_written = true;
        return {};
    }

    dstd::Result<void> write_footer() {
        ArrowIpcFooter footer;
        ArrowIpcFooterInit(&footer);
        ArrowSchemaMove(&footer_schema, &footer.schema);
        for (const auto& block : blocks) {
            if (ArrowBufferAppend(&footer.record_batch_blocks, &block,
                                  sizeof(block)) != NANOARROW_OK) {
                ArrowIpcFooterReset(&footer);
                return fail(common::ErrorCode::Internal,
                            "arrow ipc write: footer block append failed");
            }
        }
        ArrowIpcEncoder encoder;
        ArrowIpcEncoderInit(&encoder);
        ArrowError error;
        int rc = ArrowIpcEncoderEncodeFooter(&encoder, &footer, &error);
        ArrowBuffer buffer;
        ArrowBufferInit(&buffer);
        if (rc == NANOARROW_OK)
            rc = ArrowIpcEncoderFinalizeBuffer(&encoder, 0, &buffer);
        ArrowIpcEncoderReset(&encoder);
        ArrowIpcFooterReset(&footer);
        if (rc != NANOARROW_OK) {
            ArrowBufferReset(&buffer);
            return fail(common::ErrorCode::Internal, error.message);
        }

        // End of stream, footer, its size, and the magic.
        dstd::Vector<std::uint8_t> data(8 + buffer.size_bytes + 4 + 6, 0);
        const std::int32_t continuation = -1;
        const std::int32_t end_of_stream = 0;
        std::memcpy(data.data(), &continuation, 4);
        std::memcpy(data.data() + 4, &end_of_stream, 4);
        std::memcpy(data.data() + 8, buffer.data, buffer.size_bytes);
        const auto footer_size = static_cast<std::int32_t>(buffer.size_bytes);
        std::memcpy(data.data() + 8 + buffer.size_bytes, &footer_size, 4);
        std::memcpy(data.data() + 8 + buffer.size_bytes + 4, FILE_FOOTER_MAGIC,
                    6);
        ArrowBufferReset(&buffer);

        auto out = emit(dstd::ByteView(data));
        if (!out) return out;
        return file.close() ? dstd::Result<void>{}
                            : fail(common::ErrorCode::Io,
                                   "arrow ipc write: close failed");
    }
};

Writer::Writer(dstd::UniquePtr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Writer::Writer(Writer&& other) noexcept = default;
Writer& Writer::operator=(Writer&& other) noexcept = default;
Writer::~Writer() = default;

dstd::Result<Writer> Writer::open(const dstd::Path& path,
                                  WriterOptions options) {
    auto impl = dstd::make_unique<Impl>();
    impl->is_file = true;
    impl->compression = options.compression;
    const dstd::OpenOptions open_options{.access = dstd::Access::Write,
                                         .creation = dstd::Creation::AlwaysNew};
    auto opened = dstd::File::open(path.view(), open_options);
    if (!opened)
        return common::make_error<"arrow ipc open '{}': {}">(
            common::ErrorCode::Io, path.view(), opened.error().message());
    impl->file = std::move(*opened);
    auto header = impl->emit(dstd::ByteView(FILE_HEADER_MAGIC, 8));
    if (!header) return header.error();
    return Writer{std::move(impl)};
}

Writer Writer::to_buffer(WriterOptions options) {
    auto impl = dstd::make_unique<Impl>();
    impl->compression = options.compression;
    return Writer{std::move(impl)};
}

dstd::Result<void> Writer::write(const DataFrame& frame) {
    Impl& self = *impl_;
    DFTRACER_UTILS_TRY_VOID(self.not_failed());
    if (self.finished)
        return self.fail(common::ErrorCode::InvalidArgument,
                         "arrow ipc write: the writer is finished");
    const DataFrame written = as_written(frame);
    if (self.schema_written) {
        if (self.names.size() != written.names.size() ||
            self.types.size() != written.columns.size())
            return self.fail(common::ErrorCode::InvalidArgument,
                             "arrow ipc write: a frame with other columns");
        for (std::size_t i = 0; i < self.names.size(); ++i) {
            if (self.names[i] != written.names[i].view())
                return self.fail(common::ErrorCode::InvalidArgument,
                                 "arrow ipc write: a frame with other columns");
        }
        for (std::size_t i = 0; i < self.types.size(); ++i) {
            if (self.types[i] != written.columns[i].data_type())
                return self.fail(common::ErrorCode::InvalidArgument,
                                 "arrow ipc write: a frame with other column "
                                 "types");
        }
    }

    if (g_fail_next_ipc_write.exchange(false, std::memory_order_relaxed))
        return common::make_error<"arrow ipc write: an injected failure">(
            common::ErrorCode::Internal);
    auto batch = to_arrow(written);
    if (!batch) return batch.error();
    ArrowSchema schema;
    ArrowArray array;
    batch->release_into(&schema, &array);

    dstd::Result<void> out{};
    if (!self.schema_written) {
        out = self.write_schema(&schema);
        // Set only once the schema is out, so a failed call leaves them unset.
        if (out) {
            self.names = dstd::to_strings(written.names);
            self.types.reserve(written.columns.size());
            for (const Series& col : written.columns)
                self.types.push_back(col.data_type());
        }
    }
    if (out) {
        dstd::Vector<std::uint8_t> header;
        dstd::Vector<std::uint8_t> body;
        if (!encode_batch(&schema, &array, self.compression, header, body)) {
            out = self.fail(common::ErrorCode::Internal,
                            "arrow ipc write: record batch encode failed");
        } else {
            ArrowIpcFileBlock block{};
            block.offset = self.offset;
            block.metadata_length = static_cast<std::int32_t>(header.size());
            block.body_length = static_cast<std::int64_t>(body.size());
            out = self.emit(dstd::ByteView(header));
            if (out) out = self.emit(dstd::ByteView(body));
            if (out) self.blocks.push_back(block);
        }
    }
    if (schema.release != nullptr) schema.release(&schema);
    if (array.release != nullptr) array.release(&array);
    return out;
}

dstd::Result<void> Writer::finish() {
    Impl& self = *impl_;
    DFTRACER_UTILS_TRY_VOID(self.not_failed());
    if (self.finished) return {};
    self.finished = true;
    if (self.is_file) {
        if (!self.schema_written)
            return self.fail(common::ErrorCode::InvalidArgument,
                             "arrow ipc finish: no frame was written");
        return self.write_footer();
    }
    const std::int32_t continuation = -1;
    const std::int32_t end_of_stream = 0;
    std::uint8_t eos[8];
    std::memcpy(eos, &continuation, 4);
    std::memcpy(eos + 4, &end_of_stream, 4);
    return self.emit(dstd::ByteView(eos, 8));
}

dstd::Vector<std::uint8_t> Writer::take_bytes() {
    if (impl_ == nullptr) return {};
    return std::move(impl_->bytes);
}

}  // namespace dftracer::utils::arrow
