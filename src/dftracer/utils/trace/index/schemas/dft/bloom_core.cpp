// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/hash/value_hash.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/extensions/kinds/payloads.h>
#include <dftracer/utils/trace/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/trace/index/schemas/dft/bloom_core.h>
#include <dftracer/utils/trace/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/trace/index/tables.h>

#include <array>
#include <charconv>
#include <cstring>

using dftracer::utils::trace::index::extensions::ScalableBloomFilter;
namespace dftracer::utils::trace::index::schemas::dft {

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

using trace::index::store::IndexExtension;

// The build a file's tier records go into.
class Tier {
   public:
    explicit Tier(trace::index::FileBuild& build) : build_(build) {}

    void path(IndexExtension ext, dstd::StringView path) {
        trace::index::put_path(build_, ext, path);
    }
    void path_granule(IndexExtension ext, dstd::StringView path,
                      std::uint64_t granule, dstd::StringView payload) {
        trace::index::put_path_granule(build_, ext, path, granule, payload);
    }
    void path_file(IndexExtension ext, dstd::StringView path,
                   dstd::StringView payload) {
        trace::index::put_path_file(build_, ext, path, payload);
    }
    void posting(dstd::StringView path, std::uint64_t value_hash) {
        trace::index::put_posting(build_, path, value_hash);
    }
    void posting_granule(dstd::StringView path, std::uint64_t value_hash,
                         std::uint64_t granule) {
        trace::index::put_posting_granule(build_, path, value_hash, granule);
    }
    void chunk_statistics(std::uint64_t checkpoint_idx,
                          const BloomCore::ChunkStatistics& stats) {
        trace::index::put_chunk_statistics(build_, checkpoint_idx, stats);
    }
    void chunk_metadata(std::uint64_t checkpoint_idx,
                        const trace::index::store::ChunkMetadata& metadata) {
        trace::index::put_chunk_metadata(build_, checkpoint_idx, metadata);
    }
    void scalar_stats(const BloomCore::ChunkStatistics& stats,
                      std::uint64_t chunks) {
        trace::index::put_file_scalar_stats(build_, stats, chunks);
    }
    void pid_tid_counts(const dstd::StringViewMap<std::uint64_t>& counts) {
        trace::index::put_file_pid_tid_counts(build_, counts);
    }
    void catalog_path(dstd::StringView path,
                      const trace::index::store::PathStat& stat) {
        trace::index::put_catalog_path(build_, path, stat);
    }
    void finish(IndexExtension ext, std::uint64_t params_hash) {
        build_.finish(ext, params_hash);
    }

