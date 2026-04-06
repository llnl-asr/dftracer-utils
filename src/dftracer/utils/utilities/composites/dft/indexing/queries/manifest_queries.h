#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_QUERIES_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_QUERIES_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

// --- Packed line numbers helpers ---

inline std::vector<unsigned char> pack_line_numbers(
    const std::vector<std::uint32_t>& lines) {
    std::vector<unsigned char> blob(lines.size() * sizeof(std::uint32_t));
    if (!blob.empty()) {
        std::memcpy(blob.data(), lines.data(), blob.size());
    }
    return blob;
}

inline std::vector<std::uint32_t> unpack_line_numbers(const unsigned char* data,
                                                      std::size_t size) {
    std::vector<std::uint32_t> lines(size / sizeof(std::uint32_t));
    if (!lines.empty()) {
        std::memcpy(lines.data(), data, lines.size() * sizeof(std::uint32_t));
    }
    return lines;
}

struct EventRangeResult {
    std::uint64_t checkpoint_idx;
    std::string cat;
    std::string name;
    std::vector<std::uint32_t> line_numbers;
    std::uint64_t event_count;
};

struct MetadataLinesResult {
    std::uint64_t checkpoint_idx;
    std::string meta_type;
    std::vector<std::uint32_t> line_numbers;
};

struct ProvenanceSource {
    int source_idx;
    std::string path;
    int num_checkpoints;
    std::string event_hash;
};

struct ProvenanceSegment {
    int source_idx;
    int source_checkpoint;
    int output_line_start;
    int output_line_end;
    int event_count;
};

}  // namespace
   // dftracer::utils::utilities::composites::dft::indexing::queries

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_QUERIES_H
