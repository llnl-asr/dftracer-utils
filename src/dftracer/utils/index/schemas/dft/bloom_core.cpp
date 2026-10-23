#include <dftracer/utils/dstd/transparent_string_hash.h>
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

constexpr std::array<std::string_view, BloomCore::BfCount> FIXED_BLOOM_NAMES = {
    DIM_NAME, DIM_CAT, DIM_PID, DIM_TID, DIM_HHASH, DIM_FHASH, DIM_SHASH};

constexpr std::array<std::string_view, BloomCore::FdCount> FIXED_DIM_NAMES = {
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
        w, IndexExtension::Bloom, file_id, path, checkpoint_idx,
        kinds::encode_bloom(blob,
                            static_cast<std::uint64_t>(bf.num_entries())));
}

void put_file_bloom(index::store::IndexWrite& w, int file_id,
                    std::string_view path, const ScalableBloomFilter& bf,
                    std::vector<unsigned char>& blob) {
    bf.serialize_into(blob);
    records::put_path_file(w, IndexExtension::Bloom, file_id, path,
                           kinds::encode_bloom(blob, static_cast<std::uint64_t>(
                                                         bf.num_entries())));
}

// One dimension's statistics as a zonemap and, when its value dictionary fit
// the cap, counts. `observed` is the chunk's data events. An extra dimension
// without a value gets an absence zone.
void put_dimension(index::store::IndexWrite& w, int file_id,
                   std::uint64_t checkpoint_idx,
                   const BloomCore::ChunkDimensionStats& ds,
                   std::uint64_t observed,
                   const BloomCore::ChunkIndexerConfig& config,
                   const BloomCore::ChunkStatistics& stats, bool extra) {
    const bool zones = config.extensions.has(IndexExtension::Zonemap);
    const bool counts = config.extensions.has(IndexExtension::Counts);
    if (!zones && !counts) return;
    // dftracer's ts and dur are on every data event; other dimensions count.
    const bool always_present =
        config.fixed_dimensions &&
        (ds.dimension == DIM_TS || ds.dimension == DIM_DUR);
    const auto compressed = ds.compress_value_counts(config.value_counts_cap);
    const std::uint64_t present = always_present ? observed : ds.present;
    const bool absent = extra && present == 0;
    if (zones && (absent || !ds.min_value.empty() || !ds.max_value.empty())) {
        kinds::Zone zone;
        zone.value_type = ds.value_type;
        zone.min = ds.min_value;
        zone.max = ds.max_value;
        zone.observed = observed;
        zone.present = present;
        if (ds.dimension == DIM_TS && !stats.timestamp_histogram.empty())
            zone.histogram = stats.timestamp_histogram;
        records::put_path_granule(w, IndexExtension::Zonemap, file_id,
                                  ds.dimension, checkpoint_idx,
                                  kinds::encode_zone(zone));
    }
    if (counts && compressed)
        records::put_path_granule(
            w, IndexExtension::Counts, file_id, ds.dimension, checkpoint_idx,
            kinds::encode_counts({observed, *compressed}));
}

}  // namespace

