// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/extensions/bloom_filter.h>
#include <dftracer/utils/trace/index/extensions/bloom_fold.h>
#include <dftracer/utils/trace/index/extensions/catalog_fold.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <system_error>
#include <variant>

namespace dftracer::utils::trace::index::extensions {

namespace {

using BV = trace::index::schemas::dft::BloomCore;
// What a stored evidence record costs besides its path and payload.
constexpr std::uint64_t RECORD_BYTES = 32;

// The bits of a chunk bloom sized for `values`, as write_chunk sizes it.
std::uint64_t bloom_bytes(std::size_t values, double false_positive_rate) {
    return BloomFilter::optimal_bit_count(values + 1, false_positive_rate) / 8;
}

}  // namespace

namespace {

dstd::String capture_path(dstd::StringView dim) {
    return dstd::fmt<"args.{}">(dim);
}

}  // namespace

BloomFold::BloomFold(
    dftracer::utils::dstd::StringIntern& intern,
    trace::index::schemas::dft::BloomCore::ChunkIndexerConfig config)
    : intern_(&intern),
      config_(std::move(config)),
      feeder_(intern, config_, !config_.auto_prefix.empty()) {
    if (!config_.validate())
        dstd::fatal("BloomFold: the config was not validated");
}

BloomFold::Catalog BloomFold::file_catalog(dstd::StringView file) {
    auto it = files_.find(file);
    if (it == files_.end()) return {};
    flush_shapes(it->second);
    return catalog(it->second);
}

void BloomFold::observe_shape(FileState& fs,
                              dstd::Span<const SchemaLeaf> leaves) {
    if (leaves.empty()) return;
    auto same = [&](const Shape& s) {
        return std::equal(leaves.begin(), leaves.end(), s.leaves.begin(),
                          s.leaves.end());
    };
    if (fs.last_shape < fs.shapes.size() && same(fs.shapes[fs.last_shape])) {
        ++fs.shapes[fs.last_shape].events;
        return;
    }
    std::uint64_t h = leaves.size();
    for (const auto& [id, tag] : leaves)
        h = dstd::hash::splitmix64(h ^ ((std::uint64_t{id} << 8) | tag));
    for (;; ++h) {
        auto [it, fresh] = fs.shape_ids.try_emplace(
            h, static_cast<std::uint32_t>(fs.shapes.size()));
        if (fresh) {
            Shape& s = fs.shapes.emplace_back();
            s.leaves.assign(leaves.begin(), leaves.end());
        } else if (!same(fs.shapes[it->second]))
            continue;
        fs.last_shape = it->second;
        ++fs.shapes[it->second].events;
        return;
    }
}

void BloomFold::flush_shapes(FileState& fs) {
    dstd::HashMap<std::uint64_t, std::uint64_t> total;
    for (Shape& s : fs.shapes) {
        if (s.events == 0) continue;
        for (const auto& [id, tag] : s.leaves)
            total[(std::uint64_t{id} << 8) | tag] += s.events;
        s.events = 0;
    }
    for (const auto& [key, n] : total) {
        const auto tag = static_cast<std::uint8_t>(key & 0xFF);
        auto& stat = fs.paths[static_cast<std::uint32_t>(key >> 8)];
        observe_catalog_path(stat, tag);
        ++catalog_observations_;
        if (static_cast<trace::index::store::PathType>(tag) !=
            trace::index::store::PathType::NullValue)
            stat.count += n - 1;
    }
}

BloomFold::Catalog BloomFold::catalog(const FileState& fs) const {
    Catalog out;
    out.reserve(fs.paths.size());
    for (const auto& [id, stat] : fs.paths)
        out.emplace_back(intern_->resolve(id), stat);
    return out;
}

void BloomFold::summarize(PathEvidence& ev, const AutoField& f) const {
    const auto& zone = f.stats.zone.bounds();
    const bool string = zone.type() == BoundType::String;
    if (ev.present_chunks == 0) {
        ev.bounds = zone;
    } else if (!zone.same_bounds(ev.bounds)) {
        ev.same_bounds = false;
    }
    ++ev.present_chunks;
    if (zone.has_bounds()) {
        ++ev.zone_chunks;
        ev.payload_bytes += zone.bounds_bytes();
    }
    if (string && !f.overflow) {
        ++ev.bloom_chunks;
        ev.payload_bytes +=
            bloom_bytes(f.values.size(), config_.false_positive_rate);
        ev.distinct += f.values.size();
    }
}

BloomFold::AutoKeys BloomFold::select_auto_keys(
    dstd::CStringView file, const FileState& fs, std::uint64_t data_chunks,
    const dstd::Map<dstd::String, std::uint32_t>& keys) const {
    std::uint64_t file_bytes = 0;
    if (file_bytes_) {
        file_bytes = *file_bytes_;
    } else {
        dftracer::utils::common::InputFileStat st;
        if (dftracer::utils::common::stat_input(file, st)) file_bytes = st.size;
    }
    const auto share = static_cast<std::uint64_t>(
        config_.stats_share * static_cast<double>(file_bytes));
    const std::uint64_t cap =
        std::max(trace::index::build::STATS_FLOOR_BYTES, share);
    const double fpr = config_.false_positive_rate;
    const std::uint64_t file_bloom_floor =
        bloom_bytes(config_.expected_entries_per_chunk, fpr);
    std::uint64_t used = data_chunks * config_.extra_dimensions.size() *
                         (2 * RECORD_BYTES + file_bloom_floor);

    dstd::Vector<dstd::Pair<std::uint64_t, const dstd::String*>> ranked;
    ranked.reserve(keys.size());
    for (const auto& [name, k] : keys) {
        auto it =
            fs.paths.find(intern_->get_or_insert(config_.auto_prefix + name));
        ranked.emplace_back(it == fs.paths.end() ? 0 : it->second.count, &name);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : *a.second < *b.second;
    });
    AutoKeys kept;
    for (const auto& [count, name] : ranked) {
        const std::uint32_t id = keys.at(*name);
        const PathEvidence& ev = fs.evidence.at(id);
        const bool absence_only = ev.same_bounds && ev.bloom_chunks == 0;
        const std::uint64_t absent = data_chunks - ev.present_chunks;
        if (absence_only && absent == 0) continue;
        if (config_.path_budget > 0 && kept.size() >= config_.path_budget)
            break;
        const std::uint64_t record = RECORD_BYTES + name->size();
        std::uint64_t cost = absent * record;
        if (!absence_only) {
            cost +=
                (ev.zone_chunks + ev.bloom_chunks) * record + ev.payload_bytes;
            if (ev.bloom_chunks == ev.present_chunks)
                cost += record + std::max(bloom_bytes(ev.distinct, fpr),
                                          file_bloom_floor);
        }
        if (used + cost > cap) break;
        used += cost;
        kept.push_back({*name, id, absence_only});
    }
    std::sort(kept.begin(), kept.end(), [](const AutoKey& a, const AutoKey& b) {
        return a.name < b.name;
    });
    return kept;
}

