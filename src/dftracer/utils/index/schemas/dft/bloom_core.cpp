// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/schemas/dft/bloom_core.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/tables.h>

#include <array>
#include <charconv>
#include <cstring>

using dftracer::utils::index::extensions::ScalableBloomFilter;
namespace dftracer::utils::index::schemas::dft {

namespace {

constexpr dstd::StringView DIM_NAME = "name";
constexpr dstd::StringView DIM_CAT = "cat";
constexpr dstd::StringView DIM_PID = "pid";
constexpr dstd::StringView DIM_TID = "tid";
constexpr dstd::StringView DIM_PID_TID = "pid_tid";
constexpr dstd::StringView DIM_HHASH = "hhash";
constexpr dstd::StringView DIM_FHASH = "fhash";
constexpr dstd::StringView DIM_SHASH = "shash";
constexpr dstd::StringView DIM_TS = "ts";
constexpr dstd::StringView DIM_DUR = "dur";
constexpr dstd::StringView DIM_TE = "te";

constexpr std::array<dstd::StringView, BloomCore::BfCount> FIXED_BLOOM_NAMES = {
    DIM_NAME, DIM_CAT, DIM_PID, DIM_TID, DIM_HHASH, DIM_FHASH, DIM_SHASH};

constexpr std::array<dstd::StringView, BloomCore::FdCount> FIXED_DIM_NAMES = {
    DIM_NAME,  DIM_CAT,   DIM_PID,   DIM_TID, DIM_PID_TID,
    DIM_HHASH, DIM_FHASH, DIM_SHASH, DIM_TS,  DIM_DUR};

using index::store::IndexExtension;

// The build a file's tier records go into.
class Tier {
   public:
    explicit Tier(index::FileBuild& build) : build_(build) {}

    void path(IndexExtension ext, dstd::StringView path) {
        index::put_path(build_, ext, path);
    }
    void path_granule(IndexExtension ext, dstd::StringView path,
                      std::uint64_t granule, dstd::StringView payload) {
        index::put_path_granule(build_, ext, path, granule, payload);
    }
    void path_file(IndexExtension ext, dstd::StringView path,
                   dstd::StringView payload) {
        index::put_path_file(build_, ext, path, payload);
    }
    void posting(dstd::StringView path, std::uint64_t value_hash) {
        index::put_posting(build_, path, value_hash);
    }
    void posting_granule(dstd::StringView path, std::uint64_t value_hash,
                         std::uint64_t granule) {
        index::put_posting_granule(build_, path, value_hash, granule);
    }
    void chunk_statistics(std::uint64_t checkpoint_idx,
                          const BloomCore::ChunkStatistics& stats) {
        index::put_chunk_statistics(build_, checkpoint_idx, stats);
    }
    void chunk_metadata(std::uint64_t checkpoint_idx,
                        const index::store::ChunkMetadata& metadata) {
        index::put_chunk_metadata(build_, checkpoint_idx, metadata);
    }
    void scalar_stats(const BloomCore::ChunkStatistics& stats,
                      std::uint64_t chunks) {
        index::put_file_scalar_stats(build_, stats, chunks);
    }
    void pid_tid_counts(const dstd::StringViewMap<std::uint64_t>& counts) {
        index::put_file_pid_tid_counts(build_, counts);
    }
    void catalog_path(dstd::StringView path,
                      const index::store::PathStat& stat) {
        index::put_catalog_path(build_, path, stat);
    }
    void finish(IndexExtension ext, std::uint64_t params_hash) {
        build_.finish(ext, params_hash);
    }

