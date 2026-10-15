#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/utilities/fileio/parallel/layout.h>

#include <algorithm>

namespace dftracer::utils::utilities::fileio::parallel {

LayoutInfo detect_layout(const std::string& path) noexcept {
    LayoutInfo info{};
    info.layout = FileLayout::STRIPED;
    info.fs = filesystem_kind(path);
    info.stripe_size = 0;
    info.stripe_count = 0;

    // NFS lacks a cheap atomic-offset write, so shard into per-writer files.
    if (info.fs == FilesystemKind::NFS) {
        info.layout = FileLayout::SHARDED;
    }
    if (info.fs == FilesystemKind::LUSTRE) {
        const StripeInfo stripe = lustre_stripe(path);
        info.stripe_size = stripe.size;
        info.stripe_count = stripe.count;
    }
    return info;
}

WriterSizing compute_writer_sizing(const LayoutInfo& info,
                                   std::size_t baseline_workers,
                                   std::size_t default_flush_bytes,
                                   std::size_t buffer_headroom_bytes,
                                   bool padded_layout) noexcept {
    WriterSizing s{};
    s.num_workers = baseline_workers == 0 ? 1 : baseline_workers;
    if (!padded_layout && info.stripe_count > 0) {
        s.num_workers = std::min(s.num_workers, info.stripe_count);
    }
    if (padded_layout && info.stripe_size > 0) {
        // Uncompressed flush sized to one stripe; compressed fits easily.
        s.flush_threshold = info.stripe_size;
    } else {
        s.flush_threshold = std::max(default_flush_bytes, info.stripe_size);
    }
    s.buffer_capacity = s.flush_threshold + buffer_headroom_bytes;
    return s;
}

bool uses_padded_layout(const LayoutInfo& info, bool gzip) noexcept {
    return info.layout == FileLayout::STRIPED && gzip &&
           info.stripe_size >= MIN_PADDED_STRIPE_BYTES;
}

}  // namespace dftracer::utils::utilities::fileio::parallel
