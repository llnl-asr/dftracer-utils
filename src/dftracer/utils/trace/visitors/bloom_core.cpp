#include <dftracer/utils/trace/indexing/chunk_statistics.h>
#include <dftracer/utils/trace/indexing/scalable_bloom_filter.h>
#include <dftracer/utils/trace/visitors/bloom_core.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>

#include <array>
#include <charconv>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using dftracer::utils::trace::indexing::ScalableBloomFilter;
namespace dftracer::utils::trace::visitors {

namespace {

constexpr std::string_view DIM_NAME = "name";
constexpr std::string_view DIM_CAT = "cat";
constexpr std::string_view DIM_PID = "pid";
constexpr std::string_view DIM_TID = "tid";
constexpr std::string_view DIM_PID_TID = "pid_tid";
constexpr std::string_view DIM_HHASH = "hhash";
constexpr std::string_view DIM_FHASH = "fhash";
constexpr std::string_view DIM_SHASH = "shash";
constexpr std::string_view DIM_TS = "ts";
constexpr std::string_view DIM_DUR = "dur";

constexpr std::array<std::string_view, BloomCore::BF_COUNT> FIXED_BLOOM_NAMES =
    {DIM_NAME, DIM_CAT, DIM_PID, DIM_TID, DIM_HHASH, DIM_FHASH, DIM_SHASH};

constexpr std::array<std::string_view, BloomCore::FD_COUNT> FIXED_DIM_NAMES = {
    DIM_NAME,  DIM_CAT,   DIM_PID,   DIM_TID, DIM_PID_TID,
    DIM_HHASH, DIM_FHASH, DIM_SHASH, DIM_TS,  DIM_DUR};

/// Emit bloom/stats/dimension records to a sink that might be either a
/// RocksDB-backed writer or an SST file emitter. Returns the accumulated
/// file-level statistics so downstream callers can use them for name
/// postings and root-summary refresh on the concrete writer.
BloomCore::ChunkStatistics persist_bloom_sink_writes(
    utilities::indexer::IndexBatchSink& db, int file_id,
    const std::vector<std::string>& extra_dim_names,
    const std::vector<BloomCore::ChunkState>& chunks,
    const BloomCore::ChunkIndexerConfig& config) {
    BloomCore::ChunkStatistics file_statistics;

    std::array<ScalableBloomFilter, BloomCore::BF_COUNT> file_fixed_blooms = {
        ScalableBloomFilter(config.expected_entries_per_chunk,
                            config.false_positive_rate),
        ScalableBloomFilter(config.expected_entries_per_chunk,
                            config.false_positive_rate),
        ScalableBloomFilter(config.expected_entries_per_chunk,
                            config.false_positive_rate),
        ScalableBloomFilter(config.expected_entries_per_chunk,
                            config.false_positive_rate),
        ScalableBloomFilter(config.expected_entries_per_chunk,
                            config.false_positive_rate),
        ScalableBloomFilter(config.expected_entries_per_chunk,
                            config.false_positive_rate),
        ScalableBloomFilter(config.expected_entries_per_chunk,
                            config.false_positive_rate),
    };
    std::vector<ScalableBloomFilter> file_extra_blooms;
    file_extra_blooms.reserve(extra_dim_names.size());
    for (std::size_t i = 0; i < extra_dim_names.size(); ++i) {
        file_extra_blooms.emplace_back(config.expected_entries_per_chunk,
                                       config.false_positive_rate);
    }

    std::vector<std::uint8_t> file_extra_skip(extra_dim_names.size(), 0);
    std::vector<unsigned char> blob;

    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk = chunks[i];
        auto checkpoint_idx = static_cast<std::uint64_t>(i);

        for (std::size_t b = 0; b < BloomCore::BF_COUNT; ++b) {
            const ScalableBloomFilter& bf = chunk.fixed_blooms[b];
            bf.serialize_into(blob);
            db.insert_chunk_bloom_filter(
                file_id, checkpoint_idx, std::string(FIXED_BLOOM_NAMES[b]),
                std::span<const unsigned char>(blob.data(), blob.size()),
                static_cast<std::uint64_t>(bf.num_entries()));
            file_fixed_blooms[b].merge_from(bf);
        }
        for (std::size_t e = 0;
             e < extra_dim_names.size() && e < chunk.extra_blooms.size(); ++e) {
            if (e < chunk.extra_bloom_skip.size() &&
                chunk.extra_bloom_skip[e]) {
                file_extra_skip[e] = 1;
                continue;
            }
            const ScalableBloomFilter& bf = chunk.extra_blooms[e];
            bf.serialize_into(blob);
            db.insert_chunk_bloom_filter(
                file_id, checkpoint_idx, extra_dim_names[e],
                std::span<const unsigned char>(blob.data(), blob.size()),
                static_cast<std::uint64_t>(bf.num_entries()));
            file_extra_blooms[e].merge_from(bf);
        }

        db.insert_chunk_statistics(file_id, checkpoint_idx, chunk.statistics);
        file_statistics.merge_from(chunk.statistics);

        for (std::size_t d = 0; d < BloomCore::FD_COUNT; ++d) {
            db.insert_chunk_dimension_stats(file_id, checkpoint_idx,
                                            chunk.fixed_dim_stats[d],
                                            config.value_counts_cap);
        }
        for (const auto& ds : chunk.extra_dim_stats) {
            db.insert_chunk_dimension_stats(file_id, checkpoint_idx, ds,
                                            config.value_counts_cap);
        }
    }

