// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/trace/reader/error.h>
#include <dftracer/utils/trace/reader/internal/gzip_reader.h>
#include <dftracer/utils/trace/reader/internal/stream_config.h>
#include <dftracer/utils/trace/reader/internal/streams/gzip_byte_stream.h>
#include <dftracer/utils/trace/reader/internal/streams/gzip_line_byte_stream.h>
#include <dftracer/utils/trace/reader/internal/streams/line_stream.h>
#include <dftracer/utils/trace/reader/internal/streams/multi_line_stream.h>
#include <dftracer/utils/trace/reader/internal/string_line_processor.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>

using dftracer::utils::trace::reader::reader_error;
using dftracer::utils::trace::reader::ReaderErrorType;

static dftracer::utils::dstd::Result<void> validate_parameters(
    const char *buffer, std::size_t buffer_size, std::size_t start_bytes,
    std::size_t end_bytes,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
    if (!buffer || buffer_size == 0) {
        return reader_error(ReaderErrorType::InvalidArgument,
                            "Invalid buffer parameters");
    }
    if (start_bytes >= end_bytes) {
        return reader_error(ReaderErrorType::InvalidArgument,
                            "start_bytes must be less than end_bytes");
    }
    if (max_bytes != SIZE_MAX) {
        if (end_bytes > max_bytes) {
            return reader_error(ReaderErrorType::InvalidArgument,
                                "end_bytes exceeds maximum available bytes");
        }
        if (start_bytes > max_bytes) {
            return reader_error(ReaderErrorType::InvalidArgument,
                                "start_bytes exceeds maximum available bytes");
        }
    }
    return {};
}

static dftracer::utils::dstd::Result<void> validate_line_range(
    std::size_t start_line, std::size_t end_line, std::size_t total_lines) {
    if (start_line == 0 || end_line == 0) {
        return reader_error(ReaderErrorType::InvalidArgument,
                            "Line numbers must be 1-based (start from 1)");
    }
    if (start_line > end_line) {
        return reader_error(ReaderErrorType::InvalidArgument,
                            "Start line must be <= end line");
    }
    if (start_line > total_lines || end_line > total_lines) {
        return reader_error<ReaderErrorType::InvalidArgument,
                            "Line numbers exceed total lines in file ({})">(
            total_lines);
    }
    return {};
}

static void check_reader_state(bool is_open, const void *indexer) {
    if (!is_open || !indexer) {
        dftracer::utils::dstd::fatal("GzipReader used after it was moved from");
    }
}

static constexpr std::size_t DEFAULT_READER_BUFFER_SIZE = 1 * 1024 * 1024;