BloomFold::ChunkState BloomFold::assemble_chunk(
    FileState& fs, std::uint64_t cp, dstd::Span<const dstd::String> dims,
    const AutoKeys& keys) {
    ChunkState out;
    if (auto base = fs.chunks.find(cp); base != fs.chunks.end()) {
        out = std::move(base->second);
        fs.chunks.erase(base);
    } else {
        BV::init_chunk_state(out, config_, config_.extra_dimensions);
    }
    const std::size_t named = config_.extra_dimensions.size();
    out.extra_dim_stats.resize(dims.size());
    out.extra_values.resize(dims.size());
    out.extra_bloom_skip.resize(dims.size(), 0);
    for (std::size_t e = named; e < dims.size(); ++e) {
        out.extra_dim_stats[e].dimension = dims[e];
        out.extra_dim_stats[e].counts.set_cap(config_.value_counts_cap);
    }
    auto ac = fs.auto_chunks.find(cp);
    if (ac == fs.auto_chunks.end()) return out;
    std::size_t e = named;
    for (const AutoKey& key : keys) {
        auto it = ac->second.find(key.id);
        if (it != ac->second.end()) {
            AutoField& f = it->second;
            auto& ds = out.extra_dim_stats[e];
            ds = std::move(f.stats);
            ds.dimension = key.name;
            if (key.absence_only) {
                ds.zone = ZonemapBuilder();
                out.extra_bloom_skip[e] = 1;
            } else if (ds.zone.bounds().type() == BoundType::String &&
                       !f.overflow) {
                auto& values = out.extra_values[e];
                values.reserve(f.values.size());
                for (std::uint32_t id : f.values)
                    values.push_back(intern_->resolve(id));
                std::sort(values.begin(), values.end());
            } else {
                out.extra_bloom_skip[e] = 1;
            }
        }
        ++e;
    }
    fs.auto_chunks.erase(ac);
    return out;
}