    for (std::size_t b = 0; b < BloomCore::BF_COUNT; ++b) {
        const ScalableBloomFilter& bf = file_fixed_blooms[b];
        bf.serialize_into(blob);
        db.insert_file_bloom_filter(
            file_id, std::string(FIXED_BLOOM_NAMES[b]),
            std::span<const unsigned char>(blob.data(), blob.size()),
            static_cast<std::uint64_t>(bf.num_entries()));
    }
    for (std::size_t e = 0; e < extra_dim_names.size(); ++e) {
        if (file_extra_skip[e]) continue;
        const ScalableBloomFilter& bf = file_extra_blooms[e];
        bf.serialize_into(blob);
        db.insert_file_bloom_filter(
            file_id, extra_dim_names[e],
            std::span<const unsigned char>(blob.data(), blob.size()),
            static_cast<std::uint64_t>(bf.num_entries()));
    }

    for (std::size_t b = 0; b < BloomCore::BF_COUNT; ++b) {
        db.insert_index_dimension(file_id, std::string(FIXED_BLOOM_NAMES[b]));
    }
    for (const auto& dim : extra_dim_names) {
        db.insert_index_dimension(file_id, dim);
    }
    db.insert_index_dimension(file_id, std::string(DIM_TS));
    db.insert_index_dimension(file_id, std::string(DIM_DUR));

    db.insert_file_scalar_stats(file_id, file_statistics, chunks.size());
    db.insert_file_category_counts(file_id, file_statistics.category_counts);
    db.insert_file_name_counts(file_id, file_statistics.name_counts);
    db.insert_file_pid_tid_counts(file_id, file_statistics.pid_tid_counts);

    // name_id is a pure FNV1a hash, so dictionary/posting inserts are
    // idempotent and safe to duplicate across workers or sink backends.
    std::unordered_map<std::string, std::uint64_t> file_name_ids;
    file_name_ids.reserve(file_statistics.name_counts.size());
    for (const auto& [name, _] : file_statistics.name_counts) {
        const auto name_id = utilities::hash::fnv1a_hash(name);
        file_name_ids.emplace(name, name_id);
        db.insert_name_dictionary_entry(name_id, name);
        db.insert_name_file_posting(name_id, file_id);
    }

    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const auto checkpoint_idx = static_cast<std::uint64_t>(i);
        const auto& chunk = chunks[i];
        for (const auto& [name, _] : chunk.statistics.name_counts) {
            auto name_id_it = file_name_ids.find(name);
            if (name_id_it != file_name_ids.end()) {
                db.insert_name_chunk_posting(name_id_it->second, file_id,
                                             checkpoint_idx);
            }
        }
    }

    return file_statistics;
}