BloomCore::FileAccumulator::FileAccumulator(const ChunkIndexerConfig& config)
    : config_(&config) {
    fixed_blooms.reserve(BfCount);
    for (std::size_t b = 0; b < BfCount; ++b)
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
    const bool blooms = m.has(IndexExtension::Bloom);
    const bool zones = m.has(IndexExtension::Zonemap);
    const bool fixed = config.fixed_dimensions;
    const auto observed = chunk.statistics.total_events;
    auto& blob = acc.blob;

    for (std::size_t b = 0; fixed && blooms && b < BfCount; ++b) {
        put_bloom(w, file_id, checkpoint_idx, FIXED_BLOOM_NAMES[b],
                  chunk.fixed_blooms[b], blob);
        acc.fixed_blooms[b].merge_from(chunk.fixed_blooms[b]);
    }
    for (std::size_t e = 0;
         blooms && e < extra_dims.size() && e < chunk.extra_dim_stats.size();
         ++e) {
        if (chunk.extra_dim_stats[e].present == 0) continue;
        auto& file_bloom = acc.extra(extra_dims[e]);
        if (e < chunk.extra_bloom_skip.size() && chunk.extra_bloom_skip[e]) {
            file_bloom.skip = true;
            continue;
        }
        if (e < chunk.extra_blooms.size()) {
            put_bloom(w, file_id, checkpoint_idx, extra_dims[e],
                      chunk.extra_blooms[e], blob);
            file_bloom.bloom.merge_from(chunk.extra_blooms[e]);
            continue;
        }
        const auto& values = chunk.extra_values[e];
        // One more than the values, so the last add opens no second level.
        ScalableBloomFilter bf(values.size() + 1, config.false_positive_rate);
        for (std::string_view v : values) {
            bf.add(v);
            file_bloom.bloom.add(v);
        }
        put_bloom(w, file_id, checkpoint_idx, extra_dims[e], bf, blob);
    }

    records::put_chunk_statistics(w, file_id, checkpoint_idx, chunk.statistics);
    records::put_chunk_metadata(w, file_id, checkpoint_idx, chunk.metadata);
    acc.statistics.merge_from(chunk.statistics);
    ++acc.chunks;

    for (const auto& ds : chunk.fixed_dim_stats)
        if (fixed)
            put_dimension(w, file_id, checkpoint_idx, ds, observed, config,
                          chunk.statistics, false);
    for (const auto& ds : chunk.extra_dim_stats)
        put_dimension(w, file_id, checkpoint_idx, ds, observed, config,
                      chunk.statistics, true);
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
        records::put_path_granule(w, IndexExtension::Zonemap, file_id, DIM_TE,
                                  checkpoint_idx, kinds::encode_zone(te));
    }

    // Postings hash the value, so postings written by concurrent workers or
    // sinks agree.
    if (m.has(IndexExtension::Postings))
        for (const auto& [name, _] : chunk.statistics.name_counts)
            records::put_posting_granule(
                w, file_id, DIM_NAME, kinds::value_hash(name), checkpoint_idx);
}

void BloomCore::write_absent_extra(index::store::IndexWrite& w, int file_id,
                                   std::uint64_t checkpoint_idx,
                                   std::string_view dim, std::uint64_t observed,
                                   const ChunkIndexerConfig& config) {
    ChunkDimensionStats ds;
    ds.dimension = std::string(dim);
    ds.value_type.clear();
    put_dimension(w, file_id, checkpoint_idx, ds, observed, config, {}, true);
}

