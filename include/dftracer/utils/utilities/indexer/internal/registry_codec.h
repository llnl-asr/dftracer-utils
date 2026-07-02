#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_REGISTRY_CODEC_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_REGISTRY_CODEC_H

#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/utilities/indexer/error.h>
#include <dftracer/utils/utilities/indexer/index_file_entry_capability.h>
#include <dftracer/utils/utilities/indexer/internal/index_encoding.h>
#include <dftracer/utils/utilities/indexer/internal/iterator_codec.h>
#include <dftracer/utils/utilities/indexer/internal/payload_codec.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::indexer::internal {

inline std::string file_lookup_key(std::string_view logical_name) {
    return std::string("f|") + std::string(logical_name);
}

inline std::string file_reverse_key(int file_id) {
    std::string key("r|");
    dftracer::utils::rocksdb::KeyCodec::append_be32(
        key, static_cast<std::uint32_t>(file_id));
    return key;
}

inline std::string schema_version_key() { return "_schema_version"; }

inline std::string root_scalar_stats_key() { return "_root"; }
inline std::string root_category_counts_key() { return "_root"; }
inline std::string root_name_counts_key() { return "_root"; }
inline std::string root_pid_tid_counts_key() { return "_root"; }

inline std::string tar_archive_key(int file_id) {
    return encoding::prefix_for_file(file_id);
}

inline IndexFileEntryCapability decode_file_capabilities(
    std::string_view record) {
    if (record.size() < 5) {
        return IndexFileEntryCapability::NONE;
    }
    return static_cast<IndexFileEntryCapability>(
        static_cast<std::uint8_t>(record[4]));
}

inline int decode_file_id(std::string_view record) {
    if (record.size() < 4) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt file record");
    }
    return static_cast<int>(
        dftracer::utils::rocksdb::KeyCodec::decode_be32(record.substr(0, 4)));
}

inline int decode_prefixed_file_id(std::string_view key) {
    if (key.size() < 4) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt file-prefixed key");
    }
    return static_cast<int>(
        dftracer::utils::rocksdb::KeyCodec::decode_be32(key.substr(0, 4)));
}

inline std::uint64_t decode_file_hash(std::string_view record) {
    if (record.size() < 28) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt file record");
    }
    return dftracer::utils::rocksdb::KeyCodec::decode_be64(
        record.substr(20, 8));
}

inline std::array<std::uint64_t, 3> decode_metadata_record(
    std::string_view value) {
    Cursor cursor(value);
    return {cursor.u64(), cursor.u64(), cursor.u64()};
}

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_REGISTRY_CODEC_H
