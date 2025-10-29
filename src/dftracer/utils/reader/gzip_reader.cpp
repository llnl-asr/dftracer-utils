#include <dftracer/utils/core/utils/timer.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/indexer/indexer_factory.h>
#include <dftracer/utils/reader/error.h>
#include <dftracer/utils/reader/gzip_reader.h>
#include <dftracer/utils/reader/streams/gzip_byte_stream.h>
#include <dftracer/utils/reader/streams/gzip_line_byte_stream.h>
#include <dftracer/utils/reader/streams/line_stream.h>
#include <dftracer/utils/reader/streams/multi_line_stream.h>
#include <dftracer/utils/reader/string_line_processor.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

static void validate_parameters(
    const char *buffer, std::size_t buffer_size, std::size_t start_bytes,
    std::size_t end_bytes,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
    if (!buffer || buffer_size == 0) {
        throw ReaderError(ReaderError::INVALID_ARGUMENT,
                          "Invalid buffer parameters");
    }
    if (start_bytes >= end_bytes) {
        throw ReaderError(ReaderError::INVALID_ARGUMENT,
                          "start_bytes must be less than end_bytes");
    }
    if (max_bytes != SIZE_MAX) {
        if (end_bytes > max_bytes) {
            throw ReaderError(ReaderError::INVALID_ARGUMENT,
                              "end_bytes exceeds maximum available bytes");
        }
        if (start_bytes > max_bytes) {
            throw ReaderError(ReaderError::INVALID_ARGUMENT,
                              "start_bytes exceeds maximum available bytes");
        }
    }
}

static void check_reader_state(bool is_open, const void *indexer) {
    if (!is_open || !indexer) {
        throw std::runtime_error("Reader is not open");
    }
}

static constexpr std::size_t DEFAULT_READER_BUFFER_SIZE = 1 * 1024 * 1024;