BloomCore::ChunkStatistics BloomCore::finish_file(
    index::store::IndexWrite& w, int file_id, FileAccumulator& acc,
    const ChunkIndexerConfig& config,
    const std::vector<std::string>& extra_dims, const Catalog& catalog) {
    const auto& m = config.extensions;
    const bool blooms = m.has(IndexExtension::Bloom);
    const bool zones = m.has(IndexExtension::Zonemap);
    const bool fixed = config.fixed_dimensions;
    auto& blob = acc.blob;

    for (std::size_t b = 0; fixed && blooms && b < BfCount; ++b) {
        put_file_bloom(w, file_id, FIXED_BLOOM_NAMES[b], acc.fixed_blooms[b],
                       blob);
        records::put_path(w, IndexExtension::Bloom, file_id,
                          FIXED_BLOOM_NAMES[b]);
    }
    for (std::size_t e = 0; blooms && e < extra_dims.size(); ++e) {
        auto& file_bloom = acc.extra(extra_dims[e]);
        if (!file_bloom.skip)
            put_file_bloom(w, file_id, extra_dims[e], file_bloom.bloom, blob);
        records::put_path(w, IndexExtension::Bloom, file_id, extra_dims[e]);
    }
    if (fixed && zones)
        records::put_path(w, IndexExtension::Zonemap, file_id, DIM_TE);
    for (auto ext : {IndexExtension::Zonemap, IndexExtension::Counts}) {
        if (!m.has(ext)) continue;
        for (auto dim : FIXED_DIM_NAMES)
            if (fixed) records::put_path(w, ext, file_id, dim);
        for (const auto& dim : extra_dims)
            records::put_path(w, ext, file_id, dim);
    }

    records::put_file_scalar_stats(w, file_id, acc.statistics, acc.chunks);
    records::put_file_pid_tid_counts(w, file_id, acc.statistics.pid_tid_counts);

    if (m.has(IndexExtension::Postings)) {
        records::put_path(w, IndexExtension::Postings, file_id, DIM_NAME);
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
    const bool num_a = a == "int" || a == "uint" || a == "double";
    const bool num_b = b == "int" || b == "uint" || b == "double";
    return num_a && num_b ? "double" : "mixed";
}

// Counts a value of `type` and joins its type in; false when the dimension
// is mixed and keeps no bounds.
bool observe_type(index::extensions::ChunkDimensionStats& ds,
                  const char* type) {
    ++ds.present;
    // The value types ("string", "uint", "int", "double", "mixed") start with
    // distinct letters, so the first byte tells them apart.
    if (!ds.value_type.empty()) {
        if (ds.value_type[0] == 'm') return false;
        if (ds.value_type[0] == type[0]) return true;
    }
    const std::string joined = join_value_types(ds.value_type, type);
    if (joined != ds.value_type) ds.value_type = joined;
    if (joined == "mixed") {
        ds.min_value.clear();
        ds.max_value.clear();
        return false;
    }
    return true;
}

void observe_bounds(index::extensions::ChunkDimensionStats& ds,
                    std::string_view text) {
    if (ds.min_value.empty() || index::extensions::dimension_value_less(
                                    text, ds.min_value, ds.value_type))
        ds.min_value = text;
    if (ds.max_value.empty() || index::extensions::dimension_value_less(
                                    ds.max_value, text, ds.value_type))
        ds.max_value = text;
}

void observe_stats(index::extensions::ChunkDimensionStats& ds,
                   std::string_view text, const char* type) {
    if (observe_type(ds, type)) observe_bounds(ds, text);
}

std::string number_text(std::int64_t v) { return std::to_string(v); }
std::string number_text(std::uint64_t v) { return std::to_string(v); }
std::string number_text(double v) {
    return index::extensions::canonical_number_text(v);
}

// parse(text(v)) grows with v and parse(text(min)) is min, so the text path's
// parse(text(v)) < min holds only when v < min. A value that does not move a
// bound as a number is never formatted; one that does is checked as text.
template <class T>
void observe_number(index::extensions::ChunkDimensionStats& ds,
                    BloomCore::NumericBounds& b, T v, const char* type) {
    if (!observe_type(ds, type)) {
        b.valid = false;
        return;
    }
    if (!b.valid) {
        b.min.reset();
        b.max.reset();
        if (!ds.min_value.empty()) b.min = duql::parse_number(ds.min_value);
        if (!ds.max_value.empty()) b.max = duql::parse_number(ds.max_value);
        if ((!ds.min_value.empty() && !b.min) ||
            (!ds.max_value.empty() && !b.max)) {
            observe_bounds(ds, number_text(v));
            return;
        }
        b.valid = true;
    }
    const auto less = [](const duql::Number& x, const duql::Number& y) {
        return duql::compare_numbers(x, y).value_or(0) < 0;
    };
    const duql::Number n{v};
    if (b.min && !less(n, *b.min) && b.max && !less(*b.max, n)) return;
    const std::string text = number_text(v);
    const auto parsed = duql::parse_number(text);
    if (!parsed) {
        b.valid = false;
        observe_bounds(ds, text);
        return;
    }
    const bool lo = !b.min || less(*parsed, *b.min);
    const bool hi = !b.max || less(*b.max, *parsed);
    if (lo) {
        ds.min_value = text;
        b.min = parsed;
    }
    if (hi) {
        ds.max_value = text;
        b.max = parsed;
    }
}

}  // namespace

std::span<const std::string_view> BloomCore::fixed_dimension_names() {
    return FIXED_DIM_NAMES;
}

void BloomCore::observe_value(ChunkDimensionStats& stats, NumericBounds& bounds,
                              std::int64_t value) {
    observe_number(stats, bounds, value, "int");
}

void BloomCore::observe_value(ChunkDimensionStats& stats, NumericBounds& bounds,
                              std::uint64_t value) {
    observe_number(stats, bounds, value, "uint");
}

