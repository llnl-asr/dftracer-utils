// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/replay/replay.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace dftracer::utils::utilities::replay {

namespace {

/**
 * Create directory path if it doesn't exist
 */
bool ensure_directory_exists(dstd::StringView path) {
    dstd::String dir_path(path.substr(0, path.rfind('/')));
    if (dir_path.empty() || dir_path == path) return true;

    return dstd::fs::create_directories(dstd::PathView(dir_path)).has_value();
}

}  // anonymous namespace

// =============================================================================
// PosixExecutor Implementation
// =============================================================================

bool PosixExecutor::execute(const Trace& trace, const ReplayConfig& config) {
    dstd::StringView func_name = trace.func_name;

    if (config.dry_run) {
        DFTRACER_UTILS_LOG_DEBUG("DRY RUN: Would execute POSIX %.*s",
                                 static_cast<int>(func_name.size()),
                                 func_name.data());
        return true;
    }

    if (func_name == "open" || func_name == "open64" || func_name == "openat") {
        return execute_open(trace, config);
    } else if (func_name == "close") {
        return execute_close(trace, config);
    } else if (func_name == "read" || func_name == "pread" ||
               func_name == "pread64") {
        return execute_read(trace, config);
    } else if (func_name == "write" || func_name == "pwrite" ||
               func_name == "pwrite64") {
        return execute_write(trace, config);
    } else if (func_name == "lseek" || func_name == "lseek64") {
        return execute_seek(trace, config);
    } else if (func_name == "stat" || func_name == "stat64" ||
               func_name == "lstat" || func_name == "fstat") {
        return execute_stat(trace, config);
    }

    DFTRACER_UTILS_LOG_DEBUG("Unsupported POSIX function: %.*s",
                             static_cast<int>(func_name.size()),
                             func_name.data());
    return false;
}

bool PosixExecutor::can_handle(const Trace& trace) const {
    return trace.cat == "posix" || trace.cat == "POSIX";
}

bool PosixExecutor::execute_open(const Trace& trace,
                                 const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX open");

    if (!trace.fhash.empty()) {
        dstd::String file_path;
        if (config.output_directory.empty()) {
            file_path.reserve(12 + trace.fhash.size());
            file_path = "replay_file_";
        } else {
            file_path.reserve(config.output_directory.size() + 13 +
                              trace.fhash.size());
            file_path = config.output_directory;
            file_path += "/replay_file_";
        }
        file_path.append(trace.fhash.data(), trace.fhash.size());

        ensure_directory_exists(file_path);

        auto opened = dstd::File::open(dstd::PathView(file_path),
                                       {.access = dstd::Access::ReadWrite,
                                        .creation = dstd::Creation::IfNeeded});
        if (opened) {
            DFTRACER_UTILS_LOG_DEBUG("Opened file %s with fd %d",
                                     file_path.c_str(),
                                     opened->native_handle());
            open_files_[trace.fhash] = OpenFile{std::move(*opened), 0};
            return true;
        } else {
            DFTRACER_UTILS_LOG_ERROR("Failed to open file %s: %s",
                                     file_path.c_str(),
                                     opened.error().format().c_str());
            return false;
        }
    }

    return true;
}

bool PosixExecutor::execute_close(const Trace& trace,
                                  [[maybe_unused]] const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX close");

    auto it = open_files_.find(trace.fhash);
    if (it != open_files_.end()) {
        open_files_.erase(it);
        DFTRACER_UTILS_LOG_DEBUG("Closed file with hash %.*s",
                                 static_cast<int>(trace.fhash.size()),
                                 trace.fhash.data());
    }

    return true;
}

void PosixExecutor::ensure_io_buffer(std::size_t size) {
    if (io_buffer_.size() < size) {
        io_buffer_.resize(size, 'A');
    }
}

bool PosixExecutor::execute_read(const Trace& trace,
                                 const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX read (size: %lld)",
                             static_cast<long long>(trace.size));

    auto it = open_files_.find(trace.fhash);
    if (it != open_files_.end() && trace.size > 0) {
        std::size_t n = std::min(static_cast<std::size_t>(trace.size),
                                 config.max_file_size);
        ensure_io_buffer(n);
        OpenFile& open = it->second;
        const auto bytes_read = open.file.read_at(
            open.position,
            dstd::Span<std::uint8_t>(
                reinterpret_cast<std::uint8_t*>(io_buffer_.data()), n));
        if (bytes_read) open.position += *bytes_read;
        DFTRACER_UTILS_LOG_DEBUG(
            "Read %zd bytes",
            bytes_read ? static_cast<ssize_t>(*bytes_read) : ssize_t{-1});
    }

    return true;
}