/// Concrete-only tail: root-summary refresh. Requires a read-through
/// (`has_file_scalar_stats`) and writes to the ROOT_* column families, which
/// are not yet covered by the distributed SST path.
void persist_bloom_concrete_tail(
    utilities::indexer::IndexDatabaseWriterContext& db, int file_id,
    const BloomCore::ChunkStatistics& file_statistics, std::size_t num_chunks,
    bool refresh_root_summaries) {
    if (!refresh_root_summaries) return;
    const bool had_existing_file_summary = db.has_file_scalar_stats(file_id);
    db.refresh_root_summaries_after_file_write(
        file_id, file_statistics, num_chunks, had_existing_file_summary);
}

}  // namespace

BloomCore::ChunkState::ChunkState() = default;

namespace {

// The type of a dimension holding values of both types; "" is no value yet.
std::string join_value_types(const std::string& a, const std::string& b) {
    if (a.empty() || a == b) return b;
    if (b.empty()) return a;
    if (a == "mixed" || b == "mixed") return "mixed";
    const bool num_a = a == "int" || a == "double";
    const bool num_b = b == "int" || b == "double";
    return num_a && num_b ? "double" : "mixed";
}

void observe_stats(indexing::ChunkDimensionStats& ds, const std::string& text,
                   const char* type) {
    if (ds.value_type == "mixed") return;
    const std::string joined = join_value_types(ds.value_type, type);
    if (joined != ds.value_type) ds.value_type = joined;
    if (joined == "mixed") {
        ds.min_value.clear();
        ds.max_value.clear();
        return;
    }
    if (ds.min_value.empty() ||
        indexing::dimension_value_less(text, ds.min_value, ds.value_type))
        ds.min_value = text;
    if (ds.max_value.empty() ||
        indexing::dimension_value_less(ds.max_value, text, ds.value_type))
        ds.max_value = text;
}

}  // namespace

void BloomCore::observe_value(ChunkDimensionStats& stats, std::int64_t value) {
    observe_stats(stats, std::to_string(value), "int");
}

void BloomCore::observe_value(ChunkDimensionStats& stats, double value) {
    observe_stats(stats, indexing::canonical_number_text(value), "double");
}

void BloomCore::observe_value(ChunkDimensionStats& stats,
                              std::string_view value) {
    observe_stats(stats, std::string(value), "string");
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e,
                              std::int64_t value) {
    const std::string text = std::to_string(value);
    chunk.extra_blooms[e].add(text);
    observe_stats(chunk.extra_dim_stats[e], text, "int");
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e, double value) {
    const std::string text = indexing::canonical_number_text(value);
    chunk.extra_blooms[e].add(text);
    observe_stats(chunk.extra_dim_stats[e], text, "double");
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e,
                              std::string_view value) {
    const std::string text(value);
    chunk.extra_blooms[e].add(text);
    observe_stats(chunk.extra_dim_stats[e], text, "string");
}

void BloomCore::init_chunk_state(ChunkState& chunk,
                                 const ChunkIndexerConfig& config,
                                 const std::vector<std::string>& extra_dims) {
    for (std::size_t b = 0; b < BF_COUNT; ++b) {
        chunk.fixed_blooms[b] = ScalableBloomFilter(
            config.expected_entries_per_chunk, config.false_positive_rate);
    }
    for (std::size_t d = 0; d < FD_COUNT; ++d) {
        auto& ds = chunk.fixed_dim_stats[d];
        ds.dimension = std::string(FIXED_DIM_NAMES[d]);
        ds.value_type =
            (d == FD_PID || d == FD_TID || d == FD_TS || d == FD_DUR)
                ? "uint"
                : "string";
    }
    chunk.extra_blooms.clear();
    chunk.extra_dim_stats.clear();
    chunk.extra_blooms.reserve(extra_dims.size());
    chunk.extra_dim_stats.resize(extra_dims.size());
    for (std::size_t e = 0; e < extra_dims.size(); ++e) {
        chunk.extra_blooms.emplace_back(config.expected_entries_per_chunk,
                                        config.false_positive_rate);
        chunk.extra_dim_stats[e].dimension = extra_dims[e];
        chunk.extra_dim_stats[e].value_type.clear();
    }
    chunk.extra_bloom_skip.assign(extra_dims.size(), 0);
}

