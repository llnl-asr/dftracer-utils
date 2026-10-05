#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utils/timer.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/utilities/reader/error.h>
#include <dftracer/utils/utilities/reader/internal/gzip_reader.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <dftracer/utils/utilities/reader/internal/streams/gzip_byte_stream.h>
#include <dftracer/utils/utilities/reader/internal/streams/gzip_line_byte_stream.h>
#include <dftracer/utils/utilities/reader/internal/streams/line_stream.h>
#include <dftracer/utils/utilities/reader/internal/streams/multi_line_stream.h>
#include <dftracer/utils/utilities/reader/internal/string_line_processor.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

static void validate_parameters(
    const char *buffer, std::size_t buffer_size, std::size_t start_bytes,
    std::size_t end_bytes,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
    if (!buffer || buffer_size == 0) {
        throw dftracer::utils::utilities::reader::ReaderError(
            dftracer::utils::utilities::reader::ReaderError::INVALID_ARGUMENT,
            "Invalid buffer parameters");
    }
    if (start_bytes >= end_bytes) {
        throw dftracer::utils::utilities::reader::ReaderError(
            dftracer::utils::utilities::reader::ReaderError::INVALID_ARGUMENT,
            "start_bytes must be less than end_bytes");
    }
    if (max_bytes != SIZE_MAX) {
        if (end_bytes > max_bytes) {
            throw dftracer::utils::utilities::reader::ReaderError(
                dftracer::utils::utilities::reader::ReaderError::
                    INVALID_ARGUMENT,
                "end_bytes exceeds maximum available bytes");
        }
        if (start_bytes > max_bytes) {
            throw dftracer::utils::utilities::reader::ReaderError(
                dftracer::utils::utilities::reader::ReaderError::
                    INVALID_ARGUMENT,
                "start_bytes exceeds maximum available bytes");
        }
    }
}

static void check_reader_state(bool is_open, const void *indexer) {
    if (!is_open || !indexer) {
        throw dftracer::utils::utilities::reader::ReaderError(
            dftracer::utils::utilities::reader::ReaderError::
                INITIALIZATION_ERROR,
            "Reader is not open");
    }
}

static constexpr std::size_t DEFAULT_READER_BUFFER_SIZE = 1 * 1024 * 1024;

