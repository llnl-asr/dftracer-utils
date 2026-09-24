#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/index/build/index_write_lock.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/helpers.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <mutex>
#include <string_view>
#include <variant>
#include <vector>

namespace dftracer::utils::index::extensions {

namespace {

using BV = index::schemas::dft::BloomCore;
constexpr std::uint32_t NO_ID = dftracer::utils::StringIntern::NO_ID;

std::string_view resolve_or_empty(const dftracer::utils::StringIntern& intern,
                                  std::uint32_t id) {
    return id == NO_ID ? std::string_view{} : intern.resolve(id);
}

}  // namespace

namespace {

std::string capture_path(const std::string& dim) { return "args." + dim; }

}  // namespace

BloomFold::BloomFold(dftracer::utils::StringIntern& intern,
                     index::schemas::dft::BloomCore::ChunkIndexerConfig config)
    : intern_(&intern),
      config_(std::move(config)),
      empty_bloom_bytes_(ScalableBloomFilter(config_.expected_entries_per_chunk,
                                             config_.false_positive_rate)
                             .size_bytes()) {
    extra_keys_.reserve(config_.extra_dimensions.size());
    for (const std::string& dim : config_.extra_dimensions)
        extra_keys_.push_back(intern_->get_or_insert(
            captures_nested() && trace::views::detail::is_nested_path(dim)
                ? capture_path(dim)
                : dim));
    auto_skip_.insert(extra_keys_.begin(), extra_keys_.end());
    for (const std::string& dim : config_.extra_dimensions)
        auto_skip_.insert(intern_->get_or_insert(dim));
    auto_skip_.insert(intern_->get_or_insert("cmd_hash"));
    auto_skip_.insert(intern_->get_or_insert("exec_hash"));
    // An args key named like a fixed dimension would be written under the
    // fixed dimension's path and replace its data.
    if (config_.fixed_dimensions)
        for (std::string_view dim : BV::fixed_dimension_names())
            auto_skip_.insert(intern_->get_or_insert(dim));
    dur_key_ = intern_->get_or_insert("dur");
}

void BloomFold::observe_path(index::store::PathStat& stat, std::uint8_t tag) {
    const auto t = static_cast<index::store::PathType>(tag);
    stat.type = index::store::join(stat.type, t);
    stat.seen |= static_cast<std::uint8_t>(1U << tag);
    if (t != index::store::PathType::NULL_VALUE) ++stat.count;
}

BloomFold::Catalog BloomFold::catalog(const FileState& fs) const {
    Catalog out;
    out.reserve(fs.paths.size());
    for (const auto& [id, stat] : fs.paths)
        out.emplace_back(intern_->resolve(id), stat);
    return out;
}

void BloomFold::observe_auto(AutoChunk& chunk,
                             const trace::views::detail::FoldEvent& e) {
    for (const auto& [k, v] : e.args) {
        if (auto_skip_.count(k)) continue;
        AutoField& f = chunk[k];
        if (const auto* i = std::get_if<std::int64_t>(&v)) {
            BV::observe_value(f.stats, *i);
        } else if (const auto* d = std::get_if<double>(&v)) {
            BV::observe_value(f.stats, *d);
        } else {
            const std::uint32_t id = std::get<std::uint32_t>(v);
            BV::observe_value(f.stats, intern_->resolve(id));
            if (!f.overflow) {
                f.values.insert(id);
                if (f.values.size() > config_.auto_max_distinct) {
                    f.overflow = true;
                    f.values.clear();
                }
            }
        }
    }
}

void BloomFold::select_auto_keys(
    const FileState& fs, std::map<std::string, std::uint32_t>& keys) const {
    if (keys.size() <= config_.path_budget) return;
    std::vector<std::pair<std::uint64_t, const std::string*>> ranked;
    ranked.reserve(keys.size());
    for (const auto& [name, k] : keys) {
        auto it =
            fs.paths.find(intern_->get_or_insert(config_.auto_prefix + name));
        ranked.emplace_back(it == fs.paths.end() ? 0 : it->second.count, &name);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : *a.second < *b.second;
    });
    std::map<std::string, std::uint32_t> kept;
    for (std::size_t i = 0; i < config_.path_budget; ++i) {
        auto node = keys.extract(*ranked[i].second);
        kept.insert(std::move(node));
    }
    keys = std::move(kept);
}

