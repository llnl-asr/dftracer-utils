#ifndef DFTRACER_UTILS_INDEX_BUILD_CHUNK_INDEXER_H
#define DFTRACER_UTILS_INDEX_BUILD_CHUNK_INDEXER_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/file.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::index::build {

struct ChunkIndexerConfig {
    bool index_name = true;
    bool index_cat = true;
    bool index_pid = true;
    bool index_tid = true;
    bool index_hhash = true;
    bool index_fhash = true;
    bool index_shash = true;

    /// Optional user-specified dimensions (arbitrary dot-paths into args)
    /// e.g. "args.level", "args.mode", "args.io.size"
    std::vector<std::string> extra_dimensions;

    /// Also index the path_budget most frequent other args paths of each
    /// file: numbers get a per-chunk min/max, strings a per-chunk bloom while
    /// the chunk holds at most auto_max_distinct of their values. 0 indexes
    /// only the fixed fields and extra_dimensions.
    std::size_t path_budget = 1024;
    std::size_t auto_max_distinct = 256;

    std::size_t expected_entries_per_chunk = 1024;
    double false_positive_rate = 0.01;

    /// Max compressed size for value_counts BLOB (0 = disable dictionaries)
    std::size_t value_counts_cap = 4096;

    /// Harvest the dftracer fixed dimensions (name, cat, pid, tid, hashes, ts,
    /// dur). Off for records that are not dftracer events.
    bool fixed_dimensions = true;
    /// The prefix under which the path catalog names an automatic key: args
    /// keys of dftracer events, or nothing when every path is a key.
    std::string auto_prefix = "args.";

    /// The pruning extensions to build; dft.stats is built with any of them.
    store::ExtensionMask extensions = store::PRUNING_EXTENSIONS;

    /// Hash of the settings `ext` is built with, recorded in its manifest
    /// entry; a change rebuilds that extension only. The dimensions are
    /// checked separately: asking for fewer than were indexed does not
    /// rebuild.
    std::uint64_t params_hash(store::IndexExtension ext) const {
        utilities::hash::HasherUtility hasher;
        switch (ext) {
            case store::IndexExtension::BLOOM:
                hasher.update(expected_entries_per_chunk);
                hasher.update(false_positive_rate);
                hasher.update(path_budget);
                hasher.update(auto_max_distinct);
                break;
            case store::IndexExtension::COUNTS:
                hasher.update(value_counts_cap);
                hasher.update(path_budget);
                break;
            case store::IndexExtension::ZONEMAP:
                hasher.update(path_budget);
                break;
            case store::IndexExtension::HOST:
            case store::IndexExtension::MEMBERS:
            case store::IndexExtension::DICT:
            case store::IndexExtension::POSTINGS:
            case store::IndexExtension::STATS:
            case store::IndexExtension::CATALOG:
            case store::IndexExtension::PROFILE:
            case store::IndexExtension::METADATA:
            case store::IndexExtension::PLUGIN:
            case store::IndexExtension::AGG:
                return 0;
        }
        return hasher.get_hash().value;
    }
};

/// A field as the index names its dimension: an args key or a dotted path
/// below args, without the "args." prefix a query column carries.
inline std::string extra_dimension_name(std::string_view field) {
    if (field.rfind("args.", 0) == 0) field.remove_prefix(5);
    return std::string(field);
}

