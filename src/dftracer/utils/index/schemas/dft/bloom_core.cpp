#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/schemas/dft/bloom_core.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/index_write.h>

#include <array>
#include <charconv>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using dftracer::utils::index::extensions::ScalableBloomFilter;
namespace dftracer::utils::index::schemas::dft {

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
constexpr std::string_view DIM_TE = "te";

constexpr std::array<std::string_view, BloomCore::BF_COUNT> FIXED_BLOOM_NAMES =
    {DIM_NAME, DIM_CAT, DIM_PID, DIM_TID, DIM_HHASH, DIM_FHASH, DIM_SHASH};

constexpr std::array<std::string_view, BloomCore::FD_COUNT> FIXED_DIM_NAMES = {
    DIM_NAME,  DIM_CAT,   DIM_PID,   DIM_TID, DIM_PID_TID,
    DIM_HHASH, DIM_FHASH, DIM_SHASH, DIM_TS,  DIM_DUR};

namespace records = index::store::records;
namespace kinds = index::extensions::kinds;
using index::store::IndexExtension;

void put_bloom(index::store::IndexWrite& w, int file_id,
               std::uint64_t checkpoint_idx, std::string_view path,
               const ScalableBloomFilter& bf,
               std::vector<unsigned char>& blob) {
    bf.serialize_into(blob);
    records::put_path_granule(
        w, IndexExtension::BLOOM, file_id, path, checkpoint_idx,
        kinds::encode_bloom(blob,
                            static_cast<std::uint64_t>(bf.num_entries())));
}

void put_file_bloom(index::store::IndexWrite& w, int file_id,
                    std::string_view path, const ScalableBloomFilter& bf,
                    std::vector<unsigned char>& blob) {
    bf.serialize_into(blob);
    records::put_path_file(w, IndexExtension::BLOOM, file_id, path,
                           kinds::encode_bloom(blob, static_cast<std::uint64_t>(
                                                         bf.num_entries())));
}

// One dimension's statistics as a zonemap and, when its value dictionary fit
// the cap, counts. `observed` is the chunk's data events.
void put_dimension(index::store::IndexWrite& w, int file_id,
                   std::uint64_t checkpoint_idx,
                   const BloomCore::ChunkDimensionStats& ds,
                   std::uint64_t observed,
                   const BloomCore::ChunkIndexerConfig& config,
                   const BloomCore::ChunkStatistics& stats) {
    const bool zones = config.extensions.has(IndexExtension::ZONEMAP);
    const bool counts = config.extensions.has(IndexExtension::COUNTS);
    if (!zones && !counts) return;
    const bool always_present =
        ds.dimension == DIM_TS || ds.dimension == DIM_DUR;
    const auto compressed = ds.compress_value_counts(config.value_counts_cap);
    std::optional<std::uint64_t> present;
    if (always_present) {
        present = observed;
    } else if (compressed && ds.value_counts) {
        std::uint64_t carried = 0;
        for (const auto& [value, count] : *ds.value_counts) carried += count;
        present = carried;
    }
    if (zones && (!ds.min_value.empty() || !ds.max_value.empty())) {
        kinds::Zone zone;
        zone.value_type = ds.value_type;
        zone.min = ds.min_value;
        zone.max = ds.max_value;
        zone.observed = observed;
        zone.present = present;
        if (ds.dimension == DIM_TS && !stats.timestamp_histogram.empty())
            zone.histogram = stats.timestamp_histogram;
        records::put_path_granule(w, IndexExtension::ZONEMAP, file_id,
                                  ds.dimension, checkpoint_idx,
                                  kinds::encode_zone(zone));
    }
    if (counts && compressed)
        records::put_path_granule(
            w, IndexExtension::COUNTS, file_id, ds.dimension, checkpoint_idx,
            kinds::encode_counts({observed, *compressed}));
}

}  // namespace

BloomCore::FileAccumulator::FileAccumulator(const ChunkIndexerConfig& config)
    : config_(&config) {
    fixed_blooms.reserve(BF_COUNT);
    for (std::size_t b = 0; b < BF_COUNT; ++b)
        fixed_blooms.emplace_back(config.expected_entries_per_chunk,
                                  config.false_positive_rate);
}

BloomCore::FileAccumulator::ExtraBloom& BloomCore::FileAccumulator::extra(
    std::string_view dim) {
    auto it = extras.find(dim);
    if (it == extras.end())
        it = extras
                 .emplace(std::string(dim),
                          ExtraBloom{ScalableBloomFilter(
                                         config_->expected_entries_per_chunk,
                                         config_->false_positive_rate),
                                     false})
                 .first;
    return it->second;
}