BloomFold::ChunkState BloomFold::assemble_chunk(
    FileState& fs, std::uint64_t cp, const std::vector<std::string>& dims,
    const AutoKeys& keys) {
    ChunkState out;
    BV::init_chunk_state(out, config_, dims);
    auto base = fs.chunks.find(cp);
    if (base != fs.chunks.end()) {
        auto blooms = std::move(out.extra_blooms);
        auto stats = std::move(out.extra_dim_stats);
        auto skip = std::move(out.extra_bloom_skip);
        out = std::move(base->second);
        fs.chunks.erase(base);
        for (std::size_t e = 0; e < config_.extra_dimensions.size(); ++e) {
            blooms[e] = std::move(out.extra_blooms[e]);
            stats[e] = std::move(out.extra_dim_stats[e]);
            skip[e] = out.extra_bloom_skip[e];
        }
        out.extra_blooms = std::move(blooms);
        out.extra_dim_stats = std::move(stats);
        out.extra_bloom_skip = std::move(skip);
    }
    auto ac = fs.auto_chunks.find(cp);
    if (ac == fs.auto_chunks.end()) return out;
    std::size_t e = config_.extra_dimensions.size();
    for (const auto& [name, k] : keys) {
        auto it = ac->second.find(k);
        if (it != ac->second.end()) {
            AutoField& f = it->second;
            out.extra_dim_stats[e] = std::move(f.stats);
            out.extra_dim_stats[e].dimension = name;
            if (out.extra_dim_stats[e].value_type == "string" && !f.overflow) {
                for (std::uint32_t id : f.values)
                    out.extra_blooms[e].add(intern_->resolve(id));
            } else {
                out.extra_bloom_skip[e] = 1;
            }
        }
        ++e;
    }
    fs.auto_chunks.erase(ac);
    return out;
}

BloomFold::AutoKeys BloomFold::auto_keys(const FileState& fs,
                                         bool select) const {
    std::map<std::string, std::uint32_t> keys;
    for (const auto& [cp, ac] : fs.auto_chunks)
        for (const auto& [k, f] : ac)
            keys.emplace(std::string(intern_->resolve(k)), k);
    for (std::uint32_t k : fs.spill.key_union)
        keys.emplace(std::string(intern_->resolve(k)), k);
    if (select) select_auto_keys(fs, keys);
    return AutoKeys(keys.begin(), keys.end());
}

std::vector<std::string> BloomFold::dims_of(const AutoKeys& keys) const {
    std::vector<std::string> dims = config_.extra_dimensions;
    for (const auto& [name, k] : keys) dims.push_back(name);
    return dims;
}

void BloomFold::write_file(index::store::IndexWrite& w, int file_id,
                           FileState& fs) {
    const AutoKeys keys = auto_keys(fs, true);
    const auto dims = dims_of(keys);
    for (auto ext : BV::tier_extensions(config_))
        index::store::records::clear_file(w, ext, file_id);
    BV::FileAccumulator acc(config_);
    // A whole-file read fills every checkpoint, so any gap is an empty chunk
    // for an unobserved member. One chunk is assembled at a time.
    const std::uint64_t end = fs.chunks.rbegin()->first + 1;
    for (std::uint64_t cp = 0; cp < end; ++cp)
        BV::write_chunk(w, file_id, cp, assemble_chunk(fs, cp, dims, keys),
                        dims, config_, acc);
    fs.auto_chunks.clear();
    BV::finish_file(w, file_id, acc, config_, dims, catalog(fs));
}