/// The settings files of `schema` are indexed with: path-decoded records get
/// no dftracer fixed dimensions or name postings; the schema's always-indexed
/// paths join the extra dimensions and its path budget replaces the build's.
inline ChunkIndexerConfig for_schema(ChunkIndexerConfig config,
                                     const RecordSchema& schema) {
    const bool by_path = schema.decoder == Decoder::PATH;
    if (by_path) {
        config.fixed_dimensions = false;
        config.auto_prefix.clear();
        store::ExtensionMask kept;
        for (auto ext :
             {store::IndexExtension::ZONEMAP, store::IndexExtension::BLOOM,
              store::IndexExtension::COUNTS})
            if (config.extensions.has(ext)) kept.add(ext);
        config.extensions = kept;
    }
    for (const auto& path : schema.always_index) {
        std::string dim = by_path ? path : extra_dimension_name(path);
        if (std::find(config.extra_dimensions.begin(),
                      config.extra_dimensions.end(),
                      dim) == config.extra_dimensions.end())
            config.extra_dimensions.push_back(std::move(dim));
    }
    if (schema.path_budget) config.path_budget = *schema.path_budget;
    return config;
}

/// Tracks which dimensions have been indexed per chunk for incremental updates
struct IndexedDimensions {
    std::vector<std::string> dimensions;

    bool has_dimension(const std::string& dim) const {
        return std::find(dimensions.begin(), dimensions.end(), dim) !=
               dimensions.end();
    }

    std::vector<std::string> missing_dimensions(
        const ChunkIndexerConfig& config) const {
        std::vector<std::string> missing;

        auto check_dim = [this, &missing](const std::string& name,
                                          bool enabled) {
            if (enabled && !has_dimension(name)) {
                missing.emplace_back(name);
            }
        };

        check_dim(std::string("name"), config.index_name);
        check_dim(std::string("cat"), config.index_cat);
        check_dim(std::string("pid"), config.index_pid);
        check_dim(std::string("tid"), config.index_tid);
        check_dim(std::string("hhash"), config.index_hhash);
        check_dim(std::string("fhash"), config.index_fhash);
        check_dim(std::string("shash"), config.index_shash);

        for (const auto& dim : config.extra_dimensions) {
            check_dim(dim, true);
        }

        return missing;
    }
};

/// Per-chunk index state for incremental re-scanning
struct ChunkIndexState {
    std::uint64_t checkpoint_idx = 0;
    std::size_t events_processed = 0;
    IndexedDimensions indexed_dims;
    index::schemas::dft::ChunkStatistics statistics;
    std::uint64_t config_hash = 0;  ///< Detect config changes across re-scans
};

struct ChunkIndexerInput {
    std::string file_path;
    std::string index_path;
    std::size_t checkpoint_size = 0;
    std::uint64_t checkpoint_idx = 0;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    ChunkIndexerConfig config;
    std::size_t batch_size = 4 * 1024 * 1024;

    ChunkIndexerInput& with_file_path(const std::string& path) {
        file_path = path;
        return *this;
    }

    ChunkIndexerInput& with_index_path(const std::string& path) {
        index_path = path;
        return *this;
    }

    ChunkIndexerInput& with_checkpoint_size(std::size_t size) {
        checkpoint_size = size;
        return *this;
    }

    ChunkIndexerInput& with_checkpoint_idx(std::uint64_t idx) {
        checkpoint_idx = idx;
        return *this;
    }

    ChunkIndexerInput& with_byte_range(std::size_t start, std::size_t end) {
        start_byte = start;
        end_byte = end;
        return *this;
    }

    ChunkIndexerInput& with_config(const ChunkIndexerConfig& cfg) {
        config = cfg;
        return *this;
    }

    ChunkIndexerInput& with_batch_size(std::size_t size) {
        batch_size = size;
        return *this;
    }

    /// Existing chunk state for incremental re-scanning
    std::shared_ptr<ChunkIndexState> existing_state;
};

struct ChunkIndexerOutput {
    std::uint64_t checkpoint_idx = 0;
    StringViewMap<index::extensions::ScalableBloomFilter> bloom_filters;
    index::schemas::dft::ChunkStatistics statistics;
    std::size_t events_processed = 0;
    bool success = false;
};

struct ChunkIndexer {
    coro::CoroTask<ChunkIndexerOutput> operator()(
        const ChunkIndexerInput& input);
};

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_CHUNK_INDEXER_H