bool PosixExecutor::execute_write(const Trace& trace,
                                  const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX write (size: %lld)",
                             static_cast<long long>(trace.size));

    auto it = open_files_.find(trace.fhash);
    if (it != open_files_.end() && trace.size > 0) {
        std::size_t write_size = std::min(static_cast<std::size_t>(trace.size),
                                          config.max_file_size);
        ensure_io_buffer(write_size);
        OpenFile& open = it->second;
        const auto bytes_written = open.file.write_at(
            open.position, dstd::ByteView(io_buffer_.data(), write_size));
        if (bytes_written) open.position += *bytes_written;
        DFTRACER_UTILS_LOG_DEBUG(
            "Wrote %zd bytes",
            bytes_written ? static_cast<ssize_t>(*bytes_written) : ssize_t{-1});
    }

    return true;
}

bool PosixExecutor::execute_seek(const Trace& trace,
                                 [[maybe_unused]] const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX seek (offset: %lld)",
                             static_cast<long long>(trace.offset));

    auto it = open_files_.find(trace.fhash);
    if (it != open_files_.end() && trace.offset >= 0) {
        it->second.position = static_cast<std::uint64_t>(trace.offset);
        DFTRACER_UTILS_LOG_DEBUG("Seek to offset %lld",
                                 static_cast<long long>(trace.offset));
    }

    return true;
}

bool PosixExecutor::execute_stat([[maybe_unused]] const Trace& trace,
                                 [[maybe_unused]] const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX stat");

    if (!trace.fhash.empty()) {
        DFTRACER_UTILS_LOG_DEBUG("Would stat file with hash %.*s",
                                 static_cast<int>(trace.fhash.size()),
                                 trace.fhash.data());
    }

    return true;
}

// =============================================================================
// DFTracerExecutor Implementation
// =============================================================================

bool DFTracerExecutor::execute(const Trace& trace, const ReplayConfig& config) {
    if (config.dry_run) {
        return true;
    }

    // Sleep for the duration of the operation instead of doing actual I/O
    double duration_us = static_cast<double>(trace.time_end - trace.time_start);

    // Cap individual operation sleeps to 1ms for practical testing
    const double MAX_DFTRACER_SLEEP_US = 1.0 * 1000.0;
    if (duration_us > MAX_DFTRACER_SLEEP_US) {
        duration_us = MAX_DFTRACER_SLEEP_US;
    }

    if (config.no_sleep) {
        if (duration_us >= 100000.0) {
            DFTRACER_UTILS_LOG_DEBUG(
                "DFTracer would sleep for %.3f ms for %.*s (skipped)",
                duration_us / 1000.0, static_cast<int>(trace.func_name.size()),
                trace.func_name.data());
        }
    } else {
        if (duration_us >= 100.0) {
            DFTRACER_UTILS_LOG_DEBUG("DFTracer sleeping for %.3f ms for %.*s",
                                     duration_us / 1000.0,
                                     static_cast<int>(trace.func_name.size()),
                                     trace.func_name.data());
        }
        sleep_for_duration(duration_us);
    }

    return true;
}

bool DFTracerExecutor::can_handle([[maybe_unused]] const Trace& trace) const {
    return true;  // Handle all trace events
}

void DFTracerExecutor::sleep_for_duration(double duration_microseconds) {
    if (duration_microseconds <= 0) return;

    const double MAX_SLEEP_US = 10.0 * 1000.0 * 1000.0;
    if (duration_microseconds > MAX_SLEEP_US) {
        duration_microseconds = MAX_SLEEP_US;
    }

    auto sleep_duration = std::chrono::nanoseconds(
        static_cast<std::int64_t>(duration_microseconds * 1000));
    std::this_thread::sleep_for(sleep_duration);
}

}  // namespace dftracer::utils::utilities::replay