std::uint64_t BloomFold::resident_bytes(const FileState& fs,
                                        std::uint64_t cp) const {
    // Held bytes plus what the chunk's records take in a run buffer; a
    // chunk bloom is sized for expected_entries whatever it holds.
    constexpr std::uint64_t ENTRY = 64;
    constexpr std::uint64_t SKETCH = 1024;
    std::uint64_t n = 0;
    auto dim = [&](const auto& ds) {
        n += sizeof(ds) + ds.min_value.capacity() + ds.max_value.capacity();
        if (ds.value_counts) n += ds.value_counts->size() * ENTRY * 2;
    };
    if (auto it = fs.chunks.find(cp); it != fs.chunks.end()) {
        const ChunkState& c = it->second;
        n += sizeof(ChunkState);
        for (const auto& b : c.fixed_blooms) n += 2 * b.size_bytes();
        for (const auto& b : c.extra_blooms) n += 2 * b.size_bytes();
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
            if (!f.overflow && f.stats.value_type == "string")
                n += 2 * empty_bloom_bytes_;
        }
    return n;
}

void BloomFold::enable_spill(std::uint64_t share, std::string dir,
                             int file_id) {
    spill_share_ = share;
    spill_dir_ = std::move(dir);
    spill_file_id_ = file_id;
}

std::size_t BloomFold::spill_runs() const {
    return files_.empty() ? 0 : files_.begin()->second.spill.runs.size();
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
    auto open_run = [&] {
        return std::make_unique<index::store::IndexDatabaseSstWriterContext>(
            spill_dir_, "run_" + std::to_string(sp.runs.size()));
    };
    auto run = open_run();
    if (sp.runs.empty())
        for (auto ext : BV::tier_extensions(config_))
            index::store::records::clear_file(*run, ext, spill_file_id_);
    std::uint64_t buffered = 0;
    for (std::uint64_t cp = sp.next; cp < upto; ++cp) {
        buffered += resident_bytes(fs, cp);
        AutoKeys own;
        if (auto ac = fs.auto_chunks.find(cp); ac != fs.auto_chunks.end()) {
            for (const auto& [k, f] : ac->second)
                own.emplace_back(std::string(intern_->resolve(k)), k);
            std::sort(own.begin(), own.end());
        }
        const ChunkState chunk = assemble_chunk(fs, cp, dims_of(own), own);
        BV::write_chunk(*run, spill_file_id_, cp, chunk, dims_of(own), config_,
                        *sp.acc);
        sp.events += chunk.statistics.total_events;
        std::vector<std::uint32_t> ids;
        ids.reserve(own.size());
        for (const auto& [name, k] : own) {
            ids.push_back(k);
            sp.key_union.insert(k);
        }
        sp.keys.emplace_back(cp, std::move(ids));
        if (buffered > spill_share_ / 4 && cp + 1 < upto) {
            sp.runs.push_back(run->commit());
            run = open_run();
            buffered = 0;
        }
    }
    sp.runs.push_back(run->commit());
    sp.next = upto;
    sp.resident = 0;
}

std::vector<index::store::IndexDatabaseSstWriterContext::Artifacts>
BloomFold::finish_spilled(int file_id) {
    FileState& fs = files_.begin()->second;
    auto& sp = fs.spill;
    const AutoKeys keys = auto_keys(fs, true);
    const auto dims = dims_of(keys);
    index::store::IndexDatabaseSstWriterContext last(
        spill_dir_, "run_" + std::to_string(sp.runs.size()));

    const std::uint64_t end =
        fs.chunks.empty() ? sp.next
                          : std::max(sp.next, fs.chunks.rbegin()->first + 1);
    for (std::uint64_t cp = sp.next; cp < end; ++cp)
        BV::write_chunk(last, file_id, cp, assemble_chunk(fs, cp, dims, keys),
                        dims, config_, *sp.acc);

    ankerl::unordered_dense::set<std::uint32_t> kept;
    for (const auto& [name, k] : keys) kept.insert(k);
    for (const auto& [cp, ids] : sp.keys) {
        ankerl::unordered_dense::set<std::uint32_t> had(ids.begin(), ids.end());
        for (const auto& [name, k] : keys)
            if (!had.contains(k))
                BV::write_absent_extra(last, file_id, cp, name, config_,
                                       *sp.acc);
    }
    // The runs hold every candidate path; a later ingest file's range delete
    // covers them, since each file of one ingest gets its own sequence number.
    for (std::uint32_t k : sp.key_union)
        if (!kept.contains(k))
            for (auto ext : {index::store::IndexExtension::ZONEMAP,
                             index::store::IndexExtension::BLOOM,
                             index::store::IndexExtension::COUNTS})
                index::store::records::clear_path(last, ext, file_id,
                                                  intern_->resolve(k));
    BV::finish_file(last, file_id, *sp.acc, config_, dims, catalog(fs));
    sp.runs.push_back(last.commit());
    sp.handed_off = true;
    return std::move(sp.runs);
}