BloomFold::AutoKeys BloomFold::auto_keys(dstd::CStringView file,
                                         FileState& fs) {
    flush_shapes(fs);
    std::uint64_t data_chunks = 0;
    for (const auto& chunk : fs.spill.keys)
        if (chunk.events > 0) ++data_chunks;
    for (const auto& [cp, chunk] : fs.chunks)
        if (chunk.statistics.total_events > 0) ++data_chunks;
    dstd::Map<dstd::String, std::uint32_t> keys;
    for (const auto& [cp, ac] : fs.auto_chunks)
        for (const auto& [k, f] : ac) {
            keys.emplace(dstd::String(intern_->resolve(k)), k);
            summarize(fs.evidence[k], f);
        }
    for (std::uint32_t k : fs.spill.key_union)
        keys.emplace(dstd::String(intern_->resolve(k)), k);
    return select_auto_keys(file, fs, data_chunks, keys);
}

dstd::Vector<dstd::String> BloomFold::dims_of(const AutoKeys& keys) const {
    dstd::Vector<dstd::String> dims = config_.extra_dimensions;
    for (const AutoKey& key : keys) dims.push_back(key.name);
    return dims;
}

void BloomFold::continue_file(const trace::index::Snapshot* snapshot,
                              const trace::index::FileRef* file,
                              std::uint64_t next_chunk) {
    continue_snapshot_ = snapshot;
    continue_file_ = file;
    continue_next_ = next_chunk;
}

void BloomFold::write_pending(trace::index::FileBuild& build, FileState& fs) {
    auto& sp = fs.spill;
    if (!sp.acc) sp.acc.emplace(config_);
    for (auto& pc : sp.pending)
        BV::write_chunk(build, pc.checkpoint, pc.chunk, pc.dims, config_,
                        *sp.acc);
    sp.pending.clear();
}

dstd::Result<void> BloomFold::write_file(trace::index::FileBuild& build,
                                         dstd::CStringView file,
                                         FileState& fs) {
    auto& sp = fs.spill;
    AutoKeys continued;
    if (continue_snapshot_ != nullptr) {
        sp.next = std::max(sp.next, continue_next_);
        if (!sp.acc) sp.acc.emplace(config_);
        // Before write_pending: a run's chunks merge into the seeded records.
        const auto seeded = BV::seed_accumulator(
            *continue_snapshot_, *continue_file_, config_, *sp.acc);
        if (!seeded) return seeded.error();
        for (const dstd::String& dim : *seeded)
            continued.push_back({dim, intern_->get_or_insert(dim), false});
    }
    write_pending(build, fs);
    // A whole-file read fills every checkpoint, so any gap below the last one
    // is an empty chunk for an unobserved member, unless its run wrote it
    // already. One chunk is assembled at a time.
    const std::uint64_t end =
        fs.chunks.empty() ? sp.next
                          : std::max(sp.next, fs.chunks.rbegin()->first + 1);
    const AutoKeys keys = [&] {
        if (continue_snapshot_ == nullptr) return auto_keys(file, fs);
        flush_shapes(fs);
        return continued;
    }();
    const auto dims = dims_of(keys);
    // The runs hold every candidate path's evidence. The paths the selection
    // did not keep are erased from them.
    dstd::HashSet<std::uint32_t> kept;
    for (const AutoKey& key : keys)
        if (!key.absence_only) kept.insert(key.id);
    for (std::uint32_t k : sp.key_union)
        if (!kept.contains(k))
            for (auto ext : {trace::index::store::IndexExtension::Zonemap,
                             trace::index::store::IndexExtension::Bloom,
                             trace::index::store::IndexExtension::Counts})
                trace::index::erase_path(build, ext, intern_->resolve(k));
    for (std::uint64_t cp = sp.next; cp < end; ++cp) {
        ChunkState chunk = assemble_chunk(fs, cp, dims, keys);
        BV::write_chunk(build, cp, chunk, dims, config_, *sp.acc);
    }
    // A run wrote the records of every auto key it met; one the selection kept
    // only as absence entries still owes the chunks that lacked it.
    for (const auto& spilled : sp.keys) {
        dstd::HashSet<std::uint32_t> had(spilled.keys.begin(),
                                         spilled.keys.end());
        for (const AutoKey& key : keys)
            if (!had.contains(key.id))
                BV::write_absent_extra(build, spilled.checkpoint, key.name,
                                       spilled.events, config_);
    }
    fs.auto_chunks.clear();
    auto merged = catalog(fs);
    if (continue_snapshot_ != nullptr) {
        const auto stored =
            trace::index::catalog(*continue_snapshot_, *continue_file_);
        if (!stored) return stored.error();
        const auto& old = *stored;
        for (auto& [path, stat] : merged) {
            const auto known = std::lower_bound(
                old.begin(), old.end(), path,
                [](const auto& entry, dstd::StringView wanted) {
                    return dstd::StringView(entry.first) < wanted;
                });
            if (known == old.end() || dstd::StringView(known->first) != path)
                continue;
            stat.count += known->second.count;
            stat.seen |= known->second.seen;
            stat.type =
                trace::index::store::join(stat.type, known->second.type);
        }
    }
    BV::finish_file(build, *sp.acc, config_, dims, merged);
    return {};
}

