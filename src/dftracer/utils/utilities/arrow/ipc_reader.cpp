// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/arrow/ipc_reader.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>
#include <sys/mman.h>

#include <cstring>
#include <new>

namespace dftracer::utils::utilities::arrow {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static ArrowIpcDecoder* as_decoder(void* p) noexcept {
    return static_cast<ArrowIpcDecoder*>(p);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

IpcReader::~IpcReader() { close(); }

IpcReader::IpcReader(IpcReader&& other) noexcept
    : mapped_data_(other.mapped_data_),
      mapped_size_(other.mapped_size_),
      file_(std::move(other.file_)),
      decoder_(other.decoder_),
      shared_schema_(std::move(other.shared_schema_)),
      blocks_(std::move(other.blocks_)),
      batch_count_(other.batch_count_),
      total_rows_(other.total_rows_) {
    other.reset_state();
}

IpcReader& IpcReader::operator=(IpcReader&& other) noexcept {
    if (this != &other) {
        close();
        mapped_data_ = other.mapped_data_;
        mapped_size_ = other.mapped_size_;
        file_ = std::move(other.file_);
        decoder_ = other.decoder_;
        shared_schema_ = std::move(other.shared_schema_);
        blocks_ = std::move(other.blocks_);
        batch_count_ = other.batch_count_;
        total_rows_ = other.total_rows_;
        other.reset_state();
    }
    return *this;
}

void IpcReader::reset_state() noexcept {
    mapped_data_ = nullptr;
    mapped_size_ = 0;
    decoder_ = nullptr;
    shared_schema_.reset();
    blocks_.clear();
    batch_count_ = 0;
    total_rows_ = 0;
}

void IpcReader::close() {
    if (decoder_) {
        ArrowIpcDecoderReset(as_decoder(decoder_));
        dstd::UniquePtr<ArrowIpcDecoder>(as_decoder(decoder_)).reset();
        decoder_ = nullptr;
    }

    shared_schema_.reset();

    if (mapped_data_ && mapped_data_ != MAP_FAILED) {
        munmap(mapped_data_, mapped_size_);
    }
    file_.close().discard();

    reset_state();
}

// ---------------------------------------------------------------------------
// open / read_footer
// ---------------------------------------------------------------------------

int IpcReader::open(dstd::CStringView path) {
    if (is_open()) return -1;

    auto opened = dstd::File::open(dstd::PathView(path.view()), {});
    if (!opened) return -1;
    const auto size = opened->size();
    if (!size) return -1;
    mapped_size_ = static_cast<std::size_t>(*size);

    // Minimum Arrow IPC file: magic(8) + footer_size(4) + magic(6) = 18 bytes
    if (mapped_size_ < 18) {
        mapped_size_ = 0;
        return -1;
    }

    // The mapping stays valid after the file closes, but the file is held for
    // as long as the reader is open.
    mapped_data_ = mmap(nullptr, mapped_size_, PROT_READ, MAP_PRIVATE,
                        opened->native_handle(), 0);
    if (mapped_data_ == MAP_FAILED) {
        mapped_data_ = nullptr;
        mapped_size_ = 0;
        return -1;
    }
    file_ = std::move(*opened);

    // Advise kernel we'll read sequentially
    madvise(mapped_data_, mapped_size_, MADV_SEQUENTIAL);

    int rc = read_footer();
    if (rc != NANOARROW_OK) {
        close();
        return rc;
    }

    return NANOARROW_OK;
}

int IpcReader::read_footer() {
    auto* data = static_cast<const uint8_t*>(mapped_data_);

    // Arrow IPC file format footer:
    // ... | footer | footer_size (4 bytes) | "ARROW1" (6 bytes)

    // Validate magic at end
    if (std::memcmp(data + mapped_size_ - 6, "ARROW1", 6) != 0) {
        return -1;
    }

    // Read footer size (4 bytes before magic)
    std::int32_t footer_size;
    std::memcpy(&footer_size, data + mapped_size_ - 10, sizeof(footer_size));

    // Calculate footer bounds
    std::int64_t footer_total_size =
        footer_size + 10;     // footer + size(4) + magic(6)
    std::int64_t footer_offset = mapped_size_ - footer_total_size;
    if (footer_offset < 8) {  // Must be after file magic
        return -1;
    }

    // Initialize decoder
    dstd::UniquePtr<ArrowIpcDecoder> decoder_owner;
    try {
        decoder_owner = dstd::make_unique<ArrowIpcDecoder>();
    } catch (const std::bad_alloc&) {
        return -1;
    }
    auto* decoder = decoder_owner.get();
    std::memset(decoder, 0, sizeof(ArrowIpcDecoder));

    int rc = ArrowIpcDecoderInit(decoder);
    if (rc != NANOARROW_OK) return rc;
    decoder_ = decoder_owner.release();

    // Decode footer directly from mmap'd memory (zero-copy)
    ArrowBufferView footer_view;
    footer_view.data.as_uint8 = data + footer_offset;
    footer_view.size_bytes = footer_total_size;

    ArrowError error;
    rc = ArrowIpcDecoderVerifyFooter(decoder, footer_view, &error);
    if (rc != NANOARROW_OK) {
        return rc;
    }

    rc = ArrowIpcDecoderDecodeFooter(decoder, footer_view, &error);
    if (rc != NANOARROW_OK) {
        return rc;
    }

    // Footer is now available at decoder->footer
    ArrowIpcFooter* footer = decoder->footer;

    // Copy block info - decoder state may be modified by subsequent operations
    batch_count_ =
        footer->record_batch_blocks.size_bytes / sizeof(ArrowIpcFileBlock);
    blocks_.resize(batch_count_);
    auto* src_blocks = reinterpret_cast<const ArrowIpcFileBlock*>(
        footer->record_batch_blocks.data);
    for (std::size_t i = 0; i < batch_count_; ++i) {
        blocks_[i].offset = src_blocks[i].offset;
        blocks_[i].metadata_length = src_blocks[i].metadata_length;
        blocks_[i].body_length = src_blocks[i].body_length;
    }

    // Create shared schema - deep copy once, share for all batches
    dstd::UniquePtr<ArrowSchema> schema_owner;
    try {
        schema_owner = dstd::make_unique<ArrowSchema>();
    } catch (const std::bad_alloc&) {
        return -1;
    }
    auto* schema = schema_owner.get();
    std::memset(schema, 0, sizeof(ArrowSchema));
    rc = ArrowSchemaDeepCopy(&footer->schema, schema);
    if (rc != NANOARROW_OK) return rc;
    schema_owner.release();

    // Wrap in shared_ptr with custom deleter
    shared_schema_ =
        dstd::adopt_shared_ptr(static_cast<void*>(schema), [](void* p) {
            auto* s = static_cast<ArrowSchema*>(p);
            if (s->release) s->release(s);
            dstd::UniquePtr<ArrowSchema>(s).reset();
        });

    // Set decoder's expected schema
    rc = ArrowIpcDecoderSetSchema(decoder, &footer->schema, &error);
    if (rc != NANOARROW_OK) {
        return rc;
    }

    return NANOARROW_OK;
}

// ---------------------------------------------------------------------------
// read_batch
// ---------------------------------------------------------------------------

ArrowExportResult IpcReader::read_batch(std::size_t index) {
    if (!is_open() || index >= batch_count_) {
        return ArrowExportResult();
    }

    auto* decoder = as_decoder(decoder_);
    auto* data = static_cast<const uint8_t*>(mapped_data_);
    const auto& block = blocks_[index];

    // Zero-copy: point directly into mmap'd memory for header
    ArrowBufferView header_view;
    header_view.data.as_uint8 = data + block.offset;
    header_view.size_bytes = block.metadata_length;

    ArrowError error;
    int rc = ArrowIpcDecoderDecodeHeader(decoder, header_view, &error);
    if (rc != NANOARROW_OK) {
        return ArrowExportResult();
    }

    // Zero-copy: point directly into mmap'd memory for body
    ArrowBufferView body_view;
    body_view.data.as_uint8 = data + block.offset + block.metadata_length;
    body_view.size_bytes = block.body_length;

    // Decode array
    nanoarrow::UniqueArray array;
    rc = ArrowIpcDecoderDecodeArray(decoder, body_view, -1, array.get(),
                                    NANOARROW_VALIDATION_LEVEL_FULL, &error);
    if (rc != NANOARROW_OK) {
        return ArrowExportResult();
    }

    // Share schema instead of deep copying
    // We need to create a new ArrowSchema that references our shared one
    auto* schema_ptr = static_cast<ArrowSchema*>(shared_schema_.get());
    nanoarrow::UniqueSchema schema;
    rc = ArrowSchemaDeepCopy(schema_ptr, schema.get());
    if (rc != NANOARROW_OK) {
        return ArrowExportResult();
    }

    return ArrowExportResult(std::move(schema), std::move(array));
}

// ---------------------------------------------------------------------------
// read_all / for_each_batch
// ---------------------------------------------------------------------------

dstd::Vector<ArrowExportResult> IpcReader::read_all() {
    dstd::Vector<ArrowExportResult> results;
    results.reserve(batch_count_);

    for (std::size_t i = 0; i < batch_count_; ++i) {
        auto batch = read_batch(i);
        if (batch.valid()) {
            results.push_back(std::move(batch));
        }
    }

    return results;
}

int IpcReader::for_each_batch(
    dstd::Function<int(ArrowExportResult&)> callback) {
    for (std::size_t i = 0; i < batch_count_; ++i) {
        auto batch = read_batch(i);
        if (!batch.valid()) {
            return -1;
        }
        int rc = callback(batch);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

}  // namespace dftracer::utils::utilities::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