void BloomCore::write_chunk(index::store::IndexWrite& w, int file_id,
                            std::uint64_t checkpoint_idx,
                            const ChunkState& chunk,
                            const std::vector<std::string>& extra_dims,
                            const ChunkIndexerConfig& config,
                            FileAccumulator& acc) {
    const auto& m = config.extensions;
    const bool blooms = m.has(IndexExtension::BLOOM);
    const bool zones = m.has(IndexExtension::ZONEMAP);
    const bool fixed = config.fixed_dimensions;
    const auto observed = chunk.statistics.total_events;
    auto& blob = acc.blob;

    for (std::size_t b = 0; fixed && blooms && b < BF_COUNT; ++b) {
        put_bloom(w, file_id, checkpoint_idx, FIXED_BLOOM_NAMES[b],
                  chunk.fixed_blooms[b], blob);
        acc.fixed_blooms[b].merge_from(chunk.fixed_blooms[b]);
    }
    for (std::size_t e = 0;
         blooms && e < extra_dims.size() && e < chunk.extra_blooms.size();
         ++e) {
        auto& file_bloom = acc.extra(extra_dims[e]);
        if (e < chunk.extra_bloom_skip.size() && chunk.extra_bloom_skip[e]) {
            file_bloom.skip = true;
            continue;
        }
        put_bloom(w, file_id, checkpoint_idx, extra_dims[e],
                  chunk.extra_blooms[e], blob);
        file_bloom.bloom.merge_from(chunk.extra_blooms[e]);
    }

    records::put_chunk_statistics(w, file_id, checkpoint_idx, chunk.statistics);
    records::put_chunk_metadata(w, file_id, checkpoint_idx, chunk.metadata);
    acc.statistics.merge_from(chunk.statistics);
    ++acc.chunks;

    for (const auto& ds : chunk.fixed_dim_stats)
        if (fixed)
            put_dimension(w, file_id, checkpoint_idx, ds, observed, config,
                          chunk.statistics);
    for (const auto& ds : chunk.extra_dim_stats)
        put_dimension(w, file_id, checkpoint_idx, ds, observed, config,
                      chunk.statistics);
    // Event end (ts + dur): the time bounds a time range prunes with. Its
    // min is the first start, a lower bound of every end.
    if (zones && observed > 0 &&
        chunk.statistics.min_timestamp_us <=
            chunk.statistics.max_timestamp_us) {
        kinds::Zone te;
        te.value_type = "uint";
        te.min = std::to_string(chunk.statistics.min_timestamp_us);
        te.max = std::to_string(chunk.statistics.max_timestamp_us);
        te.observed = observed;
        te.present = observed;
        records::put_path_granule(w, IndexExtension::ZONEMAP, file_id, DIM_TE,
                                  checkpoint_idx, kinds::encode_zone(te));
    }

    // Postings hash the value, so postings written by concurrent workers or
    // sinks agree.
    if (m.has(IndexExtension::POSTINGS))
        for (const auto& [name, _] : chunk.statistics.name_counts)
            records::put_posting_granule(
                w, file_id, DIM_NAME, kinds::value_hash(name), checkpoint_idx);
}

void BloomCore::write_absent_extra(index::store::IndexWrite& w, int file_id,
                                   std::uint64_t checkpoint_idx,
                                   std::string_view dim,
                                   const ChunkIndexerConfig& config,
                                   FileAccumulator& acc) {
    if (!config.extensions.has(IndexExtension::BLOOM)) return;
    const ScalableBloomFilter empty(config.expected_entries_per_chunk,
                                    config.false_positive_rate);
    put_bloom(w, file_id, checkpoint_idx, dim, empty, acc.blob);
    acc.extra(dim);
}

