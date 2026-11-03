// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_REPLAY_TRACE_H
#define DFTRACER_UTILS_UTILITIES_REPLAY_TRACE_H

#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::utilities::replay {

/**
 * Enumeration of trace event types
 */
enum class TraceType {
    Regular,          ///< Normal function call trace
    FileHash,         ///< File hash metadata (FH)
    HostHash,         ///< Host hash metadata (HH)
    StringHash,       ///< String hash metadata
    ProcessMetadata,  ///< Process metadata
    OtherMetadata     ///< Other metadata types
};

/**
 * Type aliases for trace fields
 */
using BinFields = dstd::HashMap<dstd::String, std::int32_t>;
using ViewFields = dstd::HashMap<dstd::String, dstd::String>;

/**
 * Trace structure representing a single trace event from DFTracer
 * Contains all information needed for replay operations
 */
struct Trace {
    /// Category and function identification.
    /// Short-lived enum-like strings (cat/func_name) and per-event hashes
    /// (fhash/hhash) are non-owning views into a program-lifetime
    /// @ref dstd::StringIntern pool.
    dstd::StringView cat;        ///< Category (e.g., "posix", "stdio", "h5py")
    dstd::String io_cat;         ///< I/O category (read, write, metadata)
    dstd::String acc_pat;        ///< Access pattern
    dstd::StringView func_name;  ///< Function name (e.g., "read", "write")

    /// Timing information
    double duration;           ///< Duration in microseconds
    std::uint64_t count;       ///< Operation count
    std::uint64_t time_range;  ///< Time range
    std::uint64_t time_start;  ///< Start timestamp in microseconds
    std::uint64_t time_end;    ///< End timestamp in microseconds
    std::uint64_t epoch;       ///< Epoch time

    /// Process/thread identification
    std::uint64_t pid;  ///< Process ID
    std::uint64_t tid;  ///< Thread ID

    /// File identification
    dstd::StringView fhash;  ///< File hash (interned)
    dstd::StringView hhash;  ///< Host hash (interned)
    std::uint64_t image_id;  ///< Image ID

    /// Trace type
    TraceType type;

    /// Extended fields for flexible trace data
    ViewFields view_fields;
    BinFields bin_fields;

    /// I/O operation parameters
    std::int64_t size = -1;    ///< Operation size (-1 means unknown)
    std::int64_t offset = -1;  ///< File offset (-1 means unknown)

    /// Validation flag
    bool is_valid = false;  ///< Set after successful parsing

    /**
     * Default constructor
     */
    Trace()
        : duration(0.0),
          count(0),
          time_range(0),
          time_start(0),
          time_end(0),
          epoch(0),
          pid(0),
          tid(0),
          image_id(0),
          type(TraceType::Regular),
          size(-1),
          offset(-1),
          is_valid(false) {}

    /**
     * Check if this is a metadata trace
     */
    bool is_metadata() const { return type != TraceType::Regular; }

    /**
     * Get end time, calculating from start + duration if needed
     */
    std::uint64_t get_end_time() const {
        if (time_end > 0) {
            return time_end;
        }
        return time_start + static_cast<std::uint64_t>(duration);
    }

    /**
     * Check if this trace has valid timing information
     */
    bool has_valid_timing() const {
        return time_start > 0 && (duration > 0 || time_end > time_start);
    }

    /**
     * Check if this trace has I/O size information
     */
    bool has_size() const { return size >= 0; }

    /**
     * Check if this trace has offset information
     */
    bool has_offset() const { return offset >= 0; }
};

}  // namespace dftracer::utils::utilities::replay

#endif  // DFTRACER_UTILS_UTILITIES_REPLAY_TRACE_H
