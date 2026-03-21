#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/utilities/common/arrow/ipc_writer.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <cstdio>
#include <cstring>
#include <new>

namespace dftracer::utils::utilities::common::arrow {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static ArrowIpcWriter* as_writer(void* p) noexcept {
    return static_cast<ArrowIpcWriter*>(p);
}

static ArrowIpcOutputStream* as_stream(void* p) noexcept {
    return static_cast<ArrowIpcOutputStream*>(p);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

IpcWriter::~IpcWriter() {
    if (is_open()) {
        close();
    }
}

IpcWriter::IpcWriter(IpcWriter&& other) noexcept
    : file_(other.file_),
      schema_written_(other.schema_written_),
      writer_(other.writer_),
      stream_(other.stream_) {
    other.reset_state();
}

IpcWriter& IpcWriter::operator=(IpcWriter&& other) noexcept {
    if (this != &other) {
        if (is_open()) close();
        file_ = other.file_;
        schema_written_ = other.schema_written_;
        writer_ = other.writer_;
        stream_ = other.stream_;
        other.reset_state();
    }
    return *this;
}

void IpcWriter::reset_state() noexcept {
    file_ = nullptr;
    writer_ = nullptr;
    stream_ = nullptr;
    schema_written_ = false;
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

int IpcWriter::open(const std::string& path) {
    if (is_open()) return -1;

    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return -1;

    // Allocate stream. ArrowIpcWriterInit takes ownership on success.
    auto* os = new (std::nothrow) ArrowIpcOutputStream;
    if (!os) {
        std::fclose(file_);
        file_ = nullptr;
        return -1;
    }
    std::memset(os, 0, sizeof(ArrowIpcOutputStream));

    // close_on_release=0: we manage the FILE* ourselves.
    int rc = ArrowIpcOutputStreamInitFile(os, file_, /*close_on_release=*/0);
    if (rc != NANOARROW_OK) {
        delete os;
        std::fclose(file_);
        file_ = nullptr;
        return rc;
    }

    auto* w = new (std::nothrow) ArrowIpcWriter;
    if (!w) {
        // os not yet consumed — release it manually
        if (os->release) os->release(os);
        delete os;
        std::fclose(file_);
        file_ = nullptr;
        return -1;
    }
    std::memset(w, 0, sizeof(ArrowIpcWriter));

    // ArrowIpcWriterInit takes ownership of *os (moves it internally).
    rc = ArrowIpcWriterInit(w, os);
    if (rc != NANOARROW_OK) {
        // Init failed: writer did not take ownership, release stream ourselves.
        if (os->release) os->release(os);
        delete os;
        delete w;
        std::fclose(file_);
        file_ = nullptr;
        return rc;
    }

    // Keep os pointer so we can delete the allocation in close().
    // The writer owns the stream contents; we only own the heap allocation.
    stream_ = os;
    writer_ = w;

    // Write IPC file magic.
    rc = ArrowIpcWriterStartFile(w, nullptr);
    if (rc != NANOARROW_OK) {
        ArrowIpcWriterReset(w);
        delete w;
        delete os;
        std::fclose(file_);
        reset_state();
        return rc;
    }

    return NANOARROW_OK;
}

// ---------------------------------------------------------------------------
// write_batch
// ---------------------------------------------------------------------------

int IpcWriter::write_batch(ArrowExportResult& batch) {
    if (!is_open() || !batch.valid()) return -1;

    ArrowIpcWriter* w = as_writer(writer_);
    ArrowSchema* schema = batch.get_schema();
    ArrowArray* array = batch.get_array();

    // Write schema once, before the first record batch.
    if (!schema_written_) {
        int rc = ArrowIpcWriterWriteSchema(w, schema, nullptr);
        if (rc != NANOARROW_OK) return rc;
        schema_written_ = true;
    }

    // Build an ArrowArrayView from the schema + array.
    ArrowArrayView view;
    ArrowError error;
    int rc = ArrowArrayViewInitFromSchema(&view, schema, &error);
    if (rc != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        return rc;
    }

    rc = ArrowArrayViewSetArray(&view, array, &error);
    if (rc != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        return rc;
    }

    rc = ArrowIpcWriterWriteArrayView(w, &view, nullptr);
    ArrowArrayViewReset(&view);
    return rc;
}

// ---------------------------------------------------------------------------
// close
// ---------------------------------------------------------------------------

int IpcWriter::close() {
    if (!is_open()) return 0;

    int rc = NANOARROW_OK;
    ArrowIpcWriter* w = as_writer(writer_);

    if (w && schema_written_) {
        rc = ArrowIpcWriterFinalizeFile(w, nullptr);
    }

    if (w) {
        ArrowIpcWriterReset(w);
        delete w;
    }

    // The stream allocation is ours; its contents were released by Reset.
    if (stream_) {
        delete as_stream(stream_);
    }

    std::fclose(file_);
    reset_state();
    return rc;
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
