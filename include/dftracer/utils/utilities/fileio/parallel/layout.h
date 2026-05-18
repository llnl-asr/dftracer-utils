#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_LAYOUT_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_LAYOUT_H

#include <cstddef>
#include <string>

namespace dftracer::utils::utilities::fileio::parallel {

enum class FileLayout {
    SHARDED,  // N files, glob by name; used on NFS
    STRIPED,  // single file, atomic-offset pwrite; used on local and PFS
};

enum class FilesystemKind {
    UNKNOWN,
    LOCAL,  // ext4, xfs, btrfs, tmpfs, etc.
    NFS,
    LUSTRE,
    GPFS,
    BEEGFS,
};

struct LayoutInfo {
    FileLayout layout;
    FilesystemKind fs;
    std::size_t stripe_size;   // 0 if unknown/not applicable
    std::size_t stripe_count;  // 0 if unknown/not applicable
};

/// Detect layout for a path (the file need not exist yet; falls back to the
/// parent directory). NFS maps to SHARDED, everything else to STRIPED.
LayoutInfo detect_layout(const std::string& path) noexcept;

struct WriterSizing {
    std::size_t num_workers;
    std::size_t flush_threshold;
    std::size_t buffer_capacity;
};

/// Minimum stripe_size for which the padded-striped layout is worth picking.
/// Below this, compressed payloads may not reliably fit one stripe, so we
/// fall back to the atomic-offset striped writer.
constexpr std::size_t MIN_PADDED_STRIPE_BYTES = 1 * 1024 * 1024;

/// Pure sizing policy. Worker count is capped by stripe_count on PFS.
/// For the atomic-offset striped writer, flush_threshold = max(default,
/// stripe_size) to keep pwrites large. For the padded striped writer,
/// flush_threshold = stripe_size so the compressed result fits in one stripe.
/// `baseline_workers` should already be capped at any caller-specific limit
/// (e.g. number of aggregation shards).
WriterSizing compute_writer_sizing(const LayoutInfo& info,
                                   std::size_t baseline_workers,
                                   std::size_t default_flush_bytes,
                                   std::size_t buffer_headroom_bytes,
                                   bool padded_layout = false) noexcept;

}  // namespace dftracer::utils::utilities::fileio::parallel

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_LAYOUT_H