   private:
    trace::index::FileBuild& build_;
};

using trace::index::extensions::BloomBuilder;
using trace::index::extensions::ExtensionOut;
using trace::index::extensions::PostingsBuilder;
using trace::index::extensions::ZonemapBuilder;

constexpr std::uint64_t NO_CAP = UINT64_MAX;

void put_bloom(Tier& t, std::uint64_t checkpoint_idx, dstd::StringView path,
               BloomBuilder& bloom) {
    ExtensionOut out{{}, NO_CAP, false};
    bloom.finish_granule(out);
    t.path_granule(IndexExtension::Bloom, path, checkpoint_idx, out.bytes);
}

void put_file_bloom(Tier& t, dstd::StringView path, BloomBuilder& bloom) {
    ExtensionOut out{{}, NO_CAP, false};
    bloom.finish_file(out);
    t.path_file(IndexExtension::Bloom, path, out.bytes);
}

// The hashes of the names a chunk or a file holds, as its postings.
PostingsBuilder name_postings(
    const dstd::StringViewMap<std::uint64_t>& name_counts) {
    PostingsBuilder postings;
    for (const auto& [name, _] : name_counts)
        postings.add_hashed(dstd::hash::value_hash_string(name));
    return postings;
}

// One dimension's statistics as a zonemap and, when its value dictionary fit
// the cap, counts. `observed` is the chunk's data events. An extra dimension
// without a value gets an absence zone.
void put_dimension(Tier& t, std::uint64_t checkpoint_idx,
                   BloomCore::ChunkDimensionStats& ds, std::uint64_t observed,
                   const BloomCore::ChunkIndexerConfig& config,
                   const BloomCore::ChunkStatistics& stats, bool extra) {
    const bool zones = config.extensions.has(IndexExtension::Zonemap);
    const bool counts = config.extensions.has(IndexExtension::Counts);
    if (!zones && !counts) return;
    // dftracer's ts and dur are on every data event; other dimensions count.
    const bool always_present =
        config.fixed_dimensions &&
        (ds.dimension == DIM_TS || ds.dimension == DIM_DUR);
    const std::uint64_t present = always_present ? observed : ds.present;
    const bool absent = extra && present == 0;
    if (zones) {
        ds.zone.set_observed(observed);
        ds.zone.set_present(present);
        if (ds.dimension == DIM_TS && !stats.timestamp_histogram.empty())
            ds.zone.set_histogram(stats.timestamp_histogram);
        // A dimension with no value is stored only as an absence.
        const auto& b = ds.zone.bounds();
        const bool holds_values =
            b.has_bounds() || b.null_count() != 0 || b.nan_count() != 0 ||
            (b.type() == trace::index::extensions::BoundType::Mixed &&
             present != 0);
        if (absent || holds_values) {
            ExtensionOut out{{}, NO_CAP, false};
            if (ds.zone.finish_granule(out))
                t.path_granule(IndexExtension::Zonemap, ds.dimension,
                               checkpoint_idx, out.bytes);
        }
    }
    if (counts) {
        ds.counts.set_observed(observed);
        ExtensionOut out{{}, NO_CAP, false};
        if (ds.counts.finish_granule(out))
            t.path_granule(IndexExtension::Counts, ds.dimension, checkpoint_idx,
                           out.bytes);
    }
}

void write_chunk_to(Tier& t, std::uint64_t checkpoint_idx,
                    BloomCore::ChunkState& chunk,
                    dstd::Span<const dstd::String> extra_dims,
                    const BloomCore::ChunkIndexerConfig& config,
                    BloomCore::FileAccumulator& acc) {
    const auto& m = config.extensions;
    const bool blooms = m.has(IndexExtension::Bloom);
    const bool zones = m.has(IndexExtension::Zonemap);
    const bool fixed = config.fixed_dimensions;
    const auto observed = chunk.statistics.total_events;

    for (std::size_t b = 0; fixed && blooms && b < BloomCore::BfCount; ++b) {
        put_bloom(t, checkpoint_idx, FIXED_BLOOM_NAMES[b],
                  chunk.fixed_blooms[b]);
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
            put_bloom(t, checkpoint_idx, extra_dims[e], chunk.extra_blooms[e]);
            file_bloom.bloom.merge_from(chunk.extra_blooms[e]);
            continue;
        }
        const auto& values = chunk.extra_values[e];
        // One more than the values, so the last add opens no second level.
        BloomBuilder bf(values.size() + 1, config.false_positive_rate);
        bf.add_str(values);
        file_bloom.bloom.add_str(values);
        put_bloom(t, checkpoint_idx, extra_dims[e], bf);
    }

    t.chunk_statistics(checkpoint_idx, chunk.statistics);
    t.chunk_metadata(checkpoint_idx, chunk.metadata);
    acc.statistics.merge_from(chunk.statistics);
    ++acc.chunks;

    for (auto& ds : chunk.fixed_dim_stats)
        if (fixed)
            put_dimension(t, checkpoint_idx, ds, observed, config,
                          chunk.statistics, false);
    for (auto& ds : chunk.extra_dim_stats)
        put_dimension(t, checkpoint_idx, ds, observed, config, chunk.statistics,
                      true);
    // Event end (ts + dur): the time bounds a time range prunes with. Its
    // min is the first start, a lower bound of every end.
    if (zones && observed > 0 &&
        chunk.statistics.min_timestamp_us <=
            chunk.statistics.max_timestamp_us) {
        ZonemapBuilder te;
        te.bounds().add(chunk.statistics.min_timestamp_us);
        te.bounds().add(chunk.statistics.max_timestamp_us);
        te.set_observed(observed);
        te.set_present(observed);
        ExtensionOut out{{}, NO_CAP, false};
        if (te.finish_granule(out))
            t.path_granule(IndexExtension::Zonemap, DIM_TE, checkpoint_idx,
                           out.bytes);
    }

    // Postings hash the value, so postings written by concurrent workers or
    // sinks agree.
    if (m.has(IndexExtension::Postings))
        for (std::uint64_t h :
             name_postings(chunk.statistics.name_counts).hashes())
            t.posting_granule(DIM_NAME, h, checkpoint_idx);
}