BloomCore::ChunkStatistics BloomCore::finish_file(
    index::store::IndexWrite& w, int file_id, FileAccumulator& acc,
    const ChunkIndexerConfig& config,
    const std::vector<std::string>& extra_dims, const Catalog& catalog) {
    const auto& m = config.extensions;
    const bool blooms = m.has(IndexExtension::BLOOM);
    const bool zones = m.has(IndexExtension::ZONEMAP);
    const bool fixed = config.fixed_dimensions;
    auto& blob = acc.blob;

    for (std::size_t b = 0; fixed && blooms && b < BF_COUNT; ++b) {
        put_file_bloom(w, file_id, FIXED_BLOOM_NAMES[b], acc.fixed_blooms[b],
                       blob);
        records::put_path(w, IndexExtension::BLOOM, file_id,
                          FIXED_BLOOM_NAMES[b]);
    }
    for (std::size_t e = 0; blooms && e < extra_dims.size(); ++e) {
        auto& file_bloom = acc.extra(extra_dims[e]);
        if (!file_bloom.skip)
            put_file_bloom(w, file_id, extra_dims[e], file_bloom.bloom, blob);
        records::put_path(w, IndexExtension::BLOOM, file_id, extra_dims[e]);
    }
    if (fixed && zones)
        records::put_path(w, IndexExtension::ZONEMAP, file_id, DIM_TE);
    for (auto ext : {IndexExtension::ZONEMAP, IndexExtension::COUNTS}) {
        if (!m.has(ext)) continue;
        for (auto dim : FIXED_DIM_NAMES)
            if (fixed) records::put_path(w, ext, file_id, dim);
        for (const auto& dim : extra_dims)
            records::put_path(w, ext, file_id, dim);
    }

    records::put_file_scalar_stats(w, file_id, acc.statistics, acc.chunks);
    records::put_file_pid_tid_counts(w, file_id, acc.statistics.pid_tid_counts);

    if (m.has(IndexExtension::POSTINGS)) {
        records::put_path(w, IndexExtension::POSTINGS, file_id, DIM_NAME);
        for (const auto& [name, _] : acc.statistics.name_counts)
            records::put_posting(w, file_id, DIM_NAME, kinds::value_hash(name));
    }

    for (const auto& [path, stat] : catalog)
        records::put_catalog_path(w, file_id, path, stat);
    for (auto ext : tier_extensions(config))
        records::put_manifest(w, file_id, ext, config.params_hash(ext));
    return acc.statistics;
}

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

void observe_stats(index::extensions::ChunkDimensionStats& ds,
                   const std::string& text, const char* type) {
    if (ds.value_type == "mixed") return;
    const std::string joined = join_value_types(ds.value_type, type);
    if (joined != ds.value_type) ds.value_type = joined;
    if (joined == "mixed") {
        ds.min_value.clear();
        ds.max_value.clear();
        return;
    }
    if (ds.min_value.empty() || index::extensions::dimension_value_less(
                                    text, ds.min_value, ds.value_type))
        ds.min_value = text;
    if (ds.max_value.empty() || index::extensions::dimension_value_less(
                                    ds.max_value, text, ds.value_type))
        ds.max_value = text;
}

}  // namespace

std::span<const std::string_view> BloomCore::fixed_dimension_names() {
    return FIXED_DIM_NAMES;
}

void BloomCore::observe_value(ChunkDimensionStats& stats, std::int64_t value) {
    observe_stats(stats, std::to_string(value), "int");
}

void BloomCore::observe_value(ChunkDimensionStats& stats, double value) {
    observe_stats(stats, index::extensions::canonical_number_text(value),
                  "double");
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
    const std::string text = index::extensions::canonical_number_text(value);
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

void BloomCore::observe_metadata_path(ChunkState& chunk,
                                      std::string_view path) {
    index::store::add_metadata_value(chunk.metadata.paths, path);
}

void BloomCore::observe_metadata(ChunkState& chunk,
                                 std::string_view record_name) {
    ++chunk.metadata.records;
    if (record_name != "HH" && record_name != "FH" && record_name != "SH")
        ++chunk.metadata.context;
    index::store::add_metadata_value(chunk.metadata.names, record_name);
}

void BloomCore::observe_data(ChunkState& chunk, PidTidCache& cache,
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
         index::extensions::dimension_value_less(src.min_value, dst.min_value,
                                                 dst.value_type)))
        dst.min_value = src.min_value;
    if (dst.max_value.empty() ||
        (!src.max_value.empty() &&
         index::extensions::dimension_value_less(dst.max_value, src.max_value,
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

    dst.metadata.records += src.metadata.records;
    dst.metadata.context += src.metadata.context;
    for (auto list : {&index::store::ChunkMetadata::names,
                      &index::store::ChunkMetadata::paths}) {
        auto& into = dst.metadata.*list;
        const auto& from = src.metadata.*list;
        if (!from) into.reset();
        if (!into) continue;
        for (const auto& v : *from) index::store::add_metadata_value(into, v);
    }
    dst.events_processed += src.events_processed;
}

std::vector<index::store::IndexExtension> BloomCore::tier_extensions(
    const ChunkIndexerConfig& config) {
    std::vector<IndexExtension> exts;
    for (auto ext : index::store::ALL_EXTENSIONS)
        if ((index::store::PRUNING_EXTENSIONS.has(ext) &&
             config.extensions.has(ext)) ||
            ext == IndexExtension::STATS || ext == IndexExtension::CATALOG ||
            ext == IndexExtension::METADATA)
            exts.push_back(ext);
    return exts;
}

}  // namespace dftracer::utils::index::schemas::dft
