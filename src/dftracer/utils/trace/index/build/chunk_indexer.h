// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_BUILD_CHUNK_INDEXER_H
#define DFTRACER_UTILS_TRACE_INDEX_BUILD_CHUNK_INDEXER_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/hash/hasher_utility.h>
#include <dftracer/utils/trace/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <dftracer/utils/trace/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/trace/index/store/file.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::trace::index::build {

/// Bytes of automatic evidence every file may keep, whatever its size.
inline constexpr std::uint64_t STATS_FLOOR_BYTES = 8ULL << 20;
/// Version of the zonemap, bloom and counts records a build writes; a change
/// in what they mean bumps it so existing evidence rebuilds.
inline constexpr std::uint32_t EVIDENCE_FORMAT_VERSION = 0;

/// The summaries one path is indexed with, in place of the build's defaults.
struct PathSummaries {
    /// The path as the index names its dimension: an args key, a dotted path
    /// below args or a fixed dimension.
    dstd::String path;
    /// The path as the caller wrote it, which names the dimension of a
    /// path-decoded record, whose dimensions keep any "args." prefix.
    dstd::String written;
    /// Any of Zonemap, Bloom and Counts.
    store::ExtensionMask kinds;
};

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
    dstd::Vector<dstd::String> extra_dimensions;

    /// Also index the other args paths of each file, most frequent first,
    /// while their estimated evidence fits max(STATS_FLOOR_BYTES,
    /// stats_share * the file's size on disk) and, when path_budget is above
    /// 0, at most path_budget of them: numbers get a per-chunk min/max,
    /// strings a per-chunk bloom while the chunk holds at most
    /// auto_max_distinct of their values. stats_share is in (0, 1].
    std::size_t path_budget = 0;
    double stats_share = 0.05;
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
    dstd::String auto_prefix = "args.";

    /// The pruning extensions to build; dft.stats is built with any of them.
    store::ExtensionMask extensions = store::PRUNING_EXTENSIONS;

    /// Paths that keep only the listed summaries, sorted by path. A path not
    /// listed keeps every summary of `extensions`.
    dstd::Vector<PathSummaries> path_summaries;

    /// Whether the dimension `dim` is indexed with the summary `ext`.
    bool builds(dstd::StringView dim, store::IndexExtension ext) const {
        if (!extensions.has(ext)) return false;
        for (const PathSummaries& p : path_summaries)
            if (p.path == dim || p.written == dim) return p.kinds.has(ext);
        return true;
    }

    /// Hash of the settings `ext` is built with, recorded in its manifest
    /// entry; a change rebuilds that extension only. The dimensions are
    /// checked separately: asking for fewer than were indexed does not
    /// rebuild.
    std::uint64_t params_hash(store::IndexExtension ext) const {
        fileio::hash::HasherUtility hasher;
        auto selection = [&] {
            hasher.update(EVIDENCE_FORMAT_VERSION);
            hasher.update(STATS_FLOOR_BYTES);
            hasher.update(stats_share);
            hasher.update(path_budget);
            for (const PathSummaries& p : path_summaries) {
                hasher.update(p.path);
                hasher.update(p.written);
                for (auto kind : {store::IndexExtension::Zonemap,
                                  store::IndexExtension::Bloom,
                                  store::IndexExtension::Counts})
                    hasher.update(static_cast<std::uint8_t>(p.kinds.has(kind)));
            }
        };
        switch (ext) {
            case store::IndexExtension::Bloom:
                hasher.update(expected_entries_per_chunk);
                hasher.update(false_positive_rate);
                hasher.update(auto_max_distinct);
                selection();
                break;
            case store::IndexExtension::Counts:
                hasher.update(value_counts_cap);
                selection();
                break;
            case store::IndexExtension::Zonemap:
                selection();
                break;
            case store::IndexExtension::Postings:
                hasher.update(EVIDENCE_FORMAT_VERSION);
                break;
            case store::IndexExtension::Members:
            case store::IndexExtension::Rowset:
            case store::IndexExtension::Stats:
            case store::IndexExtension::Catalog:
            case store::IndexExtension::Profile:
            case store::IndexExtension::Metadata:
            case store::IndexExtension::Plugin:
            case store::IndexExtension::Agg:
            case store::IndexExtension::Format:
                return 0;
        }
        return hasher.hash().value;
    }

    /// An InvalidArgument error naming the setting when stats_share is not in
    /// (0, 1].
    dstd::Result<void> validate() const {
        if (!(stats_share > 0.0 && stats_share <= 1.0))
            return common::make_error<"stats_share must be in (0, 1], got {}">(
                common::ErrorCode::InvalidArgument, stats_share);
        return {};
    }
};