void write_absent_extra_to(Tier& t, std::uint64_t checkpoint_idx,
                           dstd::StringView dim, std::uint64_t observed,
                           const BloomCore::ChunkIndexerConfig& config) {
    BloomCore::ChunkDimensionStats ds;
    ds.dimension = dstd::String(dim);
    put_dimension(t, checkpoint_idx, ds, observed, config, {}, true);
}

BloomCore::ChunkStatistics finish_file_to(
    Tier& t, BloomCore::FileAccumulator& acc,
    const BloomCore::ChunkIndexerConfig& config,
    dstd::Span<const dstd::String> extra_dims,
    const BloomCore::Catalog& catalog) {
    const auto& m = config.extensions;
    const bool blooms = m.has(IndexExtension::Bloom);
    const bool zones = m.has(IndexExtension::Zonemap);
    const bool fixed = config.fixed_dimensions;

    for (std::size_t b = 0; fixed && blooms && b < BloomCore::BfCount; ++b) {
        put_file_bloom(t, FIXED_BLOOM_NAMES[b], acc.fixed_blooms[b]);
        t.path(IndexExtension::Bloom, FIXED_BLOOM_NAMES[b]);
    }
    for (std::size_t e = 0; blooms && e < extra_dims.size(); ++e) {
        auto& file_bloom = acc.extra(extra_dims[e]);
        if (!file_bloom.skip)
            put_file_bloom(t, extra_dims[e], file_bloom.bloom);
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
        for (std::uint64_t h :
             name_postings(acc.statistics.name_counts).hashes())
            t.posting(DIM_NAME, h);
    }

    for (const auto& [path, stat] : catalog) t.catalog_path(path, stat);
    for (auto ext : BloomCore::tier_extensions(config))
        t.finish(ext, config.params_hash(ext));
    return acc.statistics;
}

}  // namespace

dstd::Result<dstd::Vector<dstd::String>> BloomCore::seed_accumulator(
    const trace::index::Snapshot& snapshot, const trace::index::FileRef& file,
    const ChunkIndexerConfig& config, FileAccumulator& acc) {
    auto stats_read = trace::index::scalar_stats(snapshot, file);
    if (!stats_read) return stats_read.error();
    auto stats = std::move(*stats_read);
    if (stats) {
        acc.statistics = std::move(stats->stats);
        acc.chunks = stats->chunk_count;
    }
    auto pid_tid_read = trace::index::pid_tid_counts(snapshot, file);
    if (!pid_tid_read) return pid_tid_read.error();
    auto pid_tid = std::move(*pid_tid_read);
    if (pid_tid) acc.statistics.pid_tid_counts = std::move(*pid_tid);
    dstd::Optional<dstd::Error> failed;
    const auto file_bloom = [&](dstd::StringView dim)
        -> dstd::Optional<trace::index::extensions::ScalableBloomFilter> {
        auto read =
            trace::index::path_file(snapshot, file, IndexExtension::Bloom, dim);
        if (!read) {
            if (!failed) failed = read.error();
            return dstd::nullopt;
        }
        auto payload = std::move(*read);
        if (!payload) return dstd::nullopt;
        auto bloom = trace::index::extensions::kinds::decode_bloom(*payload);
        // New values cannot join a filter whose hashes this build cannot make.
        if (bloom && !bloom->has_known_hash_version()) {
            if (!failed)
                failed = dstd::Error(
                    dstd::Condition::InvalidArgument,
                    "index: stored bloom filter has an unknown hash version");
            return dstd::nullopt;
        }
        return bloom;
    };
    for (std::size_t b = 0; config.fixed_dimensions && b < BfCount; ++b) {
        auto bloom = file_bloom(FIXED_BLOOM_NAMES[b]);
        if (failed) return *failed;
        if (bloom) acc.fixed_blooms[b].filter() = std::move(*bloom);
    }
    dstd::Set<dstd::String> dims;
    for (auto ext : {IndexExtension::Zonemap, IndexExtension::Bloom,
                     IndexExtension::Counts}) {
        if (!config.extensions.has(ext)) continue;
        auto listed = trace::index::paths(snapshot, file, ext);
        if (!listed) return listed.error();
        for (auto& path : *listed) dims.insert(std::move(path));
    }
    dstd::Vector<dstd::String> extras;
    for (const dstd::String& dim : dims) {
        const dstd::StringView name(dim.data(), dim.size());
        const bool is_fixed =
            name == DIM_TE ||
            std::find(FIXED_DIM_NAMES.begin(), FIXED_DIM_NAMES.end(), name) !=
                FIXED_DIM_NAMES.end();
        if (is_fixed) continue;
        auto& extra = acc.extra(name);
        auto bloom = file_bloom(name);
        if (failed) return *failed;
        if (bloom) {
            extra.bloom.filter() = std::move(*bloom);
        } else {
            extra.skip = true;
        }
        const bool is_named = std::find(config.extra_dimensions.begin(),
                                        config.extra_dimensions.end(),
                                        dim) != config.extra_dimensions.end();
        if (!is_named) extras.push_back(dim);
    }
    return extras;
}

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
                          ExtraBloom{
                              BloomBuilder(config_->expected_entries_per_chunk,
                                           config_->false_positive_rate),
                              false})
                 .first;
    return it->second;
}