   private:
    index::FileBuild& build_;
};

void put_bloom(Tier& t, std::uint64_t checkpoint_idx, dstd::StringView path,
               const ScalableBloomFilter& bf,
               dstd::Vector<unsigned char>& blob) {
    bf.serialize_into(blob);
    t.path_granule(IndexExtension::Bloom, path, checkpoint_idx,
                   index::extensions::kinds::encode_bloom(
                       blob, static_cast<std::uint64_t>(bf.entry_count())));
}

void put_file_bloom(Tier& t, dstd::StringView path,
                    const ScalableBloomFilter& bf,
                    dstd::Vector<unsigned char>& blob) {
    bf.serialize_into(blob);
    t.path_file(IndexExtension::Bloom, path,
                index::extensions::kinds::encode_bloom(
                    blob, static_cast<std::uint64_t>(bf.entry_count())));
}

// One dimension's statistics as a zonemap and, when its value dictionary fit
// the cap, counts. `observed` is the chunk's data events. An extra dimension
// without a value gets an absence zone.
void put_dimension(Tier& t, std::uint64_t checkpoint_idx,
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
        index::extensions::kinds::Zone zone;
        zone.value_type = ds.value_type;
        zone.min = ds.min_value;
        zone.max = ds.max_value;
        zone.observed = observed;
        zone.present = present;
        if (ds.dimension == DIM_TS && !stats.timestamp_histogram.empty())
            zone.histogram = stats.timestamp_histogram;
        t.path_granule(IndexExtension::Zonemap, ds.dimension, checkpoint_idx,
                       index::extensions::kinds::encode_zone(zone));
    }
    if (counts && compressed)
        t.path_granule(
            IndexExtension::Counts, ds.dimension, checkpoint_idx,
            index::extensions::kinds::encode_counts({observed, *compressed}));
}

void write_chunk_to(Tier& t, std::uint64_t checkpoint_idx,
                    const BloomCore::ChunkState& chunk,
                    const dstd::Vector<dstd::String>& extra_dims,
                    const BloomCore::ChunkIndexerConfig& config,
                    BloomCore::FileAccumulator& acc) {
    const auto& m = config.extensions;
    const bool blooms = m.has(IndexExtension::Bloom);
    const bool zones = m.has(IndexExtension::Zonemap);
    const bool fixed = config.fixed_dimensions;
    const auto observed = chunk.statistics.total_events;
    auto& blob = acc.blob;

    for (std::size_t b = 0; fixed && blooms && b < BloomCore::BfCount; ++b) {
        put_bloom(t, checkpoint_idx, FIXED_BLOOM_NAMES[b],
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
            put_bloom(t, checkpoint_idx, extra_dims[e], chunk.extra_blooms[e],
                      blob);
            file_bloom.bloom.merge_from(chunk.extra_blooms[e]);
            continue;
        }
        const auto& values = chunk.extra_values[e];
        // One more than the values, so the last add opens no second level.
        ScalableBloomFilter bf(values.size() + 1, config.false_positive_rate);
        for (dstd::StringView v : values) {
            bf.add(v);
            file_bloom.bloom.add(v);
        }
        put_bloom(t, checkpoint_idx, extra_dims[e], bf, blob);
    }

    t.chunk_statistics(checkpoint_idx, chunk.statistics);
    t.chunk_metadata(checkpoint_idx, chunk.metadata);
    acc.statistics.merge_from(chunk.statistics);
    ++acc.chunks;

    for (const auto& ds : chunk.fixed_dim_stats)
        if (fixed)
            put_dimension(t, checkpoint_idx, ds, observed, config,
                          chunk.statistics, false);
    for (const auto& ds : chunk.extra_dim_stats)
        put_dimension(t, checkpoint_idx, ds, observed, config, chunk.statistics,
                      true);
    // Event end (ts + dur): the time bounds a time range prunes with. Its
    // min is the first start, a lower bound of every end.
    if (zones && observed > 0 &&
        chunk.statistics.min_timestamp_us <=
            chunk.statistics.max_timestamp_us) {
        index::extensions::kinds::Zone te;
        te.value_type = "uint";
        te.min = dstd::to_string(chunk.statistics.min_timestamp_us);
        te.max = dstd::to_string(chunk.statistics.max_timestamp_us);
        te.observed = observed;
        te.present = observed;
        t.path_granule(IndexExtension::Zonemap, DIM_TE, checkpoint_idx,
                       index::extensions::kinds::encode_zone(te));
    }

    // Postings hash the value, so postings written by concurrent workers or
    // sinks agree.
    if (m.has(IndexExtension::Postings))
        for (const auto& [name, _] : chunk.statistics.name_counts)
            t.posting_granule(DIM_NAME,
                              index::extensions::kinds::value_hash(name),
                              checkpoint_idx);
}

void write_absent_extra_to(Tier& t, std::uint64_t checkpoint_idx,
                           dstd::StringView dim, std::uint64_t observed,
                           const BloomCore::ChunkIndexerConfig& config) {
    BloomCore::ChunkDimensionStats ds;
    ds.dimension = dstd::String(dim);
    ds.value_type.clear();
    put_dimension(t, checkpoint_idx, ds, observed, config, {}, true);
}

BloomCore::ChunkStatistics finish_file_to(
    Tier& t, BloomCore::FileAccumulator& acc,
    const BloomCore::ChunkIndexerConfig& config,
    const dstd::Vector<dstd::String>& extra_dims,
    const BloomCore::Catalog& catalog) {
    const auto& m = config.extensions;
    const bool blooms = m.has(IndexExtension::Bloom);
    const bool zones = m.has(IndexExtension::Zonemap);
    const bool fixed = config.fixed_dimensions;
    auto& blob = acc.blob;

    for (std::size_t b = 0; fixed && blooms && b < BloomCore::BfCount; ++b) {
        put_file_bloom(t, FIXED_BLOOM_NAMES[b], acc.fixed_blooms[b], blob);
        t.path(IndexExtension::Bloom, FIXED_BLOOM_NAMES[b]);
    }
    for (std::size_t e = 0; blooms && e < extra_dims.size(); ++e) {
        auto& file_bloom = acc.extra(extra_dims[e]);
        if (!file_bloom.skip)
            put_file_bloom(t, extra_dims[e], file_bloom.bloom, blob);
        t.path(IndexExtension::Bloom, extra_dims[e]);
    }
    if (fixed && zones) t.path(IndexExtension::Zonemap, DIM_TE);
    for (auto ext : {IndexExtension::Zonemap, IndexExtension::Counts}) {
        if (!m.has(ext)) continue;
        for (auto dim : FIXED_DIM_NAMES)
            if (fixed) t.path(ext, dim);
        for (const auto& dim : extra_dims) t.path(ext, dim);
    }

    t.scalar_stats(acc.statistics, acc.chunks);
    t.pid_tid_counts(acc.statistics.pid_tid_counts);

    if (m.has(IndexExtension::Postings)) {
        t.path(IndexExtension::Postings, DIM_NAME);
        for (const auto& [name, _] : acc.statistics.name_counts)
            t.posting(DIM_NAME, index::extensions::kinds::value_hash(name));
    }

    for (const auto& [path, stat] : catalog) t.catalog_path(path, stat);
    for (auto ext : BloomCore::tier_extensions(config))
        t.finish(ext, config.params_hash(ext));
    return acc.statistics;
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
    dstd::StringView dim) {
    auto it = extras.find(dim);
    if (it == extras.end())
        it = extras
                 .emplace(dstd::String(dim),
                          ExtraBloom{ScalableBloomFilter(
                                         config_->expected_entries_per_chunk,
                                         config_->false_positive_rate),
                                     false})
                 .first;
    return it->second;
}

void BloomCore::write_chunk(index::FileBuild& build,
                            std::uint64_t checkpoint_idx,
                            const ChunkState& chunk,
                            const dstd::Vector<dstd::String>& extra_dims,
                            const ChunkIndexerConfig& config,
                            FileAccumulator& acc) {
    Tier tier(build);
    write_chunk_to(tier, checkpoint_idx, chunk, extra_dims, config, acc);
}

void BloomCore::write_absent_extra(index::FileBuild& build,
                                   std::uint64_t checkpoint_idx,
                                   dstd::StringView dim, std::uint64_t observed,
                                   const ChunkIndexerConfig& config) {
    Tier tier(build);
    write_absent_extra_to(tier, checkpoint_idx, dim, observed, config);
}

BloomCore::ChunkStatistics BloomCore::finish_file(
    index::FileBuild& build, FileAccumulator& acc,
    const ChunkIndexerConfig& config,
    const dstd::Vector<dstd::String>& extra_dims, const Catalog& catalog) {
    Tier tier(build);
    return finish_file_to(tier, acc, config, extra_dims, catalog);
}

BloomCore::ChunkState::ChunkState() = default;

namespace {

// The type of a dimension holding values of both types; "" is no value yet.
dstd::String join_value_types(const dstd::String& a, const dstd::String& b) {
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
    // The value types start with distinct letters, so the first byte tells them
    // apart.
    if (!ds.value_type.empty()) {
        if (ds.value_type[0] == 'm') return false;
        if (ds.value_type[0] == type[0]) return true;
    }
    const dstd::String joined = join_value_types(ds.value_type, type);
    if (joined != ds.value_type) ds.value_type = joined;
    if (joined == "mixed") {
        ds.min_value.clear();
        ds.max_value.clear();
        return false;
    }
    return true;
}

void observe_bounds(index::extensions::ChunkDimensionStats& ds,
                    dstd::StringView text) {
    if (ds.min_value.empty() || index::extensions::dimension_value_less(
                                    text, ds.min_value, ds.value_type))
        ds.min_value = text;
    if (ds.max_value.empty() || index::extensions::dimension_value_less(
                                    ds.max_value, text, ds.value_type))
        ds.max_value = text;
}

void observe_stats(index::extensions::ChunkDimensionStats& ds,
                   dstd::StringView text, const char* type) {
    if (observe_type(ds, type)) observe_bounds(ds, text);
}

dstd::String number_text(std::int64_t v) { return dstd::to_string(v); }
dstd::String number_text(std::uint64_t v) { return dstd::to_string(v); }
dstd::String number_text(double v) {
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
    const dstd::String text = number_text(v);
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

dstd::Span<const dstd::StringView> BloomCore::fixed_dimension_names() {
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
    observe_stats(stats, dstd::to_string(value), "int");
}

void BloomCore::observe_value(ChunkDimensionStats& stats, std::uint64_t value) {
    observe_stats(stats, dstd::to_string(value), "uint");
}

void BloomCore::observe_value(ChunkDimensionStats& stats, double value) {
    observe_stats(stats, index::extensions::canonical_number_text(value),
                  "double");
}

void BloomCore::observe_value(ChunkDimensionStats& stats,
                              dstd::StringView value) {
    observe_stats(stats, value, "string");
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e,
                              std::int64_t value) {
    const dstd::String text = dstd::to_string(value);
    chunk.extra_blooms[e].add(text);
    observe_stats(chunk.extra_dim_stats[e], text, "int");
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e,
                              std::uint64_t value) {
    const dstd::String text = dstd::to_string(value);
    chunk.extra_blooms[e].add(text);
    observe_stats(chunk.extra_dim_stats[e], text, "uint");
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e, double value) {
    const dstd::String text = index::extensions::canonical_number_text(value);
    chunk.extra_blooms[e].add(text);
    observe_stats(chunk.extra_dim_stats[e], text, "double");
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e,
                              dstd::StringView value) {
    chunk.extra_blooms[e].add(value);
    observe_stats(chunk.extra_dim_stats[e], value, "string");
}

void BloomCore::init_chunk_state(ChunkState& chunk,
                                 const ChunkIndexerConfig& config,
                                 const dstd::Vector<dstd::String>& extra_dims) {
    for (std::size_t b = 0; b < BfCount; ++b) {
        chunk.fixed_blooms[b] = ScalableBloomFilter(
            config.expected_entries_per_chunk, config.false_positive_rate);
    }
    for (std::size_t d = 0; d < FdCount; ++d) {
        auto& ds = chunk.fixed_dim_stats[d];
        ds.dimension = dstd::String(FIXED_DIM_NAMES[d]);
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
                                      dstd::StringView path) {
    index::store::add_metadata_value(chunk.metadata.paths, path);
}

void BloomCore::observe_metadata(ChunkState& chunk,
                                 dstd::StringView record_name) {
    ++chunk.metadata.records;
    index::store::add_metadata_value(chunk.metadata.names, record_name);
}

BloomCore::EventCache::NameSlot& BloomCore::EventCache::name_slot(
    std::uint32_t name_id, std::uint32_t cat_id) {
    const std::uint64_t key = (std::uint64_t{name_id} << 32) | cat_id;
    NameSlot& slot = names_[(key * dstd::hash::GOLDEN_RATIO) >> 56];
    if (slot.epoch != epoch_ || slot.key != key) {
        slot = NameSlot{};
        slot.key = key;
        slot.epoch = epoch_;
    }
    return slot;
}

BloomCore::EventCache::PidSlot& BloomCore::EventCache::pid_slot(
    std::uint64_t pid, std::uint64_t tid) {
    // 0xC2B2AE3D27D4EB4F is xxHash's PRIME64_2 (Yann Collet, BSD-2-Clause,
    // https://github.com/Cyan4973/xxHash).
    PidSlot& slot = pids_[((pid * dstd::hash::GOLDEN_RATIO) ^
                           (tid * 0xC2B2AE3D27D4EB4FULL)) >>
                          56];
    if (slot.epoch != epoch_ || slot.pid != pid || slot.tid != tid) {
        slot = PidSlot{};
        slot.pid = pid;
        slot.tid = tid;
        slot.epoch = epoch_;
        slot.pid_size = static_cast<std::uint8_t>(
            dstd::to_chars(slot.pid_buf, slot.pid_buf + sizeof(slot.pid_buf),
                           pid)
                .ptr -
            slot.pid_buf);
        slot.tid_size = static_cast<std::uint8_t>(
            dstd::to_chars(slot.tid_buf, slot.tid_buf + sizeof(slot.tid_buf),
                           tid)
                .ptr -
            slot.tid_buf);
    }
    return slot;
}

void BloomCore::observe_data(ChunkState& chunk, EventCache& cache,
                             std::uint32_t name_id, std::uint32_t cat_id,
                             dstd::StringView name, dstd::StringView cat,
                             std::uint64_t pid, std::uint64_t tid,
                             std::uint64_t ts, std::uint64_t dur, bool has_dur,
                             dstd::StringView hhash, dstd::StringView fhash,
                             dstd::StringView shash) {
    EventCache::NameSlot& ns = cache.name_slot(name_id, cat_id);
    EventCache::PidSlot& ps = cache.pid_slot(pid, tid);
    chunk.statistics.update_from_event(ns.stats, ps.stats, name, cat, pid, tid,
                                       ts, dur, has_dur);

    // A value already in a chunk's bloom adds nothing, so a slot skips the
    // probe.
    auto observe_known = [&chunk](int bloom_idx, std::size_t dim_idx,
                                  dstd::StringView val, bool& in_bloom,
                                  std::uint32_t& dim_slot) {
        if (val.empty()) return;
        if (bloom_idx >= 0 && !in_bloom) {
            chunk.fixed_blooms[bloom_idx].add(val);
            in_bloom = true;
        }
        chunk.fixed_dim_stats[dim_idx].observe(val, dim_slot);
    };
    auto observe_fixed = [&chunk](int bloom_idx, std::size_t dim_idx,
                                  dstd::StringView val) {
        if (val.empty()) return;
        if (bloom_idx >= 0) chunk.fixed_blooms[bloom_idx].add(val);
        chunk.fixed_dim_stats[dim_idx].observe(val);
    };

    observe_known(BfName, FdName, name, ns.name_in_bloom, ns.name_dim);
    observe_known(BfCat, FdCat, cat, ns.cat_in_bloom, ns.cat_dim);

    const dstd::StringView pid_sv(ps.pid_buf, ps.pid_size);
    const dstd::StringView tid_sv(ps.tid_buf, ps.tid_size);
    observe_known(BfPid, FdPid, pid_sv, ps.pid_in_bloom, ps.pid_dim);
    observe_known(BfTid, FdTid, tid_sv, ps.tid_in_bloom, ps.tid_dim);

    char pt_buf[52];
    std::memcpy(pt_buf, ps.pid_buf, ps.pid_size);
    pt_buf[ps.pid_size] = ':';
    std::memcpy(pt_buf + ps.pid_size + 1, ps.tid_buf, ps.tid_size);
    const dstd::StringView pt_sv(pt_buf, ps.pid_size + 1 + ps.tid_size);
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
        const dstd::String joined =
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

dstd::Vector<index::store::IndexExtension> BloomCore::tier_extensions(
    const ChunkIndexerConfig& config) {
    dstd::Vector<IndexExtension> exts;
    for (auto ext : index::store::ALL_EXTENSIONS)
        if ((index::store::PRUNING_EXTENSIONS.has(ext) &&
             config.extensions.has(ext)) ||
            ext == IndexExtension::Stats || ext == IndexExtension::Catalog ||
            ext == IndexExtension::Metadata)
            exts.push_back(ext);
    return exts;
}

}  // namespace dftracer::utils::index::schemas::dft
