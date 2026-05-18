#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_FILE_ENTRY_CAPABILITY_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_FILE_ENTRY_CAPABILITY_H

#include <cstdint>

namespace dftracer::utils::utilities::indexer {

enum class IndexFileEntryCapability : std::uint8_t {
    NONE = 0,
    BLOOM = 1 << 0,
    MANIFEST = 1 << 1,
    FILE_SUMMARY = 1 << 2,
    CHECKPOINTS = 1 << 3,
    INDEXING_COMPLETE = 1 << 4,
};

inline IndexFileEntryCapability operator|(IndexFileEntryCapability a,
                                          IndexFileEntryCapability b) {
    return static_cast<IndexFileEntryCapability>(static_cast<std::uint8_t>(a) |
                                                 static_cast<std::uint8_t>(b));
}
inline IndexFileEntryCapability operator&(IndexFileEntryCapability a,
                                          IndexFileEntryCapability b) {
    return static_cast<IndexFileEntryCapability>(static_cast<std::uint8_t>(a) &
                                                 static_cast<std::uint8_t>(b));
}
inline IndexFileEntryCapability& operator|=(IndexFileEntryCapability& a,
                                            IndexFileEntryCapability b) {
    a = a | b;
    return a;
}
inline bool has_capability(IndexFileEntryCapability caps,
                           IndexFileEntryCapability flag) {
    return (caps & flag) != IndexFileEntryCapability::NONE;
}

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_FILE_ENTRY_CAPABILITY_H