void BloomCore::write_chunk(trace::index::FileBuild& build,
                            std::uint64_t checkpoint_idx, ChunkState& chunk,
                            dstd::Span<const dstd::String> extra_dims,
                            const ChunkIndexerConfig& config,
                            FileAccumulator& acc) {
    Tier tier(build);
    write_chunk_to(tier, checkpoint_idx, chunk, extra_dims, config, acc);
}

void BloomCore::write_absent_extra(trace::index::FileBuild& build,
                                   std::uint64_t checkpoint_idx,
                                   dstd::StringView dim, std::uint64_t observed,
                                   const ChunkIndexerConfig& config) {
    Tier tier(build);
    write_absent_extra_to(tier, checkpoint_idx, dim, observed, config);
}

BloomCore::ChunkStatistics BloomCore::finish_file(
    trace::index::FileBuild& build, FileAccumulator& acc,
    const ChunkIndexerConfig& config, dstd::Span<const dstd::String> extra_dims,
    const Catalog& catalog) {
    Tier tier(build);
    return finish_file_to(tier, acc, config, extra_dims, catalog);
}

BloomCore::ChunkState::ChunkState() = default;

dstd::Span<const dstd::StringView> BloomCore::fixed_dimension_names() {
    return FIXED_DIM_NAMES;
}

void BloomCore::observe_value(ChunkDimensionStats& stats, std::int64_t value) {
    ++stats.present;
    stats.zone.bounds().add(value);
}

void BloomCore::observe_value(ChunkDimensionStats& stats, std::uint64_t value) {
    ++stats.present;
    stats.zone.bounds().add(value);
}

void BloomCore::observe_value(ChunkDimensionStats& stats, double value) {
    ++stats.present;
    stats.zone.bounds().add(value);
}

void BloomCore::observe_value(ChunkDimensionStats& stats,
                              dstd::StringView value) {
    ++stats.present;
    stats.zone.bounds().add(value);
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e,
                              std::int64_t value) {
    chunk.extra_blooms[e].add_hashed(dstd::hash::value_hash_int64(value));
    observe_value(chunk.extra_dim_stats[e], value);
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e,
                              std::uint64_t value) {
    chunk.extra_blooms[e].add_hashed(dstd::hash::value_hash_uint64(value));
    observe_value(chunk.extra_dim_stats[e], value);
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e, double value) {
    chunk.extra_blooms[e].add_hashed(dstd::hash::value_hash_double(value));
    observe_value(chunk.extra_dim_stats[e], value);
}

void BloomCore::observe_extra(ChunkState& chunk, std::size_t e,
                              dstd::StringView value) {
    chunk.extra_blooms[e].filter().add(value);
    observe_value(chunk.extra_dim_stats[e], value);
}

void BloomCore::init_chunk_state(ChunkState& chunk,
                                 const ChunkIndexerConfig& config,
                                 dstd::Span<const dstd::String> extra_dims) {
    for (std::size_t b = 0; b < BfCount; ++b) {
        chunk.fixed_blooms[b] = BloomBuilder(config.expected_entries_per_chunk,
                                             config.false_positive_rate);
    }
    for (std::size_t d = 0; d < FdCount; ++d) {
        auto& ds = chunk.fixed_dim_stats[d];
        ds.dimension = dstd::String(FIXED_DIM_NAMES[d]);
        ds.numeric_text = d == FdPid || d == FdTid;
        ds.counts.set_cap(config.value_counts_cap);
    }
    chunk.extra_blooms.clear();
    chunk.extra_dim_stats.clear();
    chunk.extra_blooms.reserve(extra_dims.size());
    chunk.extra_dim_stats.resize(extra_dims.size());
    for (std::size_t e = 0; e < extra_dims.size(); ++e) {
        chunk.extra_blooms.emplace_back(config.expected_entries_per_chunk,
                                        config.false_positive_rate);
        chunk.extra_dim_stats[e].dimension = extra_dims[e];
        chunk.extra_dim_stats[e].counts.set_cap(config.value_counts_cap);
    }
    chunk.extra_values.assign(extra_dims.size(), {});
    chunk.extra_bloom_skip.assign(extra_dims.size(), 0);
}

