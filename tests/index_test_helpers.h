#ifndef DFTRACER_UTILS_TESTS_INDEX_TEST_HELPERS_H
#define DFTRACER_UTILS_TESTS_INDEX_TEST_HELPERS_H

#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Helpers for tests that write synthetic index data by hand.
namespace dftu_utils_test {

namespace index_records = dftracer::utils::index::store::records;
using dftracer::utils::index::store::IndexExtension;

/// Registers `logical_path` in the write and returns its file id.
inline int register_test_file(
    dftracer::utils::index::store::IndexDatabaseWriterContext& w,
    std::string_view logical_path, std::uint64_t hash = 0,
    std::uint64_t mtime = 0, std::uint64_t size = 0) {
    const int fid = w.file_id_for(logical_path);
    index_records::put_file_record(
        w, logical_path, {static_cast<std::uint32_t>(fid), mtime, hash, size});
    return fid;
}

/// Marks the members and every pruning extension built for `file_id`, so
/// readers use the data a test wrote.
inline void mark_built(dftracer::utils::index::store::IndexWrite& w,
                       int file_id) {
    for (auto ext : {IndexExtension::MEMBERS, IndexExtension::ZONEMAP,
                     IndexExtension::BLOOM, IndexExtension::COUNTS,
                     IndexExtension::POSTINGS, IndexExtension::STATS})
        index_records::put_manifest(w, file_id, ext, 0);
}

/// One dimension's statistics as the dftracer schema writes them: a zonemap
/// and, when the values fit, counts. `observed` is the chunk's data events.
inline void put_dimension_stats(
    dftracer::utils::index::store::IndexWrite& w, int file_id,
    std::uint64_t chunk,
    const dftracer::utils::index::extensions::ChunkDimensionStats& ds,
    std::uint64_t observed) {
    namespace kinds = dftracer::utils::index::extensions::kinds;
    const auto compressed = ds.compress_value_counts(4096);
    std::optional<std::uint64_t> present;
    if (ds.dimension == "ts" || ds.dimension == "dur") {
        present = observed;
    } else if (compressed && ds.value_counts) {
        std::uint64_t carried = 0;
        for (const auto& [value, count] : *ds.value_counts) carried += count;
        present = carried;
    }
    kinds::Zone zone;
    zone.value_type = ds.value_type;
    zone.min = ds.min_value;
    zone.max = ds.max_value;
    zone.observed = observed;
    zone.present = present;
    index_records::put_path_granule(w, IndexExtension::ZONEMAP, file_id,
                                    ds.dimension, chunk,
                                    kinds::encode_zone(zone));
    if (compressed)
        index_records::put_path_granule(
            w, IndexExtension::COUNTS, file_id, ds.dimension, chunk,
            kinds::encode_counts({observed, *compressed}));
    index_records::put_path(w, IndexExtension::ZONEMAP, file_id, ds.dimension);
}

inline void put_chunk_bloom(dftracer::utils::index::store::IndexWrite& w,
                            int file_id, std::uint64_t chunk,
                            std::string_view path,
                            std::span<const unsigned char> bloom,
                            std::uint64_t num_entries) {
    index_records::put_path_granule(
        w, IndexExtension::BLOOM, file_id, path, chunk,
        dftracer::utils::index::extensions::kinds::encode_bloom(bloom,
                                                                num_entries));
}

inline void put_file_bloom(dftracer::utils::index::store::IndexWrite& w,
                           int file_id, std::string_view path,
                           std::span<const unsigned char> bloom,
                           std::uint64_t num_entries) {
    index_records::put_path_file(
        w, IndexExtension::BLOOM, file_id, path,
        dftracer::utils::index::extensions::kinds::encode_bloom(bloom,
                                                                num_entries));
}

/// Chunks that hold a bloom of `path`.
inline std::size_t bloom_chunks(
    const dftracer::utils::index::store::IndexDatabase& db, int file_id,
    std::string_view path) {
    return db.path_granules(file_id, IndexExtension::BLOOM, path).size();
}

inline bool has_file_bloom(
    const dftracer::utils::index::store::IndexDatabase& db, int file_id,
    std::string_view path) {
    return db.path_file_value(file_id, IndexExtension::BLOOM, path).has_value();
}

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_INDEX_TEST_HELPERS_H