namespace dftracer::utils::trace::reader::internal {

GzipReader::GzipReader(
    dstd::SharedPtr<dftracer::utils::trace::index::gzip::CheckpointIndexer>
        indexer)
    : default_buffer_size_(DEFAULT_READER_BUFFER_SIZE),
      indexer_(std::move(indexer)) {
    if (!indexer_) {
        dstd::fatal("GzipReader needs an indexer");
    }
    is_open_ = true;
    gz_path_ = dstd::String(indexer_->archive_path());
    index_path_ = dstd::String(indexer_->index_path());
}

GzipReader::~GzipReader() {
    DFTRACER_UTILS_LOG_DEBUG("Destroying GZIP reader for gz: {} and index: {}",
                             gz_path_, index_path_);
    stream_cache_.clear();
    is_open_ = false;
}

GzipReader::GzipReader(GzipReader &&other) noexcept
    : gz_path_(std::move(other.gz_path_)),
      index_path_(std::move(other.index_path_)),
      is_open_(other.is_open_),
      default_buffer_size_(other.default_buffer_size_),
      indexer_(std::move(other.indexer_)) {
    other.is_open_ = false;
}

GzipReader &GzipReader::operator=(GzipReader &&other) noexcept {
    if (this != &other) {
        gz_path_ = std::move(other.gz_path_);
        index_path_ = std::move(other.index_path_);
        is_open_ = other.is_open_;
        default_buffer_size_ = other.default_buffer_size_;
        indexer_ = std::move(other.indexer_);
        other.is_open_ = false;
    }
    return *this;
}

std::size_t GzipReader::max_bytes() const {
    check_reader_state(is_open_, indexer_.get());
    std::size_t max_bytes =
        static_cast<std::size_t>(indexer_.get()->max_bytes());
    DFTRACER_UTILS_LOG_DEBUG("Maximum bytes available: {}", max_bytes);
    return max_bytes;
}

std::size_t GzipReader::line_count() const {
    check_reader_state(is_open_, indexer_.get());
    std::size_t line_count = static_cast<std::size_t>(indexer_->line_count());
    DFTRACER_UTILS_LOG_DEBUG("Total lines available: {}", line_count);
    return line_count;
}

const dstd::String &GzipReader::archive_path() const { return gz_path_; }

const dstd::String &GzipReader::index_path() const { return index_path_; }

void GzipReader::set_buffer_size(std::size_t size) {
    default_buffer_size_ = size;
}

void GzipReader::reset() {
    check_reader_state(is_open_, indexer_.get());
    stream_cache_.clear();
}

coro::CoroTask<dstd::Result<std::size_t>> GzipReader::read_async(
    std::size_t start_bytes, std::size_t end_bytes, char *buffer,
    std::size_t buffer_size) {
    check_reader_state(is_open_, indexer_.get());
    auto valid = validate_parameters(buffer, buffer_size, start_bytes,
                                     end_bytes, indexer_.get()->max_bytes());
    if (!valid.has_value()) co_return valid.error();

    DFTRACER_UTILS_LOG_DEBUG(
        "GzipReader::read - request: start_bytes={}, end_bytes={}, "
        "buffer_size={}",
        start_bytes, end_bytes, buffer_size);

    // Check if we can reuse cached stream
    if (!stream_cache_.can_continue(StreamType::Bytes, gz_path_, start_bytes,
                                    end_bytes)) {
        DFTRACER_UTILS_LOG_DEBUG("{}",
                                 "GzipReader::read - creating new byte stream");
        auto new_stream = stream(StreamConfig()
                                     .stream_type(StreamType::Bytes)
                                     .range_type(RangeType::ByteRange)
                                     .from(start_bytes)
                                     .to(end_bytes));
        if (!new_stream.has_value()) co_return new_stream.error();
        stream_cache_.update(std::move(*new_stream), StreamType::Bytes,
                             gz_path_, start_bytes, end_bytes);
    } else {
        DFTRACER_UTILS_LOG_DEBUG(
            "{}", "GzipReader::read - reusing cached byte stream");
    }

    auto read = co_await stream_cache_.get()->read_async(buffer, buffer_size);
    if (!read.has_value()) co_return read.error();
    const std::size_t result = *read;
    DFTRACER_UTILS_LOG_DEBUG("GzipReader::read - returned {} bytes", result);

    // Update position for next potential read
    stream_cache_.update_position(start_bytes + result);

    co_return result;
}

coro::CoroTask<dstd::Result<std::size_t>> GzipReader::read_line_bytes_async(
    std::size_t start_bytes, std::size_t end_bytes, char *buffer,
    std::size_t buffer_size) {
    check_reader_state(is_open_, indexer_.get());

    if (end_bytes > indexer_.get()->max_bytes()) {
        end_bytes = indexer_.get()->max_bytes();
    }

    auto valid = validate_parameters(buffer, buffer_size, start_bytes,
                                     end_bytes, indexer_.get()->max_bytes());
    if (!valid.has_value()) co_return valid.error();

    // Check if we can reuse cached stream
    if (!stream_cache_.can_continue(StreamType::MultiLinesBytes, gz_path_,
                                    start_bytes, end_bytes)) {
        auto new_stream = stream(StreamConfig()
                                     .stream_type(StreamType::MultiLinesBytes)
                                     .range_type(RangeType::ByteRange)
                                     .from(start_bytes)
                                     .to(end_bytes));
        if (!new_stream.has_value()) co_return new_stream.error();
        stream_cache_.update(std::move(*new_stream),
                             StreamType::MultiLinesBytes, gz_path_, start_bytes,
                             end_bytes);
    }

    auto read = co_await stream_cache_.get()->read_async(buffer, buffer_size);
    if (!read.has_value()) co_return read.error();
    const std::size_t result = *read;

    // Update position for next potential read
    stream_cache_.update_position(start_bytes + result);

    co_return result;
}

coro::CoroTask<dstd::Result<dstd::String>> GzipReader::read_lines_async(
    std::size_t start_line, std::size_t end_line) {
    check_reader_state(is_open_, indexer_.get());

    auto valid =
        validate_line_range(start_line, end_line, indexer_.get()->line_count());
    if (!valid.has_value()) co_return valid.error();

    // Check if we can reuse cached stream
    if (!stream_cache_.can_continue(StreamType::MultiLines, gz_path_,
                                    start_line, end_line)) {
        auto new_stream = stream(StreamConfig()
                                     .stream_type(StreamType::MultiLines)
                                     .range_type(RangeType::LineRange)
                                     .from(start_line)
                                     .to(end_line));
        if (!new_stream.has_value()) co_return new_stream.error();
        stream_cache_.update(std::move(*new_stream), StreamType::MultiLines,
                             gz_path_, start_line, end_line);
    }

    dstd::String result;
    // Pre-allocate to avoid reallocations (like old StringLineProcessor)
    std::size_t estimated_lines = end_line - start_line + 1;
    result.reserve(estimated_lines * 100);  // Estimate ~100 bytes per line

    dstd::Vector<char> buffer(default_buffer_size_);

    while (!stream_cache_.get()->done()) {
        auto read = co_await stream_cache_.get()->read_async(buffer.data(),
                                                             buffer.size());
        if (!read.has_value()) co_return read.error();
        const std::size_t bytes_read = *read;
        if (bytes_read == 0) break;

        result.append(buffer.data(), bytes_read);
    }

    co_return result;
}

coro::CoroTask<dstd::Result<void>> GzipReader::read_lines_with_processor_async(
    std::size_t start_line, std::size_t end_line, LineProcessor &processor) {
    check_reader_state(is_open_, indexer_.get());

    auto valid =
        validate_line_range(start_line, end_line, indexer_.get()->line_count());
    if (!valid.has_value()) co_return valid.error();

    processor.begin(start_line, end_line);

    // Create a LineStream that returns one line at a time
    auto opened = stream(StreamConfig()
                             .stream_type(StreamType::Line)
                             .range_type(RangeType::LineRange)
                             .from(start_line)
                             .to(end_line));
    if (!opened.has_value()) co_return opened.error();
    auto line_stream = std::move(*opened);

    dstd::Vector<char> buffer(default_buffer_size_);

    while (!line_stream->done()) {
        auto read =
            co_await line_stream->read_async(buffer.data(), buffer.size());
        if (!read.has_value()) co_return read.error();
        const std::size_t bytes_read = *read;
        if (bytes_read == 0) break;

        // LineStream returns one complete line with \n
        // Processor expects line without \n
        std::size_t line_size = bytes_read;
        if (line_size > 0 && buffer[line_size - 1] == '\n') {
            line_size--;
        }

        const auto process_ok =
            co_await processor.process(buffer.data(), line_size);
        if (!process_ok) {
            processor.end();
            co_return dstd::Result<void>{};
        }
    }

    processor.end();
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>>
GzipReader::read_line_bytes_with_processor_async(std::size_t start_bytes,
                                                 std::size_t end_bytes,
                                                 LineProcessor &processor) {
    check_reader_state(is_open_, indexer_.get());

    if (end_bytes > indexer_.get()->max_bytes()) {
        end_bytes = indexer_.get()->max_bytes();
    }

    if (start_bytes >= end_bytes) {
        co_return dstd::Result<void>{};
    }

    processor.begin(start_bytes, end_bytes);

    auto opened = stream(StreamConfig()
                             .stream_type(StreamType::LineBytes)
                             .range_type(RangeType::ByteRange)
                             .from(start_bytes)
                             .to(end_bytes));
    if (!opened.has_value()) co_return opened.error();
    auto lines_stream = std::move(*opened);

    dstd::Vector<char> buffer(default_buffer_size_);

    while (!lines_stream->done()) {
        auto read =
            co_await lines_stream->read_async(buffer.data(), buffer.size());
        if (!read.has_value()) co_return read.error();
        const std::size_t bytes_read = *read;
        if (bytes_read == 0) break;
        co_await processor.process(buffer.data(), bytes_read);
    }

    processor.end();
    co_return dstd::Result<void>{};
}

bool GzipReader::is_valid() const { return is_open_ && indexer_.get(); }

dstd::String GzipReader::format_name() const { return "GZIP"; }

dstd::Result<dstd::UniquePtr<ReaderStream>> GzipReader::stream(
    const StreamConfig &config) {
    check_reader_state(is_open_, indexer_.get());

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

    if (range_type == RangeType::LineRange) {
        // Convert line numbers to byte offsets using the member table
        auto valid = validate_line_range(start, end, indexer_->line_count());
        if (!valid.has_value()) return valid.error();

        // Members carry the same line bookkeeping as the old recovery
        // points, and their starts are decodable without a dictionary.
        auto members = indexer_->members();

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
            end_bytes = indexer_->max_bytes();
            actual_start_line = 1;
            DFTRACER_UTILS_LOG_DEBUG(
                "No member covers lines {}-{}, using full file: end_bytes={}",
                start, end, end_bytes);
        } else {
            // Members are line-aligned (each written member is a whole-line
            // batch), so a member's uc_offset is the first byte of its
            // first_line_num. Begin decoding at the member containing `start`
            // and label the first decoded line as that member's first line;
            // LineStream then drops [first_line_num, start) and serves
            // [start, end]. Decoding from the containing member avoids
            // re-inflating earlier members.
            // A restart piece starts inside a line unless the byte before it is
            // a newline, and then owns its lines from the next one on (the
            // line it opens is `first_line_num`). It qualifies when the first
            // line it owns whole is at or before `start`. One with no window
            // does not.
            bool found_start = false;
            std::size_t at = members.size();
            while (at > 0 && !found_start) {
                --at;
                const auto &member = members[at];
                if (member.first_line_num > start) continue;
                std::size_t first_owned = member.first_line_num;
                if (member.kind ==
                    trace::index::gzip::GzipRecordKind::Restart) {
                    const dstd::String window =
                        indexer_->restart_window(member.member_idx);
                    if (window.empty()) continue;
                    if (window.back() != '\n') ++first_owned;
                    if (first_owned > start) continue;
                }
                start_bytes = member.uc_offset;
                actual_start_line = first_owned;
                found_start = true;
            }
            if (!found_start) {
                start_bytes = 0;
                actual_start_line = 1;
            }
            end_bytes = last_overlap_end;

            DFTRACER_UTILS_LOG_DEBUG(
                "Lines {}-{} -> byte range {}-{}, actual_start_line={}", start,
                end, start_bytes, end_bytes, actual_start_line);
        }
    }

    // Create appropriate stream type
    switch (stream_type) {
        case StreamType::Bytes: {
            auto byte_stream = dstd::make_unique<GzipByteStream>(buffer_size);
            byte_stream->initialize(gz_path_, start_bytes, end_bytes,
                                    *indexer_);
            return dstd::UniquePtr<ReaderStream>(std::move(byte_stream));
        }
        case StreamType::LineBytes: {
            // Single line-aligned bytes at a time
            auto line_byte_stream =
                dstd::make_unique<GzipLineByteStream>(buffer_size);
            line_byte_stream->set_extend_to_line_boundary(
                extend_to_line_boundary);
            line_byte_stream->set_open_at_earlier_piece(
                range_type == RangeType::ByteRange && !extend_to_line_boundary);
            line_byte_stream->initialize(gz_path_, start_bytes, end_bytes,
                                         *indexer_);

            // Wrap with LineStream to return one line-aligned chunk at a time
            if (range_type == RangeType::LineRange) {
                return dstd::UniquePtr<ReaderStream>(
                    dstd::make_unique<LineStream>(std::move(line_byte_stream),
                                                  start, end,
                                                  actual_start_line));
            } else {
                return dstd::UniquePtr<ReaderStream>(
                    dstd::make_unique<LineStream>(std::move(line_byte_stream)));
            }
        }
        case StreamType::MultiLinesBytes: {
            // Multiple line-aligned bytes per read
            auto line_byte_stream =
                dstd::make_unique<GzipLineByteStream>(buffer_size);
            line_byte_stream->set_extend_to_line_boundary(
                extend_to_line_boundary);
            line_byte_stream->set_open_at_earlier_piece(
                range_type == RangeType::ByteRange && !extend_to_line_boundary);
            line_byte_stream->initialize(gz_path_, start_bytes, end_bytes,
                                         *indexer_);
            return dstd::UniquePtr<ReaderStream>(std::move(line_byte_stream));
        }
        case StreamType::Line: {
            // Single parsed line per read
            auto line_byte_stream =
                dstd::make_unique<GzipLineByteStream>(buffer_size);
            line_byte_stream->set_extend_to_line_boundary(
                extend_to_line_boundary);
            line_byte_stream->set_open_at_earlier_piece(
                range_type == RangeType::ByteRange && !extend_to_line_boundary);
            line_byte_stream->initialize(gz_path_, start_bytes, end_bytes,
                                         *indexer_);

            if (range_type == RangeType::LineRange) {
                return dstd::UniquePtr<ReaderStream>(
                    dstd::make_unique<LineStream>(std::move(line_byte_stream),
                                                  start, end,
                                                  actual_start_line));
            } else {
                return dstd::UniquePtr<ReaderStream>(
                    dstd::make_unique<LineStream>(std::move(line_byte_stream)));
            }
        }
        case StreamType::MultiLines: {
            // Multiple parsed lines per read
            auto line_byte_stream =
                dstd::make_unique<GzipLineByteStream>(buffer_size);
            line_byte_stream->set_extend_to_line_boundary(
                extend_to_line_boundary);
            line_byte_stream->set_open_at_earlier_piece(
                range_type == RangeType::ByteRange && !extend_to_line_boundary);
            line_byte_stream->initialize(gz_path_, start_bytes, end_bytes,
                                         *indexer_);

            if (range_type == RangeType::LineRange) {
                return dstd::UniquePtr<ReaderStream>(
                    dstd::make_unique<MultiLineStream>(
                        std::move(line_byte_stream), start, end,
                        actual_start_line));
            } else {
                return dstd::UniquePtr<ReaderStream>(
                    dstd::make_unique<MultiLineStream>(
                        std::move(line_byte_stream)));
            }
        }
        default:
            dstd::fatal("GzipReader::stream: unknown stream type");
    }
}

}  // namespace dftracer::utils::trace::reader::internal