void BloomCore::observe_value(ChunkDimensionStats& stats, NumericBounds& bounds,
                              double value) {
    observe_number(stats, bounds, value, "double");
}

void BloomCore::observe_value(ChunkDimensionStats& stats, std::int64_t value) {
    observe_stats(stats, std::to_string(value), "int");
}

void BloomCore::observe_value(ChunkDimensionStats& stats, std::uint64_t value) {
    observe_stats(stats, std::to_string(value), "uint");
}

void BloomCore::observe_value(ChunkDimensionStats& stats, double value) {
    observe_stats(stats, index::extensions::canonical_number_text(value),
                  "double");
}

void BloomCore::observe_value(ChunkDimensionStats& stats,
                              std::string_view value) {
    observe_stats(stats, value, "string");
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e,
                              std::int64_t value) {
    const std::string text = std::to_string(value);
    chunk.extra_blooms[e].add(text);
    observe_stats(chunk.extra_dim_stats[e], text, "int");
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e,
                              std::uint64_t value) {
    const std::string text = std::to_string(value);
    chunk.extra_blooms[e].add(text);
    observe_stats(chunk.extra_dim_stats[e], text, "uint");
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e, double value) {
    const std::string text = index::extensions::canonical_number_text(value);
    chunk.extra_blooms[e].add(text);
    observe_stats(chunk.extra_dim_stats[e], text, "double");
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e,
                              std::string_view value) {
    chunk.extra_blooms[e].add(value);
    observe_stats(chunk.extra_dim_stats[e], value, "string");
}