BloomFold::~BloomFold() {
    if (spill_dir_.empty()) return;
    for (const auto& [file, fs] : files_)
        if (!fs.spill.runs.empty() && !fs.spill.handed_off) {
            std::error_code ec;
            fs::remove_all(spill_dir_, ec);
        }
}

std::vector<std::string> BloomFold::extra_captures() const {
    std::vector<std::string> out;
    for (const std::string& dim : config_.extra_dimensions)
        if (captures_nested() && trace::views::detail::is_nested_path(dim))
            out.push_back(capture_path(dim));
    return out;
}

void BloomFold::step(const trace::views::detail::FoldBatch& batch) {
    FileState& fs = files_[batch.unit.file_path];
    if (fs.index_path.empty()) fs.index_path = batch.unit.index_path;

    auto it = fs.chunks.find(batch.unit.checkpoint_idx);
    if (it == fs.chunks.end()) {
        it = fs.chunks.emplace(batch.unit.checkpoint_idx, ChunkState{}).first;
        BV::init_chunk_state(it->second, config_, config_.extra_dimensions);
    }
    ChunkState& chunk = it->second;
    if (!spill_dir_.empty()) chunk_started(fs, batch.unit.checkpoint_idx);
    AutoChunk* auto_chunk = config_.path_budget > 0
                                ? &fs.auto_chunks[batch.unit.checkpoint_idx]
                                : nullptr;

    for (const auto& e : batch.events) {
        if (e.phase == trace::RecordPhase::METADATA) {
            BV::observe_metadata(chunk, resolve_or_empty(*intern_, e.name_id));
            for (const char* p : {"name", "ph", "pid", "tid", "ts"})
                BV::observe_metadata_path(chunk, p);
            if (e.has_dur) BV::observe_metadata_path(chunk, "dur");
            if (e.cat_id != NO_ID) BV::observe_metadata_path(chunk, "cat");
            // A hash arg is parsed into its own field, off e.args.
            if (e.fhash_id != NO_ID) BV::observe_metadata_path(chunk, "fhash");
            if (e.hhash_id != NO_ID) BV::observe_metadata_path(chunk, "hhash");
            for (const auto& [k, v] : e.args)
                BV::observe_metadata_path(
                    chunk, "args." + std::string(intern_->resolve(k)));
            continue;
        }

        const std::string_view name = resolve_or_empty(*intern_, e.name_id);
        const std::string_view cat = resolve_or_empty(*intern_, e.cat_id);
        const std::string_view hhash = resolve_or_empty(*intern_, e.hhash_id);
        const std::string_view fhash = resolve_or_empty(*intern_, e.fhash_id);

        std::string_view shash;
        for (const auto& [k, v] : e.args) {
            const auto* id = std::get_if<std::uint32_t>(&v);
            if (!id) continue;
            const auto key = intern_->resolve(k);
            if (key == "cmd_hash") {
                shash = intern_->resolve(*id);
                break;
            }
            if (key == "exec_hash" && shash.empty())
                shash = intern_->resolve(*id);
        }

        for (const auto& [leaf_id, tag] : e.schema_leaves)
            observe_path(fs.paths[leaf_id], tag);

        if (config_.fixed_dimensions) {
            BV::observe_data(chunk, fs.pidtid, name, cat, e.pid, e.tid, e.ts,
                             e.dur, e.has_dur, hhash, fhash, shash);
            // A record without a top-level dur (an aggregated one) matches
            // `dur` on args.dur, so the dur zone covers it too.
            if (!e.has_dur)
                for (const auto& [k, v] : e.args) {
                    if (k != dur_key_) continue;
                    auto& ds = chunk.fixed_dim_stats[BV::FD_DUR];
                    // Durations are not negative, so the uint range holds.
                    if (const auto* i = std::get_if<std::int64_t>(&v);
                        i && *i >= 0)
                        ds.observe_range_only(static_cast<std::uint64_t>(*i));
                    if (const auto* d = std::get_if<double>(&v); d && *d >= 0) {
                        ds.observe_range_only(
                            static_cast<std::uint64_t>(std::floor(*d)));
                        ds.observe_range_only(
                            static_cast<std::uint64_t>(std::ceil(*d)));
                    }
                    break;
                }
        } else
            ++chunk.statistics.total_events;

        for (std::size_t x = 0; x < extra_keys_.size(); ++x) {
            for (const auto& [k, v] : e.args) {
                if (k != extra_keys_[x]) continue;
                if (const auto* i = std::get_if<std::int64_t>(&v))
                    BV::observe_extra(chunk, x, *i);
                else if (const auto* d = std::get_if<double>(&v))
                    BV::observe_extra(chunk, x, *d);
                else
                    BV::observe_extra(
                        chunk, x, intern_->resolve(std::get<std::uint32_t>(v)));
                break;
            }
        }
        if (auto_chunk) observe_auto(*auto_chunk, e);
    }
}

