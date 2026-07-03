#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/common/arrow/array_view.h>
#include <dftracer/utils/utilities/common/arrow/partition_writer.h>
#include <nanoarrow/nanoarrow.h>

#include <iomanip>
#include <sstream>

namespace dftracer::utils::utilities::common::arrow {

PartitionWriter::~PartitionWriter() {}

PartitionWriter::PartitionWriter(PartitionWriter&& other) noexcept
    : output_dir_(std::move(other.output_dir_)),
      chunk_size_bytes_(other.chunk_size_bytes_),
      compression_(other.compression_),
      writer_(std::move(other.writer_)),
      is_open_(other.is_open_),
      file_index_(other.file_index_),
      current_file_bytes_(other.current_file_bytes_),
      current_file_rows_(other.current_file_rows_),
      total_bytes_(other.total_bytes_),
      total_rows_(other.total_rows_),
      files_(std::move(other.files_)),
      row_counts_(std::move(other.row_counts_)) {
    other.is_open_ = false;
    other.file_index_ = 0;
    other.current_file_bytes_ = 0;
    other.current_file_rows_ = 0;
    other.total_bytes_ = 0;
    other.total_rows_ = 0;
}

PartitionWriter& PartitionWriter::operator=(PartitionWriter&& other) noexcept {
    if (this != &other) {
        output_dir_ = std::move(other.output_dir_);
        chunk_size_bytes_ = other.chunk_size_bytes_;
        compression_ = other.compression_;
        writer_ = std::move(other.writer_);
        is_open_ = other.is_open_;
        file_index_ = other.file_index_;
        current_file_bytes_ = other.current_file_bytes_;
        current_file_rows_ = other.current_file_rows_;
        total_bytes_ = other.total_bytes_;
        total_rows_ = other.total_rows_;
        files_ = std::move(other.files_);
        row_counts_ = std::move(other.row_counts_);

        other.is_open_ = false;
        other.file_index_ = 0;
        other.current_file_bytes_ = 0;
        other.current_file_rows_ = 0;
        other.total_bytes_ = 0;
        other.total_rows_ = 0;
    }
    return *this;
}

std::string PartitionWriter::generate_filename() const {
    std::ostringstream ss;
    ss << "part-" << std::setw(5) << std::setfill('0') << file_index_
       << ".arrow";
    return (fs::path(output_dir_) / ss.str()).string();
}

coro::CoroTask<int> PartitionWriter::open(const std::string& output_dir,
                                          int64_t chunk_size_bytes,
                                          IpcCompression compression) {
    if (is_open_) co_return -1;

    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec) co_return -1;

    output_dir_ = output_dir;
    chunk_size_bytes_ = chunk_size_bytes;
    compression_ = compression;
    file_index_ = 0;
    current_file_bytes_ = 0;
    current_file_rows_ = 0;
    total_bytes_ = 0;
    total_rows_ = 0;
    files_.clear();
    row_counts_.clear();

    std::string path = generate_filename();
    int rc = co_await writer_.open(path, compression_);
    if (rc != 0) co_return rc;

    is_open_ = true;
    co_return 0;
}

int64_t PartitionWriter::calculate_uncompressed_size(ArrowExportResult& batch) {
    ArrowSchema* schema = batch.get_schema();
    ArrowArray* array = batch.get_array();

    ArrowArrayView view;
    if (init_array_view(view, schema, array) != NANOARROW_OK) {
        return 0;
    }

    // Calculate total buffer size recursively
    int64_t total = 0;
    struct BufferCounter {
        static void count(ArrowArrayView* v, int64_t& total, bool is_root) {
            if (!is_root) {
                int64_t num_buffers = ArrowArrayViewGetNumBuffers(v);
                for (int64_t i = 0; i < num_buffers; i++) {
                    ArrowBufferView buf = ArrowArrayViewGetBufferView(v, i);
                    total += buf.size_bytes;
                }
            }
            for (int64_t i = 0; i < v->n_children; i++) {
                count(v->children[i], total, false);
            }
        }
    };
    BufferCounter::count(&view, total, true);

    ArrowArrayViewReset(&view);
    return total;
}

coro::CoroTask<int> PartitionWriter::rotate_file() {
    co_await writer_.close();

    files_.push_back(generate_filename());
    row_counts_.push_back(current_file_rows_);

    file_index_++;
    current_file_bytes_ = 0;
    current_file_rows_ = 0;

    std::string path = generate_filename();
    co_return co_await writer_.open(path, compression_);
}

coro::CoroTask<int> PartitionWriter::write_batch(ArrowExportResult& batch) {
    if (!is_open_ || !batch.valid()) co_return -1;

    int64_t batch_size = calculate_uncompressed_size(batch);
    int64_t batch_rows = batch.num_rows();

    // Check if we need to rotate before writing
    // (only rotate if we've written something and adding this batch exceeds
    // limit)
    if (chunk_size_bytes_ > 0 && current_file_bytes_ > 0 &&
        current_file_bytes_ + batch_size > chunk_size_bytes_) {
        int rc = co_await rotate_file();
        if (rc != 0) co_return rc;
    }

    int rc = co_await writer_.write_batch(batch);
    if (rc != 0) co_return rc;

    current_file_bytes_ += batch_size;
    current_file_rows_ += batch_rows;
    total_bytes_ += batch_size;
    total_rows_ += batch_rows;

    co_return 0;
}

coro::CoroTask<PartitionWriteStats> PartitionWriter::close() {
    PartitionWriteStats stats;

    if (!is_open_) co_return stats;

    co_await writer_.close();

    // Record final file stats (only if rows were written)
    if (current_file_rows_ > 0) {
        files_.push_back(generate_filename());
        row_counts_.push_back(current_file_rows_);
    }

    stats.files = std::move(files_);
    stats.row_counts = std::move(row_counts_);
    stats.total_rows = total_rows_;
    stats.total_uncompressed_bytes = total_bytes_;

    is_open_ = false;
    file_index_ = 0;
    current_file_bytes_ = 0;
    current_file_rows_ = 0;
    total_bytes_ = 0;
    total_rows_ = 0;

    co_return stats;
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
