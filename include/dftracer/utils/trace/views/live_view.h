// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_LIVE_VIEW_H
#define DFTRACER_UTILS_TRACE_VIEWS_LIVE_VIEW_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstdint>

namespace dftracer::utils::trace::views {

/// Why a live file has bytes that its last @ref LiveView::snapshot left out.
enum class LiveFileState : std::uint8_t {
    /// Every byte is in a whole gzip member.
    Whole,
    /// The file ends in a member that is short or bad.
    Tail,
    /// Bytes that are not a member come before a whole member.
    Hole,
};

/// One file as the last @ref LiveView::snapshot found it.
struct LiveFileStatus {
    dstd::String path;
    LiveFileState state = LiveFileState::Whole;
    /// Bytes of whole members with a checked CRC, from the start of the file.
    std::uint64_t verified_bytes = 0;
    /// Bytes after `verified_bytes` that the snapshot left out.
    std::uint64_t skipped_bytes = 0;
    /// With @ref LiveFileState::Hole, the bytes from `verified_bytes` to the
    /// next whole member.
    std::uint64_t hole_bytes = 0;
};

/// Tracks gzip traces that are still being written. A live `.pfw.gz` is a
/// sequence of whole gzip members appended to one file. At any instant it can
/// end inside a member and, on a parallel file system, can hold a hole where a
/// member has not landed behind a later one.
///
/// `paths` are files and directories. Each @ref snapshot scans a directory
/// again, so new files join. `index_path` is as for View. The trace files are
/// not copied or changed. Plain uncompressed traces have no member boundaries
/// and are not read. Not thread safe.
class LiveView {
   public:
    explicit LiveView(dstd::Vector<dstd::String> paths,
                      dstd::String index_path = "");

    /// Adds the members written since the last call to the index, once each,
    /// and returns a View of every whole member. Each file stops at its first
    /// short or bad member. The View does not change after this call and stays
    /// valid after later calls. Throws when a named file does not exist.
    View snapshot();

    /// One entry per file, from the last @ref snapshot.
    const dstd::Vector<LiveFileStatus>& status() const { return status_; }

   private:
    dstd::Vector<dstd::String> paths_;
    dstd::String index_path_;
    dstd::Vector<LiveFileStatus> status_;
};

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_LIVE_VIEW_H