/// A field as the index names its dimension: an args key or a dotted path
/// below args, without the "args." prefix a query column carries.
inline dstd::String extra_dimension_name(dstd::StringView field) {
    if (field.rfind("args.", 0) == 0) field.remove_prefix(5);
    return dstd::String(field);
}

/// Whether `dim` is one of the fixed dimensions of dftracer events, which are
/// indexed without being named.
inline bool is_fixed_dimension(dstd::StringView dim) {
    for (dstd::StringView fixed : {"name", "cat", "pid", "tid", "pid_tid",
                                   "hhash", "fhash", "shash", "ts", "dur"})
        if (dim == fixed) return true;
    return false;
}

/// The settings files of `schema` are indexed with: path-decoded records get
/// no dftracer fixed dimensions or name postings; the schema's always-indexed
/// paths join the extra dimensions, its never-indexed paths leave them, and
/// its path budget and stats share replace the build's.
inline ChunkIndexerConfig for_schema(ChunkIndexerConfig config,
                                     const RecordSchema& schema) {
    const bool by_path = schema.decoder == Decoder::Path;
    if (by_path) {
        config.fixed_dimensions = false;
        config.auto_prefix.clear();
        store::ExtensionMask kept;
        for (auto ext :
             {store::IndexExtension::Zonemap, store::IndexExtension::Bloom,
              store::IndexExtension::Counts})
            if (config.extensions.has(ext)) kept.add(ext);
        config.extensions = kept;
    }
    for (const auto& path : schema.always_index) {
        dstd::String dim = by_path ? path : extra_dimension_name(path);
        if (std::find(config.extra_dimensions.begin(),
                      config.extra_dimensions.end(),
                      dim) == config.extra_dimensions.end())
            config.extra_dimensions.push_back(std::move(dim));
    }
    if (by_path)
        for (const PathSummaries& p : config.path_summaries)
            if (!p.kinds.empty() &&
                std::find(config.extra_dimensions.begin(),
                          config.extra_dimensions.end(),
                          p.written) == config.extra_dimensions.end())
                config.extra_dimensions.push_back(p.written);
    for (const auto& path : schema.never_index)
        std::erase(config.extra_dimensions, path);
    if (schema.path_budget) config.path_budget = *schema.path_budget;
    if (schema.stats_share) config.stats_share = *schema.stats_share;
    return config;
}

/// Tracks which dimensions have been indexed per chunk for incremental updates
struct IndexedDimensions {
    dstd::Vector<dstd::String> dimensions;

    bool has_dimension(dstd::StringView dim) const {
        return std::find(dimensions.begin(), dimensions.end(), dim) !=
               dimensions.end();
    }

    dstd::Vector<dstd::String> missing_dimensions(
        const ChunkIndexerConfig& config) const {
        dstd::Vector<dstd::String> missing;

        auto check_dim = [this, &missing](dstd::StringView name, bool enabled) {
            if (enabled && !has_dimension(name)) {
                missing.emplace_back(name);
            }
        };

        check_dim(dstd::String("name"), config.index_name);
        check_dim(dstd::String("cat"), config.index_cat);
        check_dim(dstd::String("pid"), config.index_pid);
        check_dim(dstd::String("tid"), config.index_tid);
        check_dim(dstd::String("hhash"), config.index_hhash);
        check_dim(dstd::String("fhash"), config.index_fhash);
        check_dim(dstd::String("shash"), config.index_shash);

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
    trace::index::schemas::dft::ChunkStatistics statistics;
    std::uint64_t config_hash = 0;  ///< Detect config changes across re-scans
};

struct ChunkIndexerInput {
    dstd::String file_path;
    dstd::String index_path;
    std::size_t checkpoint_size = 0;
    std::uint64_t checkpoint_idx = 0;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    ChunkIndexerConfig config;
    std::size_t batch_size = 4 * 1024 * 1024;

    ChunkIndexerInput& with_file_path(dstd::StringView path) {
        file_path = path;
        return *this;
    }

    ChunkIndexerInput& with_index_path(dstd::StringView path) {
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
    dstd::SharedPtr<ChunkIndexState> existing_state;
};

struct ChunkIndexerOutput {
    std::uint64_t checkpoint_idx = 0;
    dstd::StringViewMap<trace::index::extensions::ScalableBloomFilter>
        bloom_filters;
    trace::index::schemas::dft::ChunkStatistics statistics;
    std::size_t events_processed = 0;
};

struct ChunkIndexer {
    /// Scans one chunk. The error is the failure to open the file, the reader
    /// or the stream, or to scan the chunk.
    coro::CoroTask<dstd::Result<ChunkIndexerOutput>> operator()(
        const ChunkIndexerInput& input);
};

}  // namespace dftracer::utils::trace::index::build

#endif  // DFTRACER_UTILS_TRACE_INDEX_BUILD_CHUNK_INDEXER_H
