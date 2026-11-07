// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_LIVE_VIEW_H
#define DFTRACER_UTILS_TRACE_VIEWS_LIVE_VIEW_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstdint>

namespace dftracer::utils::trace::views {

/// Zero or unreadable bytes of a live file with a whole gzip member after them.
struct LiveHole {
    dstd::String path;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

/// Reads gzip traces that are still being written. A live `.pfw.gz` is a
/// sequence of whole gzip members appended to one file; at any instant it can
/// end inside a member and, on a parallel filesystem, can hold a hole where an
/// earlier member has not landed behind a later one. A View is built for
/// finished files, so this class gives it a snapshot of what is verifiably
/// whole: each file is walked member by member from the last verified boundary
/// with every CRC checked, the walk stops at the first unfinished tail or hole,
/// and only the bytes before that are appended to a private snapshot file. A
/// transient condition is never persisted, so a later refresh() that finds the
/// tail finished or the hole filled returns those rows.
///
/// `paths` are files and directories; a directory is scanned recursively for
/// gzip trace files on every refresh(), so files that appear later (spill
/// files) join. `index_path` is as for View, over the snapshots. `snapshot_dir`
/// holds the snapshots and must have room for a copy of the traces, and only
/// one LiveView at a time may use it; when empty, a private directory under the
/// system temporary directory is used and removed at process exit, since views
/// from refresh() read the snapshots lazily and can outlive this object.
/// Plain uncompressed traces have no member boundaries and are not read.
/// Not thread safe.
class LiveView {
   public:
    explicit LiveView(dstd::Vector<dstd::String> paths,
                      dstd::String index_path = "",
                      dstd::String snapshot_dir = "");
    ~LiveView();
    LiveView(const LiveView&) = delete;
    LiveView& operator=(const LiveView&) = delete;

    /// Walks every file again and returns a View of what is whole now. Throws
    /// when a named file does not exist.
    View refresh();

    /// Bytes of whole members being read, summed over the files.
    std::uint64_t verified_bytes() const;
    /// Bytes from each file's stop point to its end that were not read (an
    /// unfinished tail, or a hole and what follows it), summed over the files.
    std::uint64_t skipped_tail_bytes() const;
    /// The hole at which each file was cut short by the last refresh().
    dstd::Vector<LiveHole> holes() const;

   private:
    struct Source;
    dstd::Vector<dstd::String> paths_;
    dstd::String index_path_;
    dstd::String snapshot_dir_;
    dstd::Vector<dstd::UniquePtr<Source>> sources_;
};

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_LIVE_VIEW_H