void BloomFold::drop_unit(const trace::views::detail::ScanUnit& unit) {
    auto it = files_.find(unit.file_path);
    if (it == files_.end()) return;
    it->second.chunks.erase(unit.checkpoint_idx);
    it->second.auto_chunks.erase(unit.checkpoint_idx);
}

void BloomFold::merge(trace::views::detail::Fold& slice) {
    auto& other = static_cast<BloomFold&>(slice);
    for (auto& [file, ofs] : other.files_) {
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
            st.type = index::store::join(st.type, o.type);
            st.seen |= o.seen;
            st.count += o.count;
        }
    }
}

coro::CoroTask<bool> BloomFold::finalize(
    const trace::views::detail::CoverageSet& covered) {
    bool wrote = false;

    for (auto& [file, fs] : files_) {
        if (fs.index_path.empty() || fs.chunks.empty()) continue;
        // A chunk missing from a partial read would persist as an empty pruner
        // entry that later reads as "no events".
        if (!covered.covers_file(file)) continue;

        int file_id = -1;
        try {
            index::store::IndexDatabase ro(
                fs.index_path, index::store::IndexOpenMode::ReadOnly);
            file_id = ro.get_file_info_id(
                index::store::internal::get_logical_path(file));
            if (file_id < 0 || ro.pruning_tier_current(file_id)) continue;
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("bloom delta check failed for %s: %s",
                                    fs.index_path.c_str(), e.what());
            continue;
        }

        std::unique_lock<std::mutex> lk(
            index::build::index_write_mutex(fs.index_path));
        try {
            index::store::IndexDatabase db(fs.index_path);
            auto writer = db.begin_write();
            write_file(*writer, file_id, fs);
            writer->commit();
            wrote = true;
        } catch (const std::exception& e) {
            // A locked or read-only index persists nothing rather than failing
            // the query.
            DFTRACER_UTILS_LOG_WARN("bloom materialization skipped for %s: %s",
                                    fs.index_path.c_str(), e.what());
        }
    }

    co_return wrote;
}

void BloomFold::write(index::store::IndexWrite& w, int file_id) {
    if (files_.empty()) return;
    FileState& fs = files_.begin()->second;
    if (fs.chunks.empty()) return;
    write_file(w, file_id, fs);
}

}  // namespace dftracer::utils::index::extensions