std::uint64_t BloomFold::resident_bytes(const FileState& fs,
                                        std::uint64_t cp) const {
    // Held bytes plus what the chunk's records take in a run buffer.
    constexpr std::uint64_t ENTRY = 64;
    constexpr std::uint64_t SKETCH = 1024;
    std::uint64_t n = 0;
    auto dim = [&](const auto& ds) {
        n += sizeof(ds) + ds.zone.bounds().bounds_bytes();
        n += ds.counts.distinct_count() * ENTRY * 2;
    };
    if (auto it = fs.chunks.find(cp); it != fs.chunks.end()) {
        const ChunkState& c = it->second;
        n += sizeof(ChunkState);
        for (const auto& b : c.fixed_blooms) n += 2 * b.filter().size_bytes();
        for (const auto& b : c.extra_blooms) n += 2 * b.filter().size_bytes();
        for (const auto& ds : c.fixed_dim_stats) dim(ds);
        for (const auto& ds : c.extra_dim_stats) dim(ds);
        const auto& st = c.statistics;
        n += (st.name_counts.size() + st.category_counts.size() +
              st.pid_tid_counts.size()) *
                 ENTRY +
             (st.name_duration_sketches.size() +
              st.cat_duration_sketches.size() +
              st.pid_duration_sketches.size()) *
                 SKETCH;
    }
    if (auto it = fs.auto_chunks.find(cp); it != fs.auto_chunks.end())
        for (const auto& [k, f] : it->second) {
            n += sizeof(AutoField) + ENTRY + f.values.size() * 8;
            dim(f.stats);
            if (!f.overflow &&
                f.stats.zone.bounds().type() == BoundType::String)
                n += 2 *
                     bloom_bytes(f.values.size(), config_.false_positive_rate);
        }
    return n;
}

void BloomFold::enable_spill(std::uint64_t share,
                             trace::index::FileBuild& build) {
    spill_share_ = share;
    build_ = &build;
}

coro::CoroTask<void> BloomFold::write_run() {
    spill_pending_ = false;
    if (files_.empty()) co_return;
    FileState& fs = files_.begin()->second;
    auto& sp = fs.spill;
    if (sp.pending.empty()) co_return;
    auto held = co_await trace::index::claim(*build_);
    if (!held) co_return;
    write_pending(held->build(), fs);
    auto staged = co_await held->stage();
    if (staged) {
        ++sp.runs;
        sp.run_bytes = 0;
    }
}

std::size_t BloomFold::spill_runs() const {
    if (files_.empty()) return 0;
    const auto& sp = files_.begin()->second.spill;
    return sp.runs + (sp.run_bytes > 0 ? 1 : 0);
}

std::uint64_t BloomFold::run_limit(std::uint64_t share,
                                   std::size_t runs) noexcept {
    const std::uint64_t base = std::max<std::uint64_t>(
        share / 4, common::constants::file_admission::MIN_SPILL_RUN_BYTES);
    const std::size_t doublings = std::min(
        runs / common::constants::file_admission::SPILL_RUNS_PER_DOUBLING,
        common::constants::file_admission::MAX_SPILL_DOUBLINGS);
    return base > (std::numeric_limits<std::uint64_t>::max() >> doublings)
               ? std::numeric_limits<std::uint64_t>::max()
               : base << doublings;
}