void BloomCore::observe_metadata_path(ChunkState& chunk,
                                      dstd::StringView path) {
    trace::index::store::add_metadata_value(chunk.metadata.paths, path);
}

void BloomCore::observe_metadata(ChunkState& chunk,
                                 dstd::StringView record_name) {
    ++chunk.metadata.records;
    trace::index::store::add_metadata_value(chunk.metadata.names, record_name);
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
    PidSlot& slot = pids_[((pid * dstd::hash::GOLDEN_RATIO) ^
                           (tid * dstd::hash::XXH64_PRIME2)) >>
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
                                  std::uint32_t& dim_slot, auto&& value_hash) {
        if (val.empty()) return;
        if (bloom_idx >= 0 && !in_bloom) {
            chunk.fixed_blooms[bloom_idx].add_hashed(value_hash());
            in_bloom = true;
        }
        chunk.fixed_dim_stats[dim_idx].observe(val, dim_slot);
    };
    auto observe_fixed = [&chunk](int bloom_idx, std::size_t dim_idx,
                                  dstd::StringView val) {
        if (val.empty()) return;
        if (bloom_idx >= 0) chunk.fixed_blooms[bloom_idx].filter().add(val);
        chunk.fixed_dim_stats[dim_idx].observe(val);
    };

    observe_known(BfName, FdName, name, ns.name_in_bloom, ns.name_dim,
                  [name] { return dstd::hash::value_hash_string(name); });
    observe_known(BfCat, FdCat, cat, ns.cat_in_bloom, ns.cat_dim,
                  [cat] { return dstd::hash::value_hash_string(cat); });

    const dstd::StringView pid_sv(ps.pid_buf, ps.pid_size);
    const dstd::StringView tid_sv(ps.tid_buf, ps.tid_size);
    observe_known(BfPid, FdPid, pid_sv, ps.pid_in_bloom, ps.pid_dim,
                  [pid] { return dstd::hash::value_hash_uint64(pid); });
    observe_known(BfTid, FdTid, tid_sv, ps.tid_in_bloom, ps.tid_dim,
                  [tid] { return dstd::hash::value_hash_uint64(tid); });

    char pt_buf[52];
    std::memcpy(pt_buf, ps.pid_buf, ps.pid_size);
    pt_buf[ps.pid_size] = ':';
    std::memcpy(pt_buf + ps.pid_size + 1, ps.tid_buf, ps.tid_size);
    const dstd::StringView pt_sv(pt_buf, ps.pid_size + 1 + ps.tid_size);
    bool no_bloom = false;
    observe_known(-1, FdPidTid, pt_sv, no_bloom, ps.pid_tid_dim,
                  [] { return std::uint64_t{0}; });

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
    dst.counts.merge_from(src.counts);
    dst.zone.merge_from(src.zone);
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
    for (auto list : {&trace::index::store::ChunkMetadata::names,
                      &trace::index::store::ChunkMetadata::paths}) {
        auto& into = dst.metadata.*list;
        const auto& from = src.metadata.*list;
        if (!from) into.reset();
        if (!into) continue;
        for (const auto& v : *from)
            trace::index::store::add_metadata_value(into, v);
    }
    dst.events_processed += src.events_processed;
}

dstd::Vector<trace::index::store::IndexExtension> BloomCore::tier_extensions(
    const ChunkIndexerConfig& config) {
    dstd::Vector<IndexExtension> exts;
    for (auto ext : trace::index::store::ALL_EXTENSIONS)
        if ((trace::index::store::PRUNING_EXTENSIONS.has(ext) &&
             config.extensions.has(ext)) ||
            ext == IndexExtension::Stats || ext == IndexExtension::Catalog ||
            ext == IndexExtension::Metadata)
            exts.push_back(ext);
    return exts;
}

}  // namespace dftracer::utils::trace::index::schemas::dft