void BloomCore::observe_metadata(ChunkState& chunk,
                                 std::string_view record_name,
                                 std::string_view hash_val,
                                 std::string_view resolved) {
    if (hash_val.empty() || resolved.empty()) return;
    std::string_view dim;
    if (record_name == "HH") {
        dim = DIM_HHASH;
    } else if (record_name == "FH") {
        dim = DIM_FHASH;
    } else if (record_name == "SH") {
        dim = DIM_SHASH;
    } else {
        return;
    }
    auto outer_it = chunk.hash_resolutions.find(dim);
    if (outer_it == chunk.hash_resolutions.end()) {
        outer_it = chunk.hash_resolutions
                       .emplace(std::string(dim), StringViewMap<std::string>{})
                       .first;
    }
    auto& inner = outer_it->second;
    auto inner_it = inner.find(hash_val);
    if (inner_it == inner.end()) {
        inner.emplace(std::string(hash_val), std::string(resolved));
    } else {
        inner_it->second.assign(resolved.data(), resolved.size());
    }
}

void BloomCore::observe_data(ChunkState& chunk, PidTidCache& cache,
                             const ChunkIndexerConfig& config,
                             std::string_view name, std::string_view cat,
                             std::uint64_t pid, std::uint64_t tid,
                             std::uint64_t ts, std::uint64_t dur, bool has_dur,
                             std::string_view hhash, std::string_view fhash,
                             std::string_view shash) {
    chunk.statistics.update_from_event(name, cat, pid, tid, ts, dur, has_dur);

    auto observe_fixed = [&chunk](int bloom_idx, std::size_t dim_idx,
                                  std::string_view val) {
        if (val.empty()) return;
        if (bloom_idx >= 0) chunk.fixed_blooms[bloom_idx].add(val);
        chunk.fixed_dim_stats[dim_idx].observe(val);
    };

    observe_fixed(BF_NAME, FD_NAME, name);
    observe_fixed(BF_CAT, FD_CAT, cat);

    if (pid != cache.last_pid || cache.pid_len == 0) {
        auto [pp, _1] = std::to_chars(
            cache.pid_buf, cache.pid_buf + sizeof(cache.pid_buf), pid);
        cache.pid_len = static_cast<std::uint8_t>(pp - cache.pid_buf);
        cache.last_pid = pid;
    }
    if (tid != cache.last_tid || cache.tid_len == 0) {
        auto [tp, _2] = std::to_chars(
            cache.tid_buf, cache.tid_buf + sizeof(cache.tid_buf), tid);
        cache.tid_len = static_cast<std::uint8_t>(tp - cache.tid_buf);
        cache.last_tid = tid;
    }
    std::string_view pid_sv(cache.pid_buf, cache.pid_len);
    std::string_view tid_sv(cache.tid_buf, cache.tid_len);

    observe_fixed(BF_PID, FD_PID, pid_sv);
    observe_fixed(BF_TID, FD_TID, tid_sv);

    char pt_buf[52];
    std::memcpy(pt_buf, cache.pid_buf, cache.pid_len);
    pt_buf[cache.pid_len] = ':';
    std::memcpy(pt_buf + cache.pid_len + 1, cache.tid_buf, cache.tid_len);
    std::string_view pt_sv(pt_buf, cache.pid_len + 1 + cache.tid_len);
    observe_fixed(-1, FD_PID_TID, pt_sv);

    chunk.fixed_dim_stats[FD_TS].observe_range_only(ts);
    chunk.fixed_dim_stats[FD_DUR].observe_range_only(dur);

    if (config.sub_chunk_events > 0) {
        auto& buckets = chunk.statistics.sub_zonemaps;
        if (buckets.empty() ||
            buckets.back().event_count >= config.sub_chunk_events) {
            buckets.emplace_back();
        }
        buckets.back().observe(ts, dur);
    }

    observe_fixed(BF_HHASH, FD_HHASH, hhash);
    observe_fixed(BF_FHASH, FD_FHASH, fhash);
    observe_fixed(BF_SHASH, FD_SHASH, shash);

    chunk.events_processed++;
}

