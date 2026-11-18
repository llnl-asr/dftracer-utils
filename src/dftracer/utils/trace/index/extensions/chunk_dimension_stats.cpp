// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/trace/index/extensions/chunk_dimension_stats.h>

namespace dftracer::utils::trace::index::extensions {

void ChunkDimensionStats::add_bound(dstd::StringView value) {
    if (numeric_text) {
        std::uint64_t number = 0;
        const auto parsed = dstd::from_chars(value, number);
        if (parsed.ec == std::errc{} &&
            parsed.ptr == value.data() + value.size()) {
            zone.bounds().add(number);
            return;
        }
    }
    zone.bounds().add(value);
}

void ChunkDimensionStats::observe(dstd::StringView value) {
    ++present;
    if (counts.add_text(value)) add_bound(value);
}

void ChunkDimensionStats::observe(dstd::StringView value, std::uint32_t& slot) {
    ++present;
    if (counts.add_text(value, slot)) add_bound(value);
}

void ChunkDimensionStats::observe_range_only(std::uint64_t value) {
    ++present;
    zone.bounds().add(value);
}

}  // namespace dftracer::utils::trace::index::extensions