void BloomCore::init_chunk_state(ChunkState& chunk,
                                 const ChunkIndexerConfig& config,
                                 const std::vector<std::string>& extra_dims) {
    for (std::size_t b = 0; b < BfCount; ++b) {
        chunk.fixed_blooms[b] = ScalableBloomFilter(
            config.expected_entries_per_chunk, config.false_positive_rate);
    }
    for (std::size_t d = 0; d < FdCount; ++d) {
        auto& ds = chunk.fixed_dim_stats[d];
        ds.dimension = std::string(FIXED_DIM_NAMES[d]);
        ds.value_type = (d == FdPid || d == FdTid || d == FdTs || d == FdDur)
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
    chunk.extra_values.assign(extra_dims.size(), {});
    chunk.extra_bloom_skip.assign(extra_dims.size(), 0);
}

void BloomCore::observe_metadata_path(ChunkState& chunk,
                                      std::string_view path) {
    index::store::add_metadata_value(chunk.metadata.paths, path);
}

void BloomCore::observe_metadata(ChunkState& chunk,
                                 std::string_view record_name) {
    ++chunk.metadata.records;
    index::store::add_metadata_value(chunk.metadata.names, record_name);
}

BloomCore::EventCache::NameSlot& BloomCore::EventCache::name_slot(
    std::uint32_t name_id, std::uint32_t cat_id) {
    const std::uint64_t key = (std::uint64_t{name_id} << 32) | cat_id;
    NameSlot& slot = names_[(key * 0x9E3779B97F4A7C15ULL) >> 56];
    if (slot.epoch != epoch_ || slot.key != key) {
        slot = NameSlot{};
        slot.key = key;
        slot.epoch = epoch_;
    }
    return slot;
}

BloomCore::EventCache::PidSlot& BloomCore::EventCache::pid_slot(
    std::uint64_t pid, std::uint64_t tid) {
    PidSlot& slot =
        pids_[((pid * 0x9E3779B97F4A7C15ULL) ^ (tid * 0xC2B2AE3D27D4EB4FULL)) >>
              56];
    if (slot.epoch != epoch_ || slot.pid != pid || slot.tid != tid) {
        slot = PidSlot{};
        slot.pid = pid;
        slot.tid = tid;
        slot.epoch = epoch_;
        slot.pid_len = static_cast<std::uint8_t>(
            std::to_chars(slot.pid_buf, slot.pid_buf + sizeof(slot.pid_buf),
                          pid)
                .ptr -
            slot.pid_buf);
        slot.tid_len = static_cast<std::uint8_t>(
            std::to_chars(slot.tid_buf, slot.tid_buf + sizeof(slot.tid_buf),
                          tid)
                .ptr -
            slot.tid_buf);
    }
    return slot;
}

void BloomCore::observe_data(ChunkState& chunk, EventCache& cache,
                             std::uint32_t name_id, std::uint32_t cat_id,
                             std::string_view name, std::string_view cat,
                             std::uint64_t pid, std::uint64_t tid,
                             std::uint64_t ts, std::uint64_t dur, bool has_dur,
                             std::string_view hhash, std::string_view fhash,
                             std::string_view shash) {
    EventCache::NameSlot& ns = cache.name_slot(name_id, cat_id);
    EventCache::PidSlot& ps = cache.pid_slot(pid, tid);
    chunk.statistics.update_from_event(ns.stats, ps.stats, name, cat, pid, tid,
                                       ts, dur, has_dur);

    // A value already in a chunk's bloom adds nothing, so a slot remembers
    // that it was added and skips the probe.
    auto observe_known = [&chunk](int bloom_idx, std::size_t dim_idx,
                                  std::string_view val, bool& in_bloom,
                                  std::uint32_t& dim_slot) {
        if (val.empty()) return;
        if (bloom_idx >= 0 && !in_bloom) {
            chunk.fixed_blooms[bloom_idx].add(val);
            in_bloom = true;
        }
        chunk.fixed_dim_stats[dim_idx].observe(val, dim_slot);
    };
    auto observe_fixed = [&chunk](int bloom_idx, std::size_t dim_idx,
                                  std::string_view val) {
        if (val.empty()) return;
        if (bloom_idx >= 0) chunk.fixed_blooms[bloom_idx].add(val);
        chunk.fixed_dim_stats[dim_idx].observe(val);
    };

    observe_known(BfName, FdName, name, ns.name_in_bloom, ns.name_dim);
    observe_known(BfCat, FdCat, cat, ns.cat_in_bloom, ns.cat_dim);

    const std::string_view pid_sv(ps.pid_buf, ps.pid_len);
    const std::string_view tid_sv(ps.tid_buf, ps.tid_len);
    observe_known(BfPid, FdPid, pid_sv, ps.pid_in_bloom, ps.pid_dim);
    observe_known(BfTid, FdTid, tid_sv, ps.tid_in_bloom, ps.tid_dim);

    char pt_buf[52];
    std::memcpy(pt_buf, ps.pid_buf, ps.pid_len);
    pt_buf[ps.pid_len] = ':';
    std::memcpy(pt_buf + ps.pid_len + 1, ps.tid_buf, ps.tid_len);
    const std::string_view pt_sv(pt_buf, ps.pid_len + 1 + ps.tid_len);
    bool no_bloom = false;
    observe_known(-1, FdPidTid, pt_sv, no_bloom, ps.pid_tid_dim);

    chunk.fixed_dim_stats[FdTs].observe_range_only(ts);
    chunk.fixed_dim_stats[FdDur].observe_range_only(dur);

    observe_fixed(BfHhash, FdHhash, hhash);
    observe_fixed(BfFhash, FdFhash, fhash);
    observe_fixed(BfShash, FdShash, shash);

    chunk.events_processed++;
}

void BloomCore::merge_dimension_stats(ChunkDimensionStats& dst,
                                      ChunkDimensionStats& src) {
    dst.present += src.present;
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
    for (std::size_t b = 0; b < BfCount; ++b) {
        dst.fixed_blooms[b].merge_from(src.fixed_blooms[b]);
    }
    for (std::size_t e = 0;
         e < src.extra_blooms.size() && e < dst.extra_blooms.size(); ++e) {
        dst.extra_blooms[e].merge_from(src.extra_blooms[e]);
    }

    auto merge_dim = [](ChunkDimensionStats& dds, ChunkDimensionStats& sds) {
        merge_dimension_stats(dds, sds);
    };
    for (std::size_t d = 0; d < FdCount; ++d) {
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
            ext == IndexExtension::Stats || ext == IndexExtension::Catalog ||
            ext == IndexExtension::Metadata)
            exts.push_back(ext);
    return exts;
}

}  // namespace dftracer::utils::index::schemas::dft
