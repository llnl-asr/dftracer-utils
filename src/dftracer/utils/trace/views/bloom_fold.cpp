#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/trace/views/bloom_fold.h>
#include <dftracer/utils/trace/views/index_write_lock.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <exception>
#include <mutex>
#include <string_view>
#include <variant>
#include <vector>

namespace dftracer::utils::trace::views::detail {

namespace idx = utilities::indexer;

namespace {

using BV = visitors::BloomCore;
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
                     visitors::BloomCore::ChunkIndexerConfig config)
    : intern_(&intern), config_(std::move(config)) {
    extra_keys_.reserve(config_.extra_dimensions.size());
    for (const std::string& dim : config_.extra_dimensions)
        extra_keys_.push_back(intern_->get_or_insert(
            is_nested_path(dim) ? capture_path(dim) : dim));
    auto_skip_.insert(extra_keys_.begin(), extra_keys_.end());
    auto_skip_.insert(intern_->get_or_insert("cmd_hash"));
    auto_skip_.insert(intern_->get_or_insert("exec_hash"));
}

void BloomFold::observe_auto(AutoChunk& chunk, const FoldEvent& e) {
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

std::pair<std::vector<BloomFold::ChunkState>, std::vector<std::string>>
BloomFold::dense_chunks(FileState& fs) {
    std::vector<std::string> dims = config_.extra_dimensions;
    std::map<std::string, std::uint32_t> auto_keys;
    for (const auto& [cp, ac] : fs.auto_chunks)
        for (const auto& [k, f] : ac)
            auto_keys.emplace(std::string(intern_->resolve(k)), k);
    for (const auto& [name, k] : auto_keys) dims.push_back(name);

    // A whole-file read fills every checkpoint, so any gap is an empty chunk
    // for an unobserved member.
    const std::uint64_t max_cp = fs.chunks.rbegin()->first;
    std::vector<ChunkState> chunks(static_cast<std::size_t>(max_cp) + 1);
    for (auto& c : chunks) BV::init_chunk_state(c, config_, dims);
    for (auto& [cp, chunk] : fs.chunks) {
        ChunkState& dst = chunks[static_cast<std::size_t>(cp)];
        std::vector<indexing::ScalableBloomFilter> blooms =
            std::move(dst.extra_blooms);
        std::vector<visitors::BloomCore::ChunkDimensionStats> stats =
            std::move(dst.extra_dim_stats);
        std::vector<std::uint8_t> skip = std::move(dst.extra_bloom_skip);
        dst = std::move(chunk);
        const std::size_t n = config_.extra_dimensions.size();
        for (std::size_t e = 0; e < n; ++e) {
            blooms[e] = std::move(dst.extra_blooms[e]);
            stats[e] = std::move(dst.extra_dim_stats[e]);
            skip[e] = dst.extra_bloom_skip[e];
        }
        auto ac = fs.auto_chunks.find(cp);
        std::size_t e = n;
        for (const auto& [name, k] : auto_keys) {
            AutoField* f = nullptr;
            if (ac != fs.auto_chunks.end()) {
                auto it = ac->second.find(k);
                if (it != ac->second.end()) f = &it->second;
            }
            if (f) {
                stats[e] = std::move(f->stats);
                stats[e].dimension = name;
                if (stats[e].value_type == "string" && !f->overflow) {
                    for (std::uint32_t id : f->values)
                        blooms[e].add(intern_->resolve(id));
                } else {
                    skip[e] = 1;
                }
            }
            ++e;
        }
        dst.extra_blooms = std::move(blooms);
        dst.extra_dim_stats = std::move(stats);
        dst.extra_bloom_skip = std::move(skip);
    }
    fs.auto_chunks.clear();
    return {std::move(chunks), std::move(dims)};
}

std::vector<std::string> BloomFold::extra_captures() const {
    std::vector<std::string> out;
    for (const std::string& dim : config_.extra_dimensions)
        if (is_nested_path(dim)) out.push_back(capture_path(dim));
    return out;
}

void BloomFold::step(const FoldBatch& batch) {
    FileState& fs = files_[batch.unit.file_path];
    if (fs.index_path.empty()) fs.index_path = batch.unit.index_path;

    auto it = fs.chunks.find(batch.unit.checkpoint_idx);
    if (it == fs.chunks.end()) {
        it = fs.chunks.emplace(batch.unit.checkpoint_idx, ChunkState{}).first;
        BV::init_chunk_state(it->second, config_, config_.extra_dimensions);
    }
    ChunkState& chunk = it->second;
    AutoChunk* auto_chunk = config_.auto_fields
                                ? &fs.auto_chunks[batch.unit.checkpoint_idx]
                                : nullptr;

    for (const auto& e : batch.events) {
        if (e.phase == RecordPhase::METADATA) {
            std::string_view name, value;
            for (const auto& [k, v] : e.args) {
                const auto* id = std::get_if<std::uint32_t>(&v);
                if (!id) continue;
                const auto key = intern_->resolve(k);
                if (key == "name")
                    name = intern_->resolve(*id);
                else if (key == "value")
                    value = intern_->resolve(*id);
            }
            BV::observe_metadata(chunk, resolve_or_empty(*intern_, e.name_id),
                                 value, name);
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

        // Groupable columns: every scalar leaf the scan enumerated into
        // schema_leaves (schemaless, arbitrarily nested; pid/tid/ts/dur and the
        // structural keys are excluded there). Harvested once per distinct
        // event name and folded, so this is O(distinct names) with no reparse.
        if (fs.col_seen_names.find(name) == fs.col_seen_names.end()) {
            fs.col_seen_names.emplace(name);
            for (const auto& [leaf_id, tag] : e.schema_leaves) {
                const auto t = static_cast<idx::ColumnType>(tag);
                auto [pos, inserted] =
                    fs.columns.emplace(intern_->resolve(leaf_id), t);
                if (!inserted)
                    pos->second = idx::merge_column_type(pos->second, t);
            }
        }

        BV::observe_data(chunk, fs.pidtid, config_, name, cat, e.pid, e.tid,
                         e.ts, e.dur, e.has_dur, hhash, fhash, shash);

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

void BloomFold::drop_unit(const ScanUnit& unit) {
    auto it = files_.find(unit.file_path);
    if (it == files_.end()) return;
    it->second.chunks.erase(unit.checkpoint_idx);
    it->second.auto_chunks.erase(unit.checkpoint_idx);
}

void BloomFold::merge(Fold& slice) {
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
        for (const auto& [c, t] : ofs.columns) {
            auto [pos, inserted] = fs.columns.emplace(c, t);
            if (!inserted) pos->second = idx::merge_column_type(pos->second, t);
        }
    }
}

coro::CoroTask<bool> BloomFold::finalize(const CoverageSet& covered) {
    bool wrote = false;

    for (auto& [file, fs] : files_) {
        if (fs.index_path.empty() || fs.chunks.empty()) continue;
        // A chunk missing from a partial read would persist as an empty pruner
        // entry that later reads as "no events".
        if (!covered.covers_file(file)) continue;

        int file_id = -1;
        try {
            idx::IndexDatabase ro(fs.index_path, idx::IndexOpenMode::ReadOnly);
            file_id =
                ro.get_file_info_id(idx::internal::get_logical_path(file));
            if (file_id < 0 || ro.has_bloom_data(file_id)) continue;
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("bloom delta check failed for %s: %s",
                                    fs.index_path.c_str(), e.what());
            continue;
        }

        auto [chunks, dims] = dense_chunks(fs);

        std::unique_lock<std::mutex> lk(index_write_mutex(fs.index_path));
        try {
            idx::IndexDatabase db(fs.index_path);
            auto writer = db.begin_write();
            BV::persist_bloom(*writer, file_id, chunks, config_, dims,
                              fs.columns);
            if (config_.auto_fields)
                writer->insert_index_dimension(
                    file_id, std::string(indexing::AUTO_FIELDS_MARKER));
            writer->add_file_capability(file_id,
                                        idx::IndexFileEntryCapability::BLOOM);
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

void BloomFold::write_to_sink(
    dftracer::utils::utilities::indexer::IndexBatchSink& sink, int file_id) {
    if (files_.empty()) return;
    FileState& fs = files_.begin()->second;
    if (fs.chunks.empty()) return;
    auto [chunks, dims] = dense_chunks(fs);
    BV::persist_bloom_sink(sink, file_id, chunks, config_, dims, fs.columns);
    if (config_.auto_fields)
        sink.insert_index_dimension(file_id,
                                    std::string(indexing::AUTO_FIELDS_MARKER));
}

}  // namespace dftracer::utils::trace::views::detail