void BloomFold::chunk_started(FileState& fs, std::uint64_t cp) {
    if (fs.spill.max_seen && cp <= *fs.spill.max_seen) return;
    fs.spill.max_seen = cp;
    for (; fs.spill.final_upto < cp; ++fs.spill.final_upto)
        fs.spill.resident += resident_bytes(fs, fs.spill.final_upto);
    if (fs.spill.resident > spill_share_) spill(fs, cp);
}

void BloomFold::spill(FileState& fs, std::uint64_t upto) {
    auto& sp = fs.spill;
    if (!sp.acc) sp.acc.emplace(config_);
    for (std::uint64_t cp = sp.next; cp < upto; ++cp) {
        sp.run_bytes += resident_bytes(fs, cp);
        AutoKeys own;
        if (auto ac = fs.auto_chunks.find(cp); ac != fs.auto_chunks.end()) {
            for (const auto& [k, f] : ac->second) {
                own.push_back({dstd::String(intern_->resolve(k)), k, false});
                summarize(fs.evidence[k], f);
            }
            std::sort(own.begin(), own.end(),
                      [](const AutoKey& a, const AutoKey& b) {
                          return a.name < b.name;
                      });
        }
        ChunkState chunk = assemble_chunk(fs, cp, dims_of(own), own);
        sp.events += chunk.statistics.total_events;
        dstd::Vector<std::uint32_t> ids;
        ids.reserve(own.size());
        for (const AutoKey& key : own) {
            ids.push_back(key.id);
            sp.key_union.insert(key.id);
        }
        sp.keys.push_back({cp, chunk.statistics.total_events, std::move(ids)});
        sp.pending.push_back({cp, dims_of(own), std::move(chunk)});
    }
    sp.next = upto;
    sp.resident = 0;
    if (sp.run_bytes > run_limit(spill_share_, sp.runs)) spill_pending_ = true;
}

dstd::Vector<dstd::String> BloomFold::extra_captures() const {
    dstd::Vector<dstd::String> out;
    for (const dstd::String& dim : config_.extra_dimensions)
        if (captures_nested() && trace::views::detail::is_nested_path(dim))
            out.push_back(capture_path(dim));
    return out;
}

dstd::Result<void> BloomFold::step(
    const trace::views::detail::FoldBatch& batch) {
    observe(open(batch), batch);
    return {};
}

BloomFold::Lane BloomFold::open(const trace::views::detail::FoldBatch& batch) {
    const dstd::StringView file = batch.unit.file_path;
    if (!last_state_ || last_file_ != file) {
        last_state_ = &files_[file];
        last_file_.assign(file);
    }
    FileState& fs = *last_state_;
    if (fs.index_path.empty())
        fs.index_path.assign(dstd::StringView(batch.unit.index_path));

    const std::uint64_t cp = batch.unit.checkpoint_idx;
    auto it = fs.chunks.find(cp);
    if (it == fs.chunks.end()) {
        it = fs.chunks.emplace(cp, ChunkState{}).first;
        BV::init_chunk_state(it->second, config_, config_.extra_dimensions);
    }
    if (spills() && cp >= SPILL_LAG) chunk_started(fs, cp - SPILL_LAG);
    Lane lane;
    lane.chunk_ = &it->second;
    lane.auto_chunk_ = &fs.auto_chunks[cp];

    for (const auto& e : batch.events)
        if (e.phase != trace::RecordPhase::Metadata)
            observe_shape(fs, e.schema_leaves);
    return lane;
}

void BloomFold::observe(const Lane& lane,
                        const trace::views::detail::FoldBatch& batch) {
    feeder_.feed(*lane.chunk_, *lane.auto_chunk_, batch);
}

void BloomFold::drop_unit(const trace::views::detail::ScanUnit& unit) {
    auto it = files_.find(unit.file_path);
    if (it == files_.end()) return;
    it->second.chunks.erase(unit.checkpoint_idx);
    it->second.auto_chunks.erase(unit.checkpoint_idx);
}

