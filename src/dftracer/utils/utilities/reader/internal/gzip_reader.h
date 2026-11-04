// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_GZIP_READER_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_GZIP_READER_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/utilities/reader/internal/line_processor.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_stream_cache.h>

#include <cstddef>

namespace dftracer::utils::utilities::reader::internal {
class GzipReader : public Reader {
   public:
    GzipReader(dstd::StringView gz_path, dstd::StringView index_path,
               std::size_t index_ckpt_size = dftracer::utils::index::gzip::
                   CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE);
    explicit GzipReader(
        dstd::SharedPtr<dftracer::utils::index::gzip::CheckpointIndexer>
            indexer);
    ~GzipReader();

    // Disable copy constructor and copy assignment
    GzipReader(const GzipReader &) = delete;
    GzipReader &operator=(const GzipReader &) = delete;
    GzipReader(GzipReader &&other) noexcept;
    GzipReader &operator=(GzipReader &&other) noexcept;

    // Reader interface implementation
    std::size_t get_max_bytes() const override;
    std::size_t get_num_lines() const override;
    const dstd::String &get_archive_path() const override;
    const dstd::String &get_index_path() const override;
    void set_buffer_size(std::size_t size) override;

    coro::CoroTask<std::size_t> read_async(std::size_t start_bytes,
                                           std::size_t end_bytes, char *buffer,
                                           std::size_t buffer_size) override;
    coro::CoroTask<std::size_t> read_line_bytes_async(
        std::size_t start_bytes, std::size_t end_bytes, char *buffer,
        std::size_t buffer_size) override;
    coro::CoroTask<dstd::String> read_lines_async(std::size_t start,
                                                  std::size_t end) override;
    coro::CoroTask<void> read_lines_with_processor_async(
        std::size_t start, std::size_t end, LineProcessor &processor) override;
    coro::CoroTask<void> read_line_bytes_with_processor_async(
        std::size_t start_bytes, std::size_t end_bytes,
        LineProcessor &processor) override;

    dstd::UniquePtr<ReaderStream> stream(const StreamConfig &config) override;

    void reset() override;
    bool is_valid() const override;
    dstd::String get_format_name() const override;

   private:
    dstd::String gz_path_;
    dstd::String index_path_;
    bool is_open_;
    std::size_t default_buffer_size_;
    dstd::SharedPtr<dftracer::utils::index::gzip::CheckpointIndexer> indexer_;
    ReaderStreamCache stream_cache_;
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_GZIP_READER_H
