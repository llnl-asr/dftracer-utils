#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/io/ops.h>
#include <dftracer/utils/utilities/common/arrow/array_view.h>
#include <dftracer/utils/utilities/common/arrow/ipc_writer.h>
#include <fcntl.h>
#include <flatcc/flatcc_builder.h>
#include <nanoarrow/ipc/flatcc_generated.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef DFTRACER_UTILS_ENABLE_ZSTD
#include <zstd.h>
#endif

#include <cstring>
#include <vector>

#define ns(x) FLATBUFFERS_WRAP_NAMESPACE(org_apache_arrow_flatbuf, x)

namespace dftracer::utils::utilities::common::arrow {

using FileBlock = ArrowIpcFileBlock;

// ---------------------------------------------------------------------------
// BufferPool
// ---------------------------------------------------------------------------

BufferPool::BufferPool(std::size_t num_slots, std::size_t initial_capacity) {
    slots_.reserve(num_slots);
    for (std::size_t i = 0; i < num_slots; ++i) {
        auto slot = std::make_unique<Slot>();
        slot->data.reserve(initial_capacity);
        slots_.push_back(std::move(slot));
    }
}

BufferPool::Slot* BufferPool::acquire(std::size_t min_capacity) {
    for (auto& slot : slots_) {
        bool expected = false;
        if (slot->in_use.compare_exchange_strong(expected, true,
                                                 std::memory_order_acquire)) {
            if (slot->data.capacity() < min_capacity) {
                slot->data.reserve(min_capacity);
            }
            slot->data.clear();
            return slot.get();
        }
    }
    return nullptr;  // All slots in use
}

void BufferPool::release(Slot* slot) {
    if (slot) {
        slot->in_use.store(false, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Compression helpers
// ---------------------------------------------------------------------------

#ifdef DFTRACER_UTILS_ENABLE_ZSTD

struct BufferInfo {
    std::int64_t offset;
    std::int64_t length;
};

static void collect_flat_buffers(ArrowArrayView* view,
                                 std::vector<ArrowBufferView>& out,
                                 bool is_root = false) {
    if (!is_root) {
        std::int64_t num_buffers = ArrowArrayViewGetNumBuffers(view);
        for (std::int64_t i = 0; i < num_buffers; i++) {
            out.push_back(ArrowArrayViewGetBufferView(view, i));
        }
    }
    for (std::int64_t i = 0; i < view->n_children; i++) {
        collect_flat_buffers(view->children[i], out, false);
    }
}

static void collect_nodes(const ArrowArrayView* view,
                          std::vector<ns(FieldNode_t)>& nodes,
                          bool is_root = false) {
    if (!is_root) {
        ns(FieldNode_t) node;
        node.length = view->length;
        node.null_count = ArrowArrayViewComputeNullCount(view);
        nodes.push_back(node);
    }
    for (std::int64_t i = 0; i < view->n_children; i++) {
        collect_nodes(view->children[i], nodes, false);
    }
}

static int build_compressed_body(ArrowArrayView* view,
                                 std::vector<uint8_t>& out_body,
                                 std::vector<BufferInfo>& out_info) {
    std::vector<ArrowBufferView> flat_buffers;
    collect_flat_buffers(view, flat_buffers, true);

    out_body.clear();
    out_info.clear();
    std::int64_t compressed_offset = 0;

    for (const auto& buf : flat_buffers) {
        std::int64_t uncompressed_size = buf.size_bytes;

        if (uncompressed_size == 0 || buf.data.data == nullptr) {
            out_info.push_back({compressed_offset, 0});
            continue;
        }

        std::size_t max_compressed = ZSTD_compressBound(uncompressed_size);
        std::size_t old_size = out_body.size();

        // Reserve space: [int64 uncompressed_size][zstd data]
        out_body.resize(old_size + 8 + max_compressed);

        std::size_t compressed_size =
            ZSTD_compress(out_body.data() + old_size + 8, max_compressed,
                          buf.data.data, uncompressed_size, 3);

        if (ZSTD_isError(compressed_size)) {
            return -1;
        }

        std::int64_t total_size =
            8 + static_cast<std::int64_t>(compressed_size);
        out_body.resize(old_size + total_size);
        std::memcpy(out_body.data() + old_size, &uncompressed_size, 8);

        out_info.push_back({compressed_offset, total_size});

        compressed_offset += total_size;
        std::int64_t padded = (compressed_offset + 7) & ~7;
        out_body.resize(padded, 0);
        compressed_offset = padded;
    }

    return 0;
}

static int build_message_header(ArrowArrayView* view,
                                const std::vector<BufferInfo>& buffer_info,
                                std::int64_t body_length,
                                std::vector<uint8_t>& out_header) {
    std::vector<ns(FieldNode_t)> nodes;
    collect_nodes(view, nodes, true);

    flatcc_builder_t builder;
    if (flatcc_builder_init(&builder) == -1) {
        return -1;
    }
    flatcc_builder_set_vtable_clustering(&builder, 0);

    std::vector<ns(Buffer_t)> buffer_structs;
    buffer_structs.reserve(buffer_info.size());
    for (const auto& buf : buffer_info) {
        ns(Buffer_t) b;
        b.offset = buf.offset;
        b.length = buf.length;
        buffer_structs.push_back(b);
    }

    ns(BodyCompression_ref_t) compression_ref = ns(BodyCompression_create(
        &builder, ns(CompressionType_ZSTD), ns(BodyCompressionMethod_BUFFER)));

    ns(Message_start_as_root(&builder));
    ns(Message_version_add(&builder, ns(MetadataVersion_V5)));
    ns(Message_header_RecordBatch_start(&builder));
    ns(RecordBatch_length_add(&builder, view->length));
    ns(RecordBatch_nodes_create(
        &builder, reinterpret_cast<ns(FieldNode_t)*>(nodes.data()),
        nodes.size()));
    ns(RecordBatch_buffers_create(&builder, buffer_structs.data(),
                                  buffer_structs.size()));
    ns(RecordBatch_compression_add(&builder, compression_ref));
    ns(Message_header_RecordBatch_end(&builder));
    ns(Message_bodyLength_add(&builder, body_length));
    ns(Message_end_as_root(&builder));

    std::size_t msg_size = 0;
    void* msg_buf = flatcc_builder_get_direct_buffer(&builder, &msg_size);
    void* allocated_buf = nullptr;

    if (!msg_buf) {
        msg_buf = flatcc_builder_finalize_buffer(&builder, &msg_size);
        allocated_buf = msg_buf;
    }

    if (!msg_buf || msg_size == 0) {
        if (allocated_buf) flatcc_builder_free(allocated_buf);
        flatcc_builder_clear(&builder);
        return -1;
    }

    // Build IPC encapsulated message: continuation(-1) + size + metadata +
    // padding
    std::int32_t continuation = -1;
    std::int32_t msg_size_i32 = static_cast<std::int32_t>(msg_size);
    std::size_t msg_padding = (8 - (msg_size % 8)) % 8;

    out_header.clear();
    out_header.resize(8 + msg_size + msg_padding);
    std::memcpy(out_header.data(), &continuation, 4);
    std::memcpy(out_header.data() + 4, &msg_size_i32, 4);
    std::memcpy(out_header.data() + 8, msg_buf, msg_size);
    // Padding bytes are already zero from resize

    if (allocated_buf) flatcc_builder_free(allocated_buf);
    flatcc_builder_clear(&builder);

    return 0;
}

#endif  // DFTRACER_UTILS_ENABLE_ZSTD

// ---------------------------------------------------------------------------
// IpcWriter lifecycle
// ---------------------------------------------------------------------------

IpcWriter::~IpcWriter() {
    if (is_open()) {
        // Sync close in destructor - not ideal but safe
        if (fd_ >= 0) {
            ::close(fd_);
        }
        reset_state();
    }
}

IpcWriter::IpcWriter(IpcWriter&& other) noexcept {
    fd_ = other.fd_;
    write_offset_ = other.write_offset_;
    buffer_pool_ = std::move(other.buffer_pool_);
    schema_written_ = other.schema_written_;
    compression_ = other.compression_;
    batch_blocks_ = other.batch_blocks_;
    schema_copy_ = other.schema_copy_;
    other.reset_state();
}

IpcWriter& IpcWriter::operator=(IpcWriter&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.fd_;
        write_offset_ = other.write_offset_;
        buffer_pool_ = std::move(other.buffer_pool_);
        schema_written_ = other.schema_written_;
        compression_ = other.compression_;
        batch_blocks_ = other.batch_blocks_;
        schema_copy_ = other.schema_copy_;
        other.reset_state();
    }
    return *this;
}

void IpcWriter::reset_state() noexcept {
    fd_ = -1;
    write_offset_ = 0;
    schema_written_ = false;
    batch_blocks_ = nullptr;
    schema_copy_ = nullptr;
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

coro::CoroTask<int> IpcWriter::open(const std::string& path,
                                    IpcCompression compression,
                                    std::size_t pool_slots) {
    if (is_open()) co_return -1;

    compression_ = compression;
    buffer_pool_ = BufferPool(pool_slots);

    // Async open via io::open (auto-detects executor context)
    auto result =
        co_await io::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (result < 0) {
        reset_state();
        co_return static_cast<int>(result);
    }

    fd_ = static_cast<int>(result);
    write_offset_ = 0;

    // Write Arrow IPC file magic "ARROW1" + padding
    constexpr char MAGIC[] = "ARROW1\0\0";
    auto write_result = co_await io::pwrite(fd_, MAGIC, 8, 0);
    if (write_result < 0) {
        co_await io::close(fd_);
        reset_state();
        co_return static_cast<int>(write_result);
    }
    write_offset_ = 8;

    // Initialize block tracking
    batch_blocks_ = new std::vector<FileBlock>();

    co_return 0;
}

// ---------------------------------------------------------------------------
// write_schema
// ---------------------------------------------------------------------------

coro::CoroTask<int> IpcWriter::write_schema(ArrowExportResult& batch) {
    ArrowSchema* schema = batch.get_schema();

    // Deep copy schema for footer
    auto* schema_cp = new ArrowSchema;
    ArrowSchemaDeepCopy(schema, schema_cp);
    schema_copy_ = schema_cp;

    // Encode schema message
    ArrowIpcEncoder encoder;
    ArrowIpcEncoderInit(&encoder);

    ArrowError error;
    int rc = ArrowIpcEncoderEncodeSchema(&encoder, schema, &error);
    if (rc != NANOARROW_OK) {
        ArrowIpcEncoderReset(&encoder);
        co_return rc;
    }

    ArrowBuffer msg_buf;
    ArrowBufferInit(&msg_buf);
    rc = ArrowIpcEncoderFinalizeBuffer(&encoder, 1, &msg_buf);
    ArrowIpcEncoderReset(&encoder);
    if (rc != NANOARROW_OK) {
        ArrowBufferReset(&msg_buf);
        co_return rc;
    }

    // Write schema message
    auto result = co_await io::pwrite(fd_, msg_buf.data, msg_buf.size_bytes,
                                      write_offset_);

    if (result < 0) {
        ArrowBufferReset(&msg_buf);
        co_return static_cast<int>(result);
    }

    write_offset_ += msg_buf.size_bytes;
    ArrowBufferReset(&msg_buf);
    schema_written_ = true;

    co_return 0;
}

// ---------------------------------------------------------------------------
// encode_batch_uncompressed
// ---------------------------------------------------------------------------

static int encode_batch_uncompressed(ArrowExportResult& batch,
                                     std::vector<uint8_t>& out_header,
                                     std::vector<uint8_t>& out_body) {
    ArrowSchema* schema = batch.get_schema();
    ArrowArray* array = batch.get_array();

    ArrowArrayView view;
    ArrowError error;
    int rc = ArrowArrayViewInitFromSchema(&view, schema, &error);
    if (rc != NANOARROW_OK) {
        return rc;
    }

    rc = ArrowArrayViewSetArray(&view, array, &error);
    if (rc != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        return rc;
    }

    ArrowIpcEncoder encoder;
    ArrowIpcEncoderInit(&encoder);

    ArrowBuffer body_buf;
    ArrowBufferInit(&body_buf);

    rc = ArrowIpcEncoderEncodeSimpleRecordBatch(&encoder, &view, &body_buf,
                                                &error);
    if (rc != NANOARROW_OK) {
        ArrowBufferReset(&body_buf);
        ArrowIpcEncoderReset(&encoder);
        ArrowArrayViewReset(&view);
        return rc;
    }

    ArrowBuffer header_buf;
    ArrowBufferInit(&header_buf);

    rc = ArrowIpcEncoderFinalizeBuffer(&encoder, 1, &header_buf);
    if (rc != NANOARROW_OK) {
        ArrowBufferReset(&body_buf);
        ArrowBufferReset(&header_buf);
        ArrowIpcEncoderReset(&encoder);
        ArrowArrayViewReset(&view);
        return rc;
    }

    out_header.resize(header_buf.size_bytes);
    std::memcpy(out_header.data(), header_buf.data, header_buf.size_bytes);

    out_body.resize(body_buf.size_bytes);
    std::memcpy(out_body.data(), body_buf.data, body_buf.size_bytes);

    ArrowBufferReset(&body_buf);
    ArrowBufferReset(&header_buf);
    ArrowIpcEncoderReset(&encoder);
    ArrowArrayViewReset(&view);

    return 0;
}

// ---------------------------------------------------------------------------
// compress_batch
// ---------------------------------------------------------------------------

coro::CoroTask<IpcWriter::CompressedBatch> IpcWriter::compress_batch(
    ArrowExportResult& batch) {
    CompressedBatch result{};

    if (compression_ == IpcCompression::NONE) {
        result.body_slot = buffer_pool_.acquire(64 * 1024);
        if (!result.body_slot) {
            result.body_slot = new BufferPool::Slot();
        }

        int rc = encode_batch_uncompressed(batch, result.header,
                                           result.body_slot->data);
        if (rc != 0) {
            co_return result;
        }

        result.body_size = result.body_slot->data.size();
        result.body_length = static_cast<std::int64_t>(result.body_size);
        result.metadata_length =
            static_cast<std::int32_t>(result.header.size());
        co_return result;
    }

#ifdef DFTRACER_UTILS_ENABLE_ZSTD
    if (compression_ != IpcCompression::ZSTD) {
        co_return result;
    }

    ArrowSchema* schema = batch.get_schema();
    ArrowArray* array = batch.get_array();

    ArrowArrayView view;
    int rc = init_array_view(view, schema, array);
    if (rc != NANOARROW_OK) {
        co_return result;
    }

    // Acquire pooled buffer for compressed body
    std::size_t estimated_size = 0;
    std::vector<ArrowBufferView> flat_buffers;
    collect_flat_buffers(&view, flat_buffers, true);
    for (const auto& buf : flat_buffers) {
        estimated_size += ZSTD_compressBound(buf.size_bytes) + 8;
    }

    result.body_slot = buffer_pool_.acquire(estimated_size);
    if (!result.body_slot) {
        result.body_slot = new BufferPool::Slot();
        result.body_slot->data.reserve(estimated_size);
    }

    // Compress into pooled buffer
    std::vector<BufferInfo> buffer_info;
    rc = build_compressed_body(&view, result.body_slot->data, buffer_info);
    if (rc != 0) {
        ArrowArrayViewReset(&view);
        co_return result;
    }

    result.body_size = result.body_slot->data.size();
    result.body_length = static_cast<std::int64_t>(result.body_size);

    // Build message header
    rc = build_message_header(&view, buffer_info, result.body_length,
                              result.header);
    if (rc != 0) {
        ArrowArrayViewReset(&view);
        co_return result;
    }

    result.metadata_length = static_cast<std::int32_t>(result.header.size());

    ArrowArrayViewReset(&view);
#endif

    co_return result;
}

// ---------------------------------------------------------------------------
// write_compressed
// ---------------------------------------------------------------------------

coro::CoroTask<int> IpcWriter::write_compressed(CompressedBatch& cb) {
    auto* blocks = static_cast<std::vector<FileBlock>*>(batch_blocks_);

    // Record block info
    FileBlock block;
    block.offset = write_offset_;
    block.metadata_length = cb.metadata_length;
    block.body_length = cb.body_length;

    // Vectored write: header + body
    struct iovec iov[2];
    iov[0].iov_base = cb.header.data();
    iov[0].iov_len = cb.header.size();
    iov[1].iov_base = cb.body_slot->data.data();
    iov[1].iov_len = cb.body_size;

    auto result = co_await io::pwritev(fd_, iov, 2, write_offset_);

    // Release pooled buffer
    buffer_pool_.release(cb.body_slot);
    cb.body_slot = nullptr;

    if (result < 0) {
        co_return static_cast<int>(result);
    }

    write_offset_ += result;
    blocks->push_back(block);

    co_return 0;
}

// ---------------------------------------------------------------------------
// write_batch
// ---------------------------------------------------------------------------

coro::CoroTask<int> IpcWriter::write_batch(ArrowExportResult& batch) {
    if (!is_open() || !batch.valid()) co_return -1;

    // Write schema on first batch
    if (!schema_written_) {
        int rc = co_await write_schema(batch);
        if (rc != 0) co_return rc;
    }

    // Compress and write
    auto cb = co_await compress_batch(batch);
    if (cb.header.empty()) co_return -1;

    co_return co_await write_compressed(cb);
}

// ---------------------------------------------------------------------------
// write_batches (parallel compression)
// ---------------------------------------------------------------------------

coro::CoroTask<int> IpcWriter::write_batches(
    std::vector<ArrowExportResult>& batches) {
    if (!is_open()) co_return -1;
    if (batches.empty()) co_return 0;

    // Write schema from first batch
    if (!schema_written_) {
        int rc = co_await write_schema(batches[0]);
        if (rc != 0) co_return rc;
    }

    // Parallel compress all batches
    std::vector<coro::CoroTask<CompressedBatch>> compress_tasks;
    compress_tasks.reserve(batches.size());

    for (auto& batch : batches) {
        if (batch.valid()) {
            compress_tasks.push_back(compress_batch(batch));
        }
    }

    auto compressed = co_await coro::when_all(std::move(compress_tasks));

    // Write in order (sequential to maintain file structure)
    for (auto& cb : compressed) {
        if (cb.header.empty()) {
            co_return -1;
        }
        int rc = co_await write_compressed(cb);
        if (rc != 0) co_return rc;
    }

    co_return 0;
}

// ---------------------------------------------------------------------------
// write_footer
// ---------------------------------------------------------------------------

coro::CoroTask<int> IpcWriter::write_footer() {
    auto* blocks = static_cast<std::vector<FileBlock>*>(batch_blocks_);
    auto* schema = static_cast<ArrowSchema*>(schema_copy_);

    if (!blocks || !schema) {
        co_return -1;
    }

    ArrowIpcFooter footer;
    ArrowIpcFooterInit(&footer);
    ArrowSchemaMove(schema, &footer.schema);

    for (const auto& block : *blocks) {
        int rc = ArrowBufferAppend(&footer.record_batch_blocks, &block,
                                   sizeof(FileBlock));
        if (rc != NANOARROW_OK) {
            ArrowIpcFooterReset(&footer);
            co_return rc;
        }
    }

    ArrowIpcEncoder encoder;
    ArrowIpcEncoderInit(&encoder);

    ArrowError error;
    int rc = ArrowIpcEncoderEncodeFooter(&encoder, &footer, &error);
    if (rc != NANOARROW_OK) {
        ArrowIpcEncoderReset(&encoder);
        ArrowIpcFooterReset(&footer);
        co_return rc;
    }

    ArrowBuffer footer_buf;
    ArrowBufferInit(&footer_buf);
    rc = ArrowIpcEncoderFinalizeBuffer(&encoder, 0, &footer_buf);
    ArrowIpcEncoderReset(&encoder);
    if (rc != NANOARROW_OK) {
        ArrowBufferReset(&footer_buf);
        ArrowIpcFooterReset(&footer);
        co_return rc;
    }

    // Build footer: EOS marker + footer + footer_size + magic
    std::vector<uint8_t> footer_data;
    footer_data.resize(8 + footer_buf.size_bytes + 4 + 6);

    std::int32_t eos_continuation = -1;
    std::int32_t eos_size = 0;
    std::memcpy(footer_data.data(), &eos_continuation, 4);
    std::memcpy(footer_data.data() + 4, &eos_size, 4);
    std::memcpy(footer_data.data() + 8, footer_buf.data, footer_buf.size_bytes);

    std::int32_t footer_size = static_cast<std::int32_t>(footer_buf.size_bytes);
    std::memcpy(footer_data.data() + 8 + footer_buf.size_bytes, &footer_size,
                4);
    std::memcpy(footer_data.data() + 8 + footer_buf.size_bytes + 4, "ARROW1",
                6);

    ArrowBufferReset(&footer_buf);
    ArrowIpcFooterReset(&footer);

    // Write footer
    auto result = co_await io::pwrite(fd_, footer_data.data(),
                                      footer_data.size(), write_offset_);

    if (result < 0) {
        co_return static_cast<int>(result);
    }

    write_offset_ += result;
    co_return 0;
}

// ---------------------------------------------------------------------------
// close
// ---------------------------------------------------------------------------

coro::CoroTask<int> IpcWriter::close() {
    if (!is_open()) co_return 0;

    int rc = 0;

    // Write footer
    if (schema_written_ && batch_blocks_) {
        rc = co_await write_footer();
    }

    // Cleanup
    if (schema_copy_) {
        auto* schema = static_cast<ArrowSchema*>(schema_copy_);
        if (schema->release) schema->release(schema);
        delete schema;
    }
    if (batch_blocks_) {
        delete static_cast<std::vector<FileBlock>*>(batch_blocks_);
    }

    // Fsync and close
    co_await io::fsync(fd_);
    co_await io::close(fd_);

    reset_state();
    co_return rc;
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