dstd::Result<void> BloomFold::merge(trace::views::detail::Fold& slice) {
    auto& other = static_cast<BloomFold&>(slice);
    last_state_ = nullptr;
    for (auto& [file, ofs] : other.files_) {
        flush_shapes(ofs);
        FileState& fs = files_[file];
        if (fs.index_path.empty()) fs.index_path = ofs.index_path;
        for (auto& [cp, ochunk] : ofs.chunks) {
            auto it = fs.chunks.find(cp);
            if (it == fs.chunks.end()) {
                fs.chunks.emplace(cp, std::move(ochunk));
            } else {
                BV::merge_chunk_state(it->second, ochunk);
            }
        }
        for (auto& [cp, oac] : ofs.auto_chunks) {
            AutoChunk& ac = fs.auto_chunks[cp];
            for (auto& [k, of] : oac) {
                AutoField& f = ac[k];
                BV::merge_dimension_stats(f.stats, of.stats);
                if (f.overflow || of.overflow) {
                    f.overflow = true;
                    f.values.clear();
                    continue;
                }
                f.values.insert(of.values.begin(), of.values.end());
                if (f.values.size() > config_.auto_max_distinct) {
                    f.overflow = true;
                    f.values.clear();
                }
            }
        }
        for (const auto& [path, o] : ofs.paths) {
            auto& st = fs.paths[path];
            st.type = trace::index::store::join(st.type, o.type);
            st.seen |= o.seen;
            st.count += o.count;
        }
    }
    return {};
}

coro::CoroTask<dstd::Result<bool>> BloomFold::finalize(
    const trace::views::detail::CoverageSet& covered) {
    bool wrote = false;

    dstd::HashMap<dstd::String, trace::index::Snapshot> snapshots;
    for (auto& [file, fs] : files_) {
        if (fs.index_path.empty() || fs.chunks.empty()) continue;
        // A chunk missing from a partial read would persist as an empty pruner
        // entry that later reads as "no events".
        if (!covered.covers_file(file)) continue;

        const dstd::String logical = fileio::get_logical_path(file);
        auto at = snapshots.find(dstd::StringView(fs.index_path));
        if (at == snapshots.end()) {
            const auto checked = trace::index::Index::open(
                trace::index::normalize_index_root(fs.index_path));
            if (!checked) {
                DFTRACER_UTILS_LOG_WARN("bloom delta check failed for {}: {}",
                                        fs.index_path,
                                        checked.error().format());
                continue;
            }
            auto taken = checked->snapshot();
            if (!taken) {
                DFTRACER_UTILS_LOG_WARN("bloom delta check failed for {}: {}",
                                        fs.index_path, taken.error().format());
                continue;
            }
            at = snapshots
                     .emplace(dstd::String(fs.index_path), std::move(*taken))
                     .first;
        }
        const trace::index::Snapshot& snapshot = at->second;
        const auto ref = snapshot.file(logical);
        if (!ref ||
            trace::index::extension_current(
                snapshot, *ref, trace::index::store::IndexExtension::Stats))
            continue;

        // A locked or read-only index persists nothing rather than failing the
        // query.
        auto opened = trace::index::Index::open(
            trace::index::normalize_index_root(fs.index_path));
        auto build = opened
                         ? trace::index::rebuild(*opened, logical)
                         : dstd::Expected<trace::index::FileBuild, dstd::Error>(
                               dstd::Unexpected<dstd::Error>(opened.error()));
        dstd::String failure;
        if (build) {
            const auto written = write_file(*build, file, fs);
            if (!written) failure = dstd::String(written.error().message());
        } else {
            failure = dstd::String(build.error().message());
        }
        if (failure.empty()) {
            auto committed = co_await build->commit();
            if (!committed) failure = dstd::String(committed.error().message());
        }
        if (!failure.empty()) {
            DFTRACER_UTILS_LOG_WARN("bloom materialization skipped for {}: {}",
                                    fs.index_path, failure);
            continue;
        }
        wrote = true;
    }

    co_return wrote;
}

dstd::Result<void> BloomFold::write(trace::index::FileBuild& build) {
    if (files_.empty()) return {};
    FileState& fs = files_.begin()->second;
    if (fs.chunks.empty() && fs.spill.next == 0) return {};
    return write_file(build, files_.begin()->first, fs);
}

}  // namespace dftracer::utils::trace::index::extensions