void BloomCore::merge_dimension_stats(ChunkDimensionStats& dst,
                                      ChunkDimensionStats& src) {
    if (src.value_counts) {
        if (!dst.value_counts) dst.value_counts.emplace();
        for (const auto& [k, v] : *src.value_counts) {
            (*dst.value_counts)[k] += v;
        }
        dst.distinct_count = dst.value_counts->size();
    }
    if (!src.value_type.empty() && dst.value_type != src.value_type) {
        const std::string joined =
            join_value_types(dst.value_type, src.value_type);
        if (joined != dst.value_type) dst.value_type = joined;
        if (joined == "mixed") {
            dst.min_value.clear();
            dst.max_value.clear();
            return;
        }
    }
    if (src.min_value.empty() && src.max_value.empty()) return;
    if (dst.min_value.empty() ||
        (!src.min_value.empty() &&
         indexing::dimension_value_less(src.min_value, dst.min_value,
                                        dst.value_type)))
        dst.min_value = src.min_value;
    if (dst.max_value.empty() ||
        (!src.max_value.empty() &&
         indexing::dimension_value_less(dst.max_value, src.max_value,
                                        dst.value_type)))
        dst.max_value = src.max_value;
}

void BloomCore::merge_chunk_state(ChunkState& dst, ChunkState& src) {
    for (std::size_t b = 0; b < BF_COUNT; ++b) {
        dst.fixed_blooms[b].merge_from(src.fixed_blooms[b]);
    }
    for (std::size_t e = 0;
         e < src.extra_blooms.size() && e < dst.extra_blooms.size(); ++e) {
        dst.extra_blooms[e].merge_from(src.extra_blooms[e]);
    }

    auto merge_dim = [](ChunkDimensionStats& dds, ChunkDimensionStats& sds) {
        merge_dimension_stats(dds, sds);
    };
    for (std::size_t d = 0; d < FD_COUNT; ++d) {
        merge_dim(dst.fixed_dim_stats[d], src.fixed_dim_stats[d]);
    }
    for (std::size_t e = 0;
         e < src.extra_dim_stats.size() && e < dst.extra_dim_stats.size();
         ++e) {
        merge_dim(dst.extra_dim_stats[e], src.extra_dim_stats[e]);
    }
    for (std::size_t e = 0;
         e < src.extra_bloom_skip.size() && e < dst.extra_bloom_skip.size();
         ++e)
        dst.extra_bloom_skip[e] |= src.extra_bloom_skip[e];

    dst.statistics.merge_from(src.statistics);

    // Slices merge in event order, so appending keeps sub-chunk buckets
    // contiguous across the whole member.
    auto& dst_buckets = dst.statistics.sub_zonemaps;
    auto& src_buckets = src.statistics.sub_zonemaps;
    dst_buckets.insert(dst_buckets.end(),
                       std::make_move_iterator(src_buckets.begin()),
                       std::make_move_iterator(src_buckets.end()));

    for (auto& [dim, inner] : src.hash_resolutions) {
        auto outer_it = dst.hash_resolutions.find(dim);
        if (outer_it == dst.hash_resolutions.end()) {
            dst.hash_resolutions.emplace(dim, std::move(inner));
        } else {
            for (auto& [k, v] : inner) {
                outer_it->second.try_emplace(k, std::move(v));
            }
        }
    }

    dst.events_processed += src.events_processed;
}

BloomCore::ChunkStatistics BloomCore::persist_bloom_sink(
    utilities::indexer::IndexBatchSink& sink, int file_id,
    const std::vector<ChunkState>& chunks, const ChunkIndexerConfig& config,
    const std::vector<std::string>& extra_dims,
    const dftracer::utils::StringViewMap<utilities::indexer::ColumnType>&
        columns) {
    auto file_statistics =
        persist_bloom_sink_writes(sink, file_id, extra_dims, chunks, config);
    for (const auto& [c, t] : columns) sink.insert_column(file_id, c, t);
    return file_statistics;
}

BloomCore::ChunkStatistics BloomCore::persist_bloom(
    utilities::indexer::IndexDatabaseWriterContext& db, int file_id,
    const std::vector<ChunkState>& chunks, const ChunkIndexerConfig& config,
    const std::vector<std::string>& extra_dims,
    const dftracer::utils::StringViewMap<utilities::indexer::ColumnType>&
        columns,
    bool refresh_root_summaries) {
    auto file_statistics =
        persist_bloom_sink(db, file_id, chunks, config, extra_dims, columns);
    persist_bloom_concrete_tail(db, file_id, file_statistics, chunks.size(),
                                refresh_root_summaries);
    return file_statistics;
}

}  // namespace dftracer::utils::trace::visitors