namespace dftracer::utils::utilities::reader::internal {

GzipReader::GzipReader(const std::string &gz_path_,
                       const std::string &idx_path_,
                       std::size_t index_ckpt_size)
    : gz_path(gz_path_),
      index_path(idx_path_),
      is_open(false),
      default_buffer_size(DEFAULT_READER_BUFFER_SIZE),
      indexer(nullptr) {
    try {
        indexer =
            dftracer::utils::index::gzip::CheckpointIndexerFactory::create(
                gz_path, index_path, index_ckpt_size, false);
        is_open = true;

        DFTRACER_UTILS_LOG_DEBUG(
            "Successfully created GZIP reader for gz: %s and index: %s",
            gz_path.c_str(), index_path.c_str());
    } catch (const std::exception &e) {
        throw ReaderError(ReaderError::INITIALIZATION_ERROR,
                          "Failed to initialize reader with indexer: " +
                              std::string(e.what()));
    }
}

GzipReader::GzipReader(
    std::shared_ptr<dftracer::utils::index::gzip::CheckpointIndexer> indexer_)
    : default_buffer_size(DEFAULT_READER_BUFFER_SIZE),
      indexer(std::move(indexer_)) {
    if (!indexer) {
        throw ReaderError(ReaderError::INITIALIZATION_ERROR,
                          "Invalid indexer provided");
    }
    is_open = true;
    gz_path = indexer->get_archive_path();
    index_path = indexer->get_index_path();
}

GzipReader::~GzipReader() {
    DFTRACER_UTILS_LOG_DEBUG("Destroying GZIP reader for gz: %s and index: %s",
                             gz_path.c_str(), index_path.c_str());
    reset();
    is_open = false;
}

GzipReader::GzipReader(GzipReader &&other) noexcept
    : gz_path(std::move(other.gz_path)),
      index_path(std::move(other.index_path)),
      is_open(other.is_open),
      default_buffer_size(other.default_buffer_size),
      indexer(std::move(other.indexer)) {
    other.is_open = false;
}

GzipReader &GzipReader::operator=(GzipReader &&other) noexcept {
    if (this != &other) {
        gz_path = std::move(other.gz_path);
        index_path = std::move(other.index_path);
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

const std::string &GzipReader::get_index_path() const { return index_path; }

void GzipReader::set_buffer_size(std::size_t size) {
    default_buffer_size = size;
}

void GzipReader::reset() {
    check_reader_state(is_open, indexer.get());
    stream_cache_.clear();
}

coro::CoroTask<std::size_t> GzipReader::read_async(std::size_t start_bytes,
                                                   std::size_t end_bytes,
                                                   char *buffer,
                                                   std::size_t buffer_size) {
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
        DFTRACER_UTILS_LOG_DEBUG("%s",
                                 "GzipReader::read - creating new byte stream");
        auto new_stream = stream(StreamConfig()
                                     .stream_type(StreamType::BYTES)
                                     .range_type(RangeType::BYTE_RANGE)
                                     .from(start_bytes)
                                     .to(end_bytes));
        stream_cache_.update(std::move(new_stream), StreamType::BYTES, gz_path,
                             start_bytes, end_bytes);
    } else {
        DFTRACER_UTILS_LOG_DEBUG(
            "%s", "GzipReader::read - reusing cached byte stream");
    }

    std::size_t result =
        co_await stream_cache_.get()->read_async(buffer, buffer_size);
    DFTRACER_UTILS_LOG_DEBUG("GzipReader::read - returned %zu bytes", result);

    // Update position for next potential read
    stream_cache_.update_position(start_bytes + result);

    co_return result;
}

coro::CoroTask<std::size_t> GzipReader::read_line_bytes_async(
    std::size_t start_bytes, std::size_t end_bytes, char *buffer,
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
        auto new_stream = stream(StreamConfig()
                                     .stream_type(StreamType::MULTI_LINES_BYTES)
                                     .range_type(RangeType::BYTE_RANGE)
                                     .from(start_bytes)
                                     .to(end_bytes));
        stream_cache_.update(std::move(new_stream),
                             StreamType::MULTI_LINES_BYTES, gz_path,
                             start_bytes, end_bytes);
    }

    std::size_t result =
        co_await stream_cache_.get()->read_async(buffer, buffer_size);

    // Update position for next potential read
    stream_cache_.update_position(start_bytes + result);

    co_return result;
}

coro::CoroTask<std::string> GzipReader::read_lines_async(std::size_t start_line,
                                                         std::size_t end_line) {
    check_reader_state(is_open, indexer.get());

    if (start_line == 0 || end_line == 0) {
        throw ReaderError(ReaderError::INVALID_ARGUMENT,
                          "Line numbers must be 1-based (start from 1)");
    }

    if (start_line > end_line) {
        throw ReaderError(ReaderError::INVALID_ARGUMENT,
                          "Start line must be <= end line");
    }

    std::size_t total_lines = indexer.get()->get_num_lines();
    if (start_line > total_lines || end_line > total_lines) {
        throw ReaderError(ReaderError::INVALID_ARGUMENT,
                          "Line numbers exceed total lines in file (" +
                              std::to_string(total_lines) + ")");
    }

    // Check if we can reuse cached stream
    if (!stream_cache_.can_continue(StreamType::MULTI_LINES, gz_path,
                                    start_line, end_line)) {
        auto new_stream = stream(StreamConfig()
                                     .stream_type(StreamType::MULTI_LINES)
                                     .range_type(RangeType::LINE_RANGE)
                                     .from(start_line)
                                     .to(end_line));
        stream_cache_.update(std::move(new_stream), StreamType::MULTI_LINES,
                             gz_path, start_line, end_line);
    }

    std::string result;
    // Pre-allocate to avoid reallocations (like old StringLineProcessor)
    std::size_t estimated_lines = end_line - start_line + 1;
    result.reserve(estimated_lines * 100);  // Estimate ~100 bytes per line

    std::vector<char> buffer(default_buffer_size);

    while (!stream_cache_.get()->done()) {
        std::size_t bytes_read = co_await stream_cache_.get()->read_async(
            buffer.data(), buffer.size());
        if (bytes_read == 0) break;

        result.append(buffer.data(), bytes_read);
    }

    co_return result;
}

coro::CoroTask<void> GzipReader::read_lines_with_processor_async(
    std::size_t start_line, std::size_t end_line, LineProcessor &processor) {
    check_reader_state(is_open, indexer.get());

    if (start_line == 0 || end_line == 0) {
        throw ReaderError(ReaderError::INVALID_ARGUMENT,
                          "Line numbers must be 1-based (start from 1)");
    }

    if (start_line > end_line) {
        throw ReaderError(ReaderError::INVALID_ARGUMENT,
                          "Start line must be <= end line");
    }

    std::size_t total_lines = indexer.get()->get_num_lines();
    if (start_line > total_lines || end_line > total_lines) {
        throw ReaderError(ReaderError::INVALID_ARGUMENT,
                          "Line numbers exceed total lines in file (" +
                              std::to_string(total_lines) + ")");
    }

    processor.begin(start_line, end_line);

    // Create a LineStream that returns one line at a time
    auto line_stream = stream(StreamConfig()
                                  .stream_type(StreamType::LINE)
                                  .range_type(RangeType::LINE_RANGE)
                                  .from(start_line)
                                  .to(end_line));

    std::vector<char> buffer(default_buffer_size);

    while (!line_stream->done()) {
        std::size_t bytes_read =
            co_await line_stream->read_async(buffer.data(), buffer.size());
        if (bytes_read == 0) break;

        // LineStream returns one complete line with \n
        // Processor expects line without \n
        std::size_t line_length = bytes_read;
        if (line_length > 0 && buffer[line_length - 1] == '\n') {
            line_length--;
        }

        if (!co_await processor.process(buffer.data(), line_length)) {
            processor.end();
            co_return;
        }
    }

    processor.end();
}

coro::CoroTask<void> GzipReader::read_line_bytes_with_processor_async(
    std::size_t start_bytes, std::size_t end_bytes, LineProcessor &processor) {
    check_reader_state(is_open, indexer.get());

    if (end_bytes > indexer.get()->get_max_bytes()) {
        end_bytes = indexer.get()->get_max_bytes();
    }

    if (start_bytes >= end_bytes) {
        co_return;
    }

    processor.begin(start_bytes, end_bytes);

    auto lines_stream = stream(StreamConfig()
                                   .stream_type(StreamType::LINE_BYTES)
                                   .range_type(RangeType::BYTE_RANGE)
                                   .from(start_bytes)
                                   .to(end_bytes));

    std::vector<char> buffer(default_buffer_size);

    while (!lines_stream->done()) {
        std::size_t bytes_read =
            co_await lines_stream->read_async(buffer.data(), buffer.size());
        if (bytes_read == 0) break;
        co_await processor.process(buffer.data(), bytes_read);
    }

    processor.end();
}

bool GzipReader::is_valid() const { return is_open && indexer.get(); }

std::string GzipReader::get_format_name() const { return "GZIP"; }

std::unique_ptr<ReaderStream> GzipReader::stream(const StreamConfig &config) {
    check_reader_state(is_open, indexer.get());

    // Extract config parameters
    StreamType stream_type = config.stream_type();
    RangeType range_type = config.range_type();
    std::size_t start = config.start();
    std::size_t end = config.end();
    std::size_t buffer_size = config.buffer_size();
    bool extend_to_line_boundary = config.extend_to_line_boundary();

    // Convert line range to byte range if needed
    std::size_t start_bytes = start;
    std::size_t end_bytes = end;
    std::size_t actual_start_line =
        1;  // Track what line number start_bytes corresponds to

    if (range_type == RangeType::LINE_RANGE) {
        // Convert line numbers to byte offsets using the member table
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

        // Members carry the same line bookkeeping as the old recovery
        // points, and their starts are decodable without a dictionary.
        auto members = indexer->get_members();

        std::size_t last_overlap_end = 0;
        bool found_overlap = false;
        for (const auto &member : members) {
            if (member.first_line_num <= end && member.last_line_num >= start) {
                last_overlap_end = member.uc_offset + member.uc_size;
                found_overlap = true;
            }
        }

        if (!found_overlap) {
            start_bytes = 0;
            end_bytes = indexer->get_max_bytes();
            actual_start_line = 1;
            DFTRACER_UTILS_LOG_DEBUG(
                "No member covers lines %zu-%zu, using full file: "
                "end_bytes=%zu",
                start, end, end_bytes);
        } else {
            // Members are line-aligned (each written member is a whole-line
            // batch), so a member's uc_offset is the first byte of its
            // first_line_num. Begin decoding at the member containing `start`
            // and label the first decoded line as that member's first line;
            // LineStream then drops [first_line_num, start) and serves
            // [start, end]. Decoding from the containing member avoids
            // re-inflating earlier members.
            // A restart piece does not start at a line start, so only
            // whole members and the heads of split members qualify.
            bool found_start = false;
            for (const auto &member : members) {
                if (member.kind != index::gzip::GzipRecordKind::RESTART &&
                    member.first_line_num <= start) {
                    start_bytes = member.uc_offset;
                    actual_start_line = member.first_line_num;
                    found_start = true;
                }
            }
            if (!found_start) {
                start_bytes = 0;
                actual_start_line = 1;
            }
            end_bytes = last_overlap_end;

            DFTRACER_UTILS_LOG_DEBUG(
                "Lines %zu-%zu -> byte range %zu-%zu, actual_start_line=%zu",
                start, end, start_bytes, end_bytes, actual_start_line);
        }
    }

    // Create appropriate stream type
    switch (stream_type) {
        case StreamType::BYTES: {
            auto byte_stream = std::make_unique<GzipByteStream>(buffer_size);
            byte_stream->initialize(gz_path, start_bytes, end_bytes, *indexer);
            return byte_stream;
        }
        case StreamType::LINE_BYTES: {
            // Single line-aligned bytes at a time
            auto line_byte_stream =
                std::make_unique<GzipLineByteStream>(buffer_size);
            line_byte_stream->set_extend_to_line_boundary(
                extend_to_line_boundary);
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
            auto line_byte_stream =
                std::make_unique<GzipLineByteStream>(buffer_size);
            line_byte_stream->set_extend_to_line_boundary(
                extend_to_line_boundary);
            line_byte_stream->initialize(gz_path, start_bytes, end_bytes,
                                         *indexer);
            return line_byte_stream;
        }
        case StreamType::LINE: {
            // Single parsed line per read
            auto line_byte_stream =
                std::make_unique<GzipLineByteStream>(buffer_size);
            line_byte_stream->set_extend_to_line_boundary(
                extend_to_line_boundary);
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
            auto line_byte_stream =
                std::make_unique<GzipLineByteStream>(buffer_size);
            line_byte_stream->set_extend_to_line_boundary(
                extend_to_line_boundary);
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

}  // namespace dftracer::utils::utilities::reader::internal
