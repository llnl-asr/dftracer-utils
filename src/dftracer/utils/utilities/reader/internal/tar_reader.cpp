#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/indexer/internal/tar/queries/queries.h>
#include <dftracer/utils/utilities/reader/internal/streams/tar_byte_stream.h>
#include <dftracer/utils/utilities/reader/internal/string_line_processor.h>
#include <dftracer/utils/utilities/reader/internal/tar_reader.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

using namespace dftracer::utils::utilities::indexer::internal::tar;

namespace dftracer::utils::utilities::reader::internal {

TarReader::TarReader(const std::string &tar_gz_path_,
                     const std::string &idx_path_, std::size_t index_ckpt_size)
    : tar_gz_path(tar_gz_path_),
      idx_path(idx_path_),
      is_open(false),
      default_buffer_size(DEFAULT_TAR_READER_BUFFER_SIZE),
      logical_mapping_cached(false),
      cached_total_logical_bytes(0),
      cached_total_logical_lines(0) {
    try {
        printf("Creating TAR reader for gz: %s and index: %s\n",
               tar_gz_path.c_str(), idx_path.c_str());
        indexer = std::make_shared<TarIndexer>(tar_gz_path, idx_path,
                                               index_ckpt_size, false);
        is_open = true;

        DFTRACER_UTILS_LOG_DEBUG(
            "Successfully created TAR reader for gz: %s and index: %s",
            tar_gz_path.c_str(), idx_path.c_str());
    } catch (const std::exception &e) {
        throw std::runtime_error(
            "Failed to initialize TAR reader with indexer: " +
            std::string(e.what()));
    }
}

TarReader::TarReader(std::shared_ptr<TarIndexer> indexer_)
    : default_buffer_size(DEFAULT_TAR_READER_BUFFER_SIZE),
      indexer(indexer_),
      logical_mapping_cached(false),
      cached_total_logical_bytes(0),
      cached_total_logical_lines(0) {
    if (indexer == nullptr) {
        throw std::runtime_error("Invalid indexer provided");
    }
    is_open = true;
    tar_gz_path = indexer->get_tar_gz_path();
    idx_path = indexer->get_idx_path();
}

TarReader::~TarReader() = default;

TarReader::TarReader(TarReader &&other) noexcept
    : tar_gz_path(std::move(other.tar_gz_path)),
      idx_path(std::move(other.idx_path)),
      is_open(other.is_open),
      default_buffer_size(other.default_buffer_size),
      indexer(std::move(other.indexer)),
      logical_mapping_cached(other.logical_mapping_cached),
      cached_file_mapping(std::move(other.cached_file_mapping)),
      cached_total_logical_bytes(other.cached_total_logical_bytes),
      cached_total_logical_lines(other.cached_total_logical_lines) {
    other.is_open = false;
    other.logical_mapping_cached = false;
}

TarReader &TarReader::operator=(TarReader &&other) noexcept {
    if (this != &other) {
        tar_gz_path = std::move(other.tar_gz_path);
        idx_path = std::move(other.idx_path);
        is_open = other.is_open;
        default_buffer_size = other.default_buffer_size;
        indexer = std::move(other.indexer);
        logical_mapping_cached = other.logical_mapping_cached;
        cached_file_mapping = std::move(other.cached_file_mapping);
        cached_total_logical_bytes = other.cached_total_logical_bytes;
        cached_total_logical_lines = other.cached_total_logical_lines;

        other.is_open = false;
        other.logical_mapping_cached = false;
    }
    return *this;
}

// Metadata operations
std::size_t TarReader::get_max_bytes() const {
    build_logical_mapping();
    return cached_total_logical_bytes;
}

std::size_t TarReader::get_num_lines() const {
    build_logical_mapping();
    return cached_total_logical_lines;
}

std::string TarReader::get_format_name() const { return "TAR.GZ"; }

const std::string &TarReader::get_archive_path() const { return tar_gz_path; }

const std::string &TarReader::get_idx_path() const { return idx_path; }

void TarReader::set_buffer_size(std::size_t size) {
    default_buffer_size = size;
}

std::unique_ptr<ReaderStream> TarReader::stream(
    [[maybe_unused]] const StreamConfig &config) {
    // TODO: Implement TAR stream creation
    throw std::runtime_error("TarReader::stream() not yet implemented");
}

// Archive structure operations
std::vector<TarReader::TarFileInfo> TarReader::list_files() const {
    build_logical_mapping();
    return cached_file_mapping;
}

// Reader interface - operates on concatenated logical view
std::size_t TarReader::read(std::size_t start_bytes, std::size_t end_bytes,
                            char *buffer, std::size_t buffer_size) {
    return read_logical(start_bytes, end_bytes, buffer, buffer_size);
}

std::size_t TarReader::read_line_bytes(std::size_t start_bytes,
                                       std::size_t end_bytes, char *buffer,
                                       std::size_t buffer_size) {
    build_logical_mapping();

    if (start_bytes >= cached_total_logical_bytes || buffer_size == 0) {
        return 0;
    }

    std::size_t actual_end = std::min(
        end_bytes, static_cast<std::size_t>(cached_total_logical_bytes));

    // Read a larger chunk to ensure we get complete lines
    std::size_t read_size = std::min(actual_end - start_bytes, buffer_size * 2);
    std::vector<char> temp_buffer(read_size);

    std::size_t bytes_read = read_logical(start_bytes, start_bytes + read_size,
                                          temp_buffer.data(), read_size);

    if (bytes_read == 0) {
        return 0;
    }

    // Find the first line start (if start_bytes is not at line beginning)
    std::size_t line_start = 0;
    if (start_bytes > 0) {
        // Look back to find the beginning of the current line
        std::size_t lookback_start =
            (start_bytes >= 512) ? start_bytes - 512 : 0;
        std::vector<char> lookback_buffer(512);
        std::size_t lookback_read = read_logical(lookback_start, start_bytes,
                                                 lookback_buffer.data(), 512);

        if (lookback_read > 0) {
            // Find the last newline in the lookback buffer
            for (std::size_t i = lookback_read; i > 0; --i) {
                if (lookback_buffer[i - 1] == '\n') {
                    // The line starts after this newline, but we need to adjust
                    // for the position difference
                    break;
                }
            }
        }
    }

    // Find complete lines within our read data
    std::size_t output_pos = 0;
    std::size_t search_pos = line_start;

    while (search_pos < bytes_read && output_pos < buffer_size) {
        // Find the next newline
        std::size_t newline_pos = search_pos;
        while (newline_pos < bytes_read && temp_buffer[newline_pos] != '\n') {
            newline_pos++;
        }

        if (newline_pos < bytes_read) {
            // Include the newline character
            newline_pos++;
            std::size_t line_len = newline_pos - search_pos;

            if (output_pos + line_len <= buffer_size) {
                std::memcpy(buffer + output_pos,
                            temp_buffer.data() + search_pos, line_len);
                output_pos += line_len;
                search_pos = newline_pos;
            } else {
                break;  // Buffer full
            }
        } else {
            // Incomplete line at the end, don't include it
            break;
        }
    }

    return output_pos;
}

std::string TarReader::read_lines(std::size_t start_line,
                                  std::size_t end_line) {
    build_logical_mapping();

    if (start_line < 1 || start_line > cached_total_logical_lines) {
        return "";
    }

    std::size_t actual_end_line = std::min(
        end_line, static_cast<std::size_t>(cached_total_logical_lines));

    // Find which files contain the requested lines
    std::string result;
    std::size_t current_global_line = 1;

    for (const auto &file_info : cached_file_mapping) {
        // Check if this file contains any of our target lines
        if (current_global_line + file_info.estimated_lines <= start_line) {
            current_global_line += file_info.estimated_lines;
            continue;
        }
        if (current_global_line > actual_end_line) {
            break;
        }

        // Read content from this file
        std::size_t file_start_line =
            std::max(start_line, current_global_line) - current_global_line + 1;
        std::size_t file_end_line =
            std::min(actual_end_line,
                     static_cast<std::size_t>(current_global_line +
                                              file_info.estimated_lines - 1)) -
            current_global_line + 1;

        std::string file_content =
            read_file_content(file_info, 0, file_info.file_size);
        result += extract_lines_from_content(file_content, file_start_line,
                                             file_end_line);

        current_global_line += file_info.estimated_lines;
    }

    return result;
}

void TarReader::read_lines_with_processor(std::size_t start_line,
                                          std::size_t end_line,
                                          LineProcessor &processor) {
    build_logical_mapping();

    if (start_line < 1 || start_line > cached_total_logical_lines) {
        return;
    }

    std::size_t actual_end_line = std::min(
        end_line, static_cast<std::size_t>(cached_total_logical_lines));

    // Find which files contain the requested lines
    std::size_t current_global_line = 1;

    processor.begin(start_line, actual_end_line);

    for (const auto &file_info : cached_file_mapping) {
        // Check if this file contains any of our target lines
        if (current_global_line + file_info.estimated_lines <= start_line) {
            current_global_line += file_info.estimated_lines;
            continue;
        }
        if (current_global_line > actual_end_line) {
            break;
        }

        // Read content from this file
        std::string file_content =
            read_file_content(file_info, 0, file_info.file_size);
        process_content_lines(file_content, processor, current_global_line,
                              start_line, actual_end_line);

        current_global_line += file_info.estimated_lines;
    }

    processor.end();
}

void TarReader::read_line_bytes_with_processor(std::size_t start_bytes,
                                               std::size_t end_bytes,
                                               LineProcessor &processor) {
    build_logical_mapping();

    if (start_bytes >= cached_total_logical_bytes) {
        return;
    }

    std::size_t actual_end = std::min(
        end_bytes, static_cast<std::size_t>(cached_total_logical_bytes));

    processor.begin(start_bytes, actual_end);

    // Find which files contain the requested byte range
    for (const auto &file_info : cached_file_mapping) {
        if (file_info.logical_end_offset <= start_bytes) {
            continue;  // This file is before our range
        }
        if (file_info.logical_start_offset >= actual_end) {
            break;     // We've passed our range
        }

        // Calculate the portion of this file we need
        std::size_t file_start =
            (start_bytes > file_info.logical_start_offset)
                ? start_bytes - file_info.logical_start_offset
                : 0;
        std::size_t file_end = std::min(
            file_info.file_size, actual_end - file_info.logical_start_offset);

        if (file_start < file_end) {
            std::string file_content =
                read_file_content(file_info, file_start, file_end - file_start);
            processor.process(file_content.c_str(), file_content.length());
        }
    }

    processor.end();
}

void TarReader::reset() {
    logical_mapping_cached = false;
    cached_file_mapping.clear();
    cached_total_logical_bytes = 0;
    cached_total_logical_lines = 0;
}

bool TarReader::is_valid() const { return is_open && indexer; }

// File-level operations - NOT YET IMPLEMENTED
std::string TarReader::read_file(const std::string &filename,
                                 std::size_t start_line, std::size_t end_line) {
    (void)filename;
    (void)start_line;
    (void)end_line;  // Suppress unused warnings
    return "";       // Placeholder
}

bool TarReader::find_file(const std::string &filename,
                          TarFileInfo &file_info) const {
    build_logical_mapping();

    auto it =
        std::find_if(cached_file_mapping.begin(), cached_file_mapping.end(),
                     [&filename](const TarFileInfo &info) {
                         return info.file_name == filename;
                     });

    if (it != cached_file_mapping.end()) {
        file_info = *it;
        return true;
    }

    return false;
}

// Internal helper methods implementation moved from TarReaderImplementor
void TarReader::build_logical_mapping() const {
    if (logical_mapping_cached) {
        return;
    }

    DFTRACER_UTILS_LOG_DEBUG(
        "%s", "Building logical content mapping for TAR.GZ file");

    try {
        // Get all TAR file entries from the indexer
        auto tar_files = indexer->list_files();

        cached_file_mapping.clear();
        cached_file_mapping.reserve(tar_files.size());

        std::uint64_t logical_offset = 0;
        std::uint64_t logical_line = 1;

        for (const auto &tar_file : tar_files) {
            TarFileInfo file_info;
            file_info.file_name = tar_file.file_name;
            file_info.file_size = tar_file.file_size;
            file_info.file_mtime = tar_file.file_mtime;
            file_info.typeflag = tar_file.typeflag;
            file_info.logical_start_offset = logical_offset;
            file_info.logical_start_line = logical_line;

            // Calculate end positions
            file_info.logical_end_offset = logical_offset + tar_file.file_size;

            // Estimate lines in this file (rough approximation)
            file_info.estimated_lines =
                tar_file.file_size > 0 ? (tar_file.file_size / 50) + 1 : 1;
            file_info.logical_end_line =
                logical_line + file_info.estimated_lines - 1;

            cached_file_mapping.push_back(file_info);

            logical_offset = file_info.logical_end_offset;
            logical_line = file_info.logical_end_line + 1;
        }

        cached_total_logical_bytes = logical_offset;
        cached_total_logical_lines = logical_line - 1;
        logical_mapping_cached = true;

        DFTRACER_UTILS_LOG_DEBUG(
            "Built logical mapping: %zu files, %zu bytes, %zu lines",
            cached_file_mapping.size(), cached_total_logical_bytes,
            cached_total_logical_lines);
    } catch (const std::exception &e) {
        throw std::runtime_error("Failed to build TAR logical mapping: " +
                                 std::string(e.what()));
    }
}

std::size_t TarReader::read_logical(std::size_t start_bytes,
                                    std::size_t end_bytes, char *buffer,
                                    std::size_t buffer_size) const {
    build_logical_mapping();

    if (start_bytes >= cached_total_logical_bytes || buffer_size == 0) {
        return 0;
    }

    std::size_t actual_end = std::min(
        end_bytes, static_cast<std::size_t>(cached_total_logical_bytes));
    [[maybe_unused]] std::size_t total_to_read = actual_end - start_bytes;
    [[maybe_unused]] std::size_t total_written = 0;

    DFTRACER_UTILS_LOG_DEBUG(
        "TAR logical read: start=%zu, end=%zu, buffer_size=%zu", start_bytes,
        actual_end, buffer_size);

    // Find which files contain the requested byte range
    std::size_t bytes_written = 0;

    for (const auto &file_info : cached_file_mapping) {
        if (file_info.logical_end_offset <= start_bytes) {
            continue;  // This file is before our range
        }
        if (file_info.logical_start_offset >= actual_end) {
            break;     // We've passed our range
        }
        if (bytes_written >= buffer_size) {
            break;     // Buffer is full
        }

        // Calculate the portion of this file we need
        std::size_t file_start =
            (start_bytes > file_info.logical_start_offset)
                ? start_bytes - file_info.logical_start_offset
                : 0;
        std::size_t file_end = std::min(
            file_info.file_size, actual_end - file_info.logical_start_offset);

        if (file_start < file_end) {
            std::size_t bytes_to_read =
                std::min(file_end - file_start, buffer_size - bytes_written);
            std::string file_content =
                read_file_content(file_info, file_start, bytes_to_read);

            std::memcpy(buffer + bytes_written, file_content.c_str(),
                        file_content.length());
            bytes_written += file_content.length();
        }
    }

    DFTRACER_UTILS_LOG_DEBUG("TAR logical read: returning %zu bytes",
                             bytes_written);
    return bytes_written;
}

// Helper method implementations
std::string TarReader::read_file_content(const TarFileInfo &file_info,
                                         std::size_t offset,
                                         std::size_t size) const {
    if (size == 0) {
        return "";
    }

    try {
        // Calculate the absolute offset in the logical stream where the file
        // data starts
        std::size_t file_data_start = file_info.logical_start_offset + offset;
        std::size_t file_data_end = file_data_start + size;

        // Ensure we don't read beyond the file boundaries
        std::size_t max_end =
            file_info.logical_start_offset + file_info.file_size;
        if (file_data_end > max_end) {
            file_data_end = max_end;
        }

        if (file_data_start >= file_data_end) {
            return "";
        }

        // Use a buffer to read the data
        std::size_t actual_size = file_data_end - file_data_start;
        std::vector<char> buffer(actual_size);

        // Create a TAR byte stream to read file content
        auto byte_stream = std::make_unique<TarByteStream>();
        byte_stream->initialize(tar_gz_path, file_data_start, file_data_end,
                                *indexer);

        std::size_t total_read = 0;
        while (total_read < actual_size) {
            std::size_t chunk_size = std::min(actual_size - total_read,
                                              static_cast<std::size_t>(8192));
            std::size_t bytes_read =
                byte_stream->read(buffer.data() + total_read, chunk_size);

            if (bytes_read == 0) {
                break;  // EOF or error
            }

            total_read += bytes_read;
        }

        return std::string(buffer.data(), total_read);

    } catch (const std::exception &e) {
        DFTRACER_UTILS_LOG_DEBUG("Error reading TAR file content: %s",
                                 e.what());
        return "";
    }
}

std::string TarReader::extract_lines_from_content(const std::string &content,
                                                  std::size_t start_line,
                                                  std::size_t end_line) const {
    if (content.empty()) {
        return "";
    }

    std::istringstream stream(content);
    std::string line;
    std::string result;
    std::size_t current_line = 1;

    while (std::getline(stream, line) && current_line <= end_line) {
        if (current_line >= start_line) {
            result += line + "\n";
        }
        current_line++;
    }

    return result;
}

void TarReader::process_content_lines(const std::string &content,
                                      LineProcessor &processor,
                                      std::size_t base_line,
                                      std::size_t start_line,
                                      std::size_t end_line) const {
    if (content.empty()) {
        return;
    }

    std::istringstream stream(content);
    std::string line;
    std::size_t current_line = base_line;

    while (std::getline(stream, line) && current_line <= end_line) {
        if (current_line >= start_line) {
            if (!processor.process(line.c_str(), line.length())) {
                break;
            }
        }
        current_line++;
    }
}

}  // namespace dftracer::utils::utilities::reader::internal