namespace dftracer::utils {

GzipReader::GzipReader(const std::string &gz_path_,
                       const std::string &idx_path_,
                       std::size_t index_ckpt_size)
    : gz_path(gz_path_),
      idx_path(idx_path_),
      is_open(false),
      default_buffer_size(DEFAULT_READER_BUFFER_SIZE),
      indexer(nullptr) {
    try {
        indexer =
            IndexerFactory::create(gz_path, idx_path, index_ckpt_size, false);
        is_open = true;

        DFTRACER_UTILS_LOG_DEBUG(
            "Successfully created GZIP reader for gz: %s and index: %s",
            gz_path.c_str(), idx_path.c_str());
    } catch (const std::exception &e) {
        throw ReaderError(ReaderError::INITIALIZATION_ERROR,
                          "Failed to initialize reader with indexer: " +
                              std::string(e.what()));
    }
}

GzipReader::GzipReader(std::shared_ptr<Indexer> indexer_)
    : default_buffer_size(DEFAULT_READER_BUFFER_SIZE),
      indexer(std::move(indexer_)) {
    if (!indexer) {
        throw ReaderError(ReaderError::INITIALIZATION_ERROR,
                          "Invalid indexer provided");
    }
    is_open = true;
    gz_path = indexer->get_archive_path();
    idx_path = indexer->get_idx_path();
}

GzipReader::~GzipReader() {
    DFTRACER_UTILS_LOG_DEBUG("Destroying GZIP reader for gz: %s and index: %s",
                             gz_path.c_str(), idx_path.c_str());
    reset();
    is_open = false;
}

GzipReader::GzipReader(GzipReader &&other) noexcept
    : gz_path(std::move(other.gz_path)),
      idx_path(std::move(other.idx_path)),
      is_open(other.is_open),
      default_buffer_size(other.default_buffer_size),
      indexer(std::move(other.indexer)) {
    other.is_open = false;
}

GzipReader &GzipReader::operator=(GzipReader &&other) noexcept {
    if (this != &other) {
        gz_path = std::move(other.gz_path);
        idx_path = std::move(other.idx_path);
        is_open = other.is_open;
        default_buffer_size = other.default_buffer_size;
        indexer = std::move(other.indexer);
        other.is_open = false;
    }
    return *this;
}

std::size_t GzipReader::get_max_bytes() const {
    check_reader_state(is_open, indexer.get());
    std::size_t max_bytes =
        static_cast<std::size_t>(indexer.get()->get_max_bytes());
    DFTRACER_UTILS_LOG_DEBUG("Maximum bytes available: %zu", max_bytes);
    return max_bytes;
}

std::size_t GzipReader::get_num_lines() const {
    check_reader_state(is_open, indexer.get());
    std::size_t num_lines = static_cast<std::size_t>(indexer->get_num_lines());
    DFTRACER_UTILS_LOG_DEBUG("Total lines available: %zu", num_lines);
    return num_lines;
}

const std::string &GzipReader::get_archive_path() const { return gz_path; }

const std::string &GzipReader::get_idx_path() const { return idx_path; }

void GzipReader::set_buffer_size(std::size_t size) {
    default_buffer_size = size;
}

void GzipReader::reset() {
    check_reader_state(is_open, indexer.get());
    stream_cache_.clear();
}

std::size_t GzipReader::read(std::size_t start_bytes, std::size_t end_bytes,
                             char *buffer, std::size_t buffer_size) {
    check_reader_state(is_open, indexer.get());
    validate_parameters(buffer, buffer_size, start_bytes, end_bytes,
                        indexer.get()->get_max_bytes());

    DFTRACER_UTILS_LOG_DEBUG(
        "GzipReader::read - request: start_bytes=%zu, end_bytes=%zu, "
        "buffer_size=%zu",
        start_bytes, end_bytes, buffer_size);

    // Check if we can reuse cached stream
    if (!stream_cache_.can_continue(StreamType::BYTES, gz_path, start_bytes,
                                    end_bytes)) {
        DFTRACER_UTILS_LOG_DEBUG("GzipReader::read - creating new byte stream",
                                 "");
        auto new_stream = stream(StreamType::BYTES, RangeType::BYTE_RANGE,
                                 start_bytes, end_bytes);
        stream_cache_.update(std::move(new_stream), StreamType::BYTES, gz_path,
                             start_bytes, end_bytes);
    } else {
        DFTRACER_UTILS_LOG_DEBUG(
            "GzipReader::read - reusing cached byte stream", "");
    }

    std::size_t result = stream_cache_.get()->read(buffer, buffer_size);
    DFTRACER_UTILS_LOG_DEBUG("GzipReader::read - returned %zu bytes", result);

    // Update position for next potential read
    stream_cache_.update_position(start_bytes + result);

    return result;
}

std::size_t GzipReader::read_line_bytes(std::size_t start_bytes,
                                        std::size_t end_bytes, char *buffer,
                                        std::size_t buffer_size) {
    check_reader_state(is_open, indexer.get());

    if (end_bytes > indexer.get()->get_max_bytes()) {
        end_bytes = indexer.get()->get_max_bytes();
    }

    validate_parameters(buffer, buffer_size, start_bytes, end_bytes,
                        indexer.get()->get_max_bytes());

    // Check if we can reuse cached stream
    if (!stream_cache_.can_continue(StreamType::MULTI_LINES_BYTES, gz_path,
                                    start_bytes, end_bytes)) {
        auto new_stream = stream(StreamType::MULTI_LINES_BYTES,
                                 RangeType::BYTE_RANGE, start_bytes, end_bytes);
        stream_cache_.update(std::move(new_stream),
                             StreamType::MULTI_LINES_BYTES, gz_path,
                             start_bytes, end_bytes);
    }

    std::size_t result = stream_cache_.get()->read(buffer, buffer_size);

    // Update position for next potential read
    stream_cache_.update_position(start_bytes + result);

    return result;
}

std::string GzipReader::read_lines(std::size_t start_line,
                                   std::size_t end_line) {
    check_reader_state(is_open, indexer.get());

    if (start_line == 0 || end_line == 0) {
        throw std::runtime_error("Line numbers must be 1-based (start from 1)");
    }

    if (start_line > end_line) {
        throw std::runtime_error("Start line must be <= end line");
    }

    std::size_t total_lines = indexer.get()->get_num_lines();
    if (start_line > total_lines || end_line > total_lines) {
        throw std::runtime_error("Line numbers exceed total lines in file (" +
                                 std::to_string(total_lines) + ")");
    }

    // Check if we can reuse cached stream
    if (!stream_cache_.can_continue(StreamType::MULTI_LINES, gz_path,
                                    start_line, end_line)) {
        auto new_stream = stream(StreamType::MULTI_LINES, RangeType::LINE_RANGE,
                                 start_line, end_line);
        stream_cache_.update(std::move(new_stream), StreamType::MULTI_LINES,
                             gz_path, start_line, end_line);
    }

    std::string result;
    // Pre-allocate to avoid reallocations (like old StringLineProcessor)
    std::size_t estimated_lines = end_line - start_line + 1;
    result.reserve(estimated_lines * 100);  // Estimate ~100 bytes per line

    std::vector<char> buffer(default_buffer_size);

    while (!stream_cache_.get()->done()) {
        std::size_t bytes_read =
            stream_cache_.get()->read(buffer.data(), buffer.size());
        if (bytes_read == 0) break;

        result.append(buffer.data(), bytes_read);
    }

    return result;
}

void GzipReader::read_lines_with_processor(std::size_t start_line,
                                           std::size_t end_line,
                                           LineProcessor &processor) {
    check_reader_state(is_open, indexer.get());

    if (start_line == 0 || end_line == 0) {
        throw std::runtime_error("Line numbers must be 1-based (start from 1)");
    }

    if (start_line > end_line) {
        throw std::runtime_error("Start line must be <= end line");
    }

    std::size_t total_lines = indexer.get()->get_num_lines();
    if (start_line > total_lines || end_line > total_lines) {
        throw std::runtime_error("Line numbers exceed total lines in file (" +
                                 std::to_string(total_lines) + ")");
    }

    processor.begin(start_line, end_line);

    // Create a LineStream that returns one line at a time
    auto line_stream =
        stream(StreamType::LINE, RangeType::LINE_RANGE, start_line, end_line);

    std::vector<char> buffer(default_buffer_size);

    while (!line_stream->done()) {
        std::size_t bytes_read =
            line_stream->read(buffer.data(), buffer.size());
        if (bytes_read == 0) break;

        // LineStream returns one complete line with \n
        // Processor expects line without \n
        std::size_t line_length = bytes_read;
        if (line_length > 0 && buffer[line_length - 1] == '\n') {
            line_length--;
        }

        if (!processor.process(buffer.data(), line_length)) {
            processor.end();
            return;
        }
    }

    processor.end();
}

void GzipReader::read_line_bytes_with_processor(std::size_t start_bytes,
                                                std::size_t end_bytes,
                                                LineProcessor &processor) {
    check_reader_state(is_open, indexer.get());

    if (end_bytes > indexer.get()->get_max_bytes()) {
        end_bytes = indexer.get()->get_max_bytes();
    }

    if (start_bytes >= end_bytes) {
        return;
    }

    processor.begin(start_bytes, end_bytes);

    auto lines_stream = stream(StreamType::LINE_BYTES, RangeType::BYTE_RANGE,
                               start_bytes, end_bytes);

    std::vector<char> buffer(default_buffer_size);

    while (!lines_stream->done()) {
        std::size_t bytes_read =
            lines_stream->read(buffer.data(), buffer.size());
        if (bytes_read == 0) break;
        processor.process(buffer.data(), bytes_read);
    }

    processor.end();
}

bool GzipReader::is_valid() const { return is_open && indexer.get(); }

std::string GzipReader::get_format_name() const { return "GZIP"; }

std::unique_ptr<ReaderStream> GzipReader::stream(StreamType stream_type,
                                                 RangeType range_type,
                                                 std::size_t start,
                                                 std::size_t end) {
    check_reader_state(is_open, indexer.get());

    // Convert line range to byte range if needed
    std::size_t start_bytes = start;
    std::size_t end_bytes = end;
    std::size_t actual_start_line =
        1;  // Track what line number start_bytes corresponds to

    if (range_type == RangeType::LINE_RANGE) {
        // Convert line numbers to byte offsets using checkpoints
        if (start == 0 || end == 0) {
            throw ReaderError(ReaderError::INVALID_ARGUMENT,
                              "Line numbers must be 1-based (start from 1)");
        }
        if (start > end) {
            throw ReaderError(ReaderError::INVALID_ARGUMENT,
                              "Start line must be <= end line");
        }

        std::size_t total_lines = indexer->get_num_lines();
        if (start > total_lines || end > total_lines) {
            throw ReaderError(ReaderError::INVALID_ARGUMENT,
                              "Line numbers exceed total lines in file (" +
                                  std::to_string(total_lines) + ")");
        }

        // Get checkpoints for the line range
        std::vector<IndexerCheckpoint> checkpoints =
            indexer->get_checkpoints_for_line_range(start, end);

        DFTRACER_UTILS_LOG_DEBUG("Line range %zu-%zu: found %zu checkpoints",
                                 start, end, checkpoints.size());

        if (checkpoints.empty()) {
            // No checkpoints, read from beginning
            start_bytes = 0;
            end_bytes = indexer->get_max_bytes();
            actual_start_line = 1;
            DFTRACER_UTILS_LOG_DEBUG(
                "No checkpoints found, using full file: start_bytes=%zu, "
                "end_bytes=%zu, max_bytes=%zu",
                start_bytes, end_bytes, indexer->get_max_bytes());
        } else {
            // Use checkpoint to determine byte range
            if (checkpoints[0].checkpoint_idx == 0) {
                start_bytes = 0;
                actual_start_line = 1;
            } else {
                // Get previous checkpoint to find starting offset and line
                // number
                auto all_checkpoints = indexer->get_checkpoints();
                // Default to first checkpoint's first line if we can't find
                // previous
                for (const auto &prev_ckpt : all_checkpoints) {
                    if (prev_ckpt.checkpoint_idx ==
                        checkpoints[0].checkpoint_idx - 1) {
                        start_bytes = prev_ckpt.uc_offset;
                        // Line number after previous checkpoint's last line
                        actual_start_line = prev_ckpt.last_line_num + 1;
                        break;
                    }
                }
            }

            const auto &last_checkpoint = checkpoints.back();
            end_bytes = last_checkpoint.uc_offset + last_checkpoint.uc_size;

            DFTRACER_UTILS_LOG_DEBUG(
                "Using checkpoints: start_checkpoint_idx=%zu (first_line=%zu, "
                "last_line=%zu), "
                "end_checkpoint_idx=%zu (first_line=%zu, last_line=%zu), "
                "byte_range=%zu-%zu, actual_start_line=%zu",
                checkpoints[0].checkpoint_idx, checkpoints[0].first_line_num,
                checkpoints[0].last_line_num, last_checkpoint.checkpoint_idx,
                last_checkpoint.first_line_num, last_checkpoint.last_line_num,
                start_bytes, end_bytes, actual_start_line);
        }
    }

    // Create appropriate stream type
    switch (stream_type) {
        case StreamType::BYTES: {
            auto byte_stream = std::make_unique<GzipByteStream>();
            byte_stream->initialize(gz_path, start_bytes, end_bytes, *indexer);
            return byte_stream;
        }
        case StreamType::LINE_BYTES: {
            // Single line-aligned bytes at a time
            auto line_byte_stream = std::make_unique<GzipLineByteStream>();
            line_byte_stream->initialize(gz_path, start_bytes, end_bytes,
                                         *indexer);

            // Wrap with LineStream to return one line-aligned chunk at a time
            if (range_type == RangeType::LINE_RANGE) {
                return std::make_unique<LineStream>(
                    std::move(line_byte_stream), start, end, actual_start_line);
            } else {
                return std::make_unique<LineStream>(
                    std::move(line_byte_stream));
            }
        }
        case StreamType::MULTI_LINES_BYTES: {
            // Multiple line-aligned bytes per read
            auto line_byte_stream = std::make_unique<GzipLineByteStream>();
            line_byte_stream->initialize(gz_path, start_bytes, end_bytes,
                                         *indexer);
            return line_byte_stream;
        }
        case StreamType::LINE: {
            // Single parsed line per read
            auto line_byte_stream = std::make_unique<GzipLineByteStream>();
            line_byte_stream->initialize(gz_path, start_bytes, end_bytes,
                                         *indexer);

            if (range_type == RangeType::LINE_RANGE) {
                return std::make_unique<LineStream>(
                    std::move(line_byte_stream), start, end, actual_start_line);
            } else {
                return std::make_unique<LineStream>(
                    std::move(line_byte_stream));
            }
        }
        case StreamType::MULTI_LINES: {
            // Multiple parsed lines per read
            auto line_byte_stream = std::make_unique<GzipLineByteStream>();
            line_byte_stream->initialize(gz_path, start_bytes, end_bytes,
                                         *indexer);

            if (range_type == RangeType::LINE_RANGE) {
                return std::make_unique<MultiLineStream>(
                    std::move(line_byte_stream), start, end, actual_start_line);
            } else {
                return std::make_unique<MultiLineStream>(
                    std::move(line_byte_stream));
            }
        }
        default:
            throw ReaderError(ReaderError::INVALID_ARGUMENT,
                              "Invalid stream type");
    }
}

}  // namespace dftracer::utils
