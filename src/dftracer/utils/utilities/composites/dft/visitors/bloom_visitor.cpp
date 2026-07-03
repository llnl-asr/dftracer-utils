#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/utilities/composites/dft/args_map.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/composites/dft/visitors/bloom_visitor.h>
#include <dftracer/utils/utilities/composites/dft/visitors/visitor_dom_helpers.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>

#include <charconv>
#include <cstring>
#include <string>

using dftracer::utils::utilities::composites::dft::indexing::BloomFilter;
namespace dftracer::utils::utilities::composites::dft::visitors {

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

constexpr std::array<std::string_view, BloomVisitor::BF_COUNT>
    FIXED_BLOOM_NAMES = {DIM_NAME,  DIM_CAT,   DIM_PID,  DIM_TID,
                         DIM_HHASH, DIM_FHASH, DIM_SHASH};

constexpr std::array<std::string_view, BloomVisitor::FD_COUNT> FIXED_DIM_NAMES =
    {DIM_NAME,  DIM_CAT,   DIM_PID,   DIM_TID, DIM_PID_TID,
     DIM_HHASH, DIM_FHASH, DIM_SHASH, DIM_TS,  DIM_DUR};

int fixed_bloom_index(std::string_view name) {
    for (std::size_t i = 0; i < FIXED_BLOOM_NAMES.size(); ++i) {
        if (FIXED_BLOOM_NAMES[i] == name) return static_cast<int>(i);
    }
    return -1;
}

bool dom_value_to_string(simdjson::dom::element val, std::string& out) {
    out.clear();
    if (val.is_string()) {
        auto sv = val.get_string().value_unsafe();
        out.assign(sv.data(), sv.size());
        return !out.empty();
    }
    char buf[32];
    if (val.is_uint64()) {
        auto [p, _] = std::to_chars(buf, buf + sizeof(buf),
                                    val.get_uint64().value_unsafe());
        out.assign(buf, p);
        return true;
    }
    if (val.is_int64()) {
        auto [p, _] = std::to_chars(buf, buf + sizeof(buf),
                                    val.get_int64().value_unsafe());
        out.assign(buf, p);
        return true;
    }
    if (val.is_double()) {
        char* p = dftracer::utils::to_chars_double(
            buf, buf + sizeof(buf), val.get_double().value_unsafe());
        if (!p) return false;
        out.assign(buf, p);
        return true;
    }
    if (val.is_bool()) {
        out = val.get_bool().value_unsafe() ? "true" : "false";
        return true;
    }
    return false;
}

/// Emit bloom/stats/dimension records to a sink that might be either a
/// RocksDB-backed writer or an SST file emitter. Returns the accumulated
/// file-level statistics so downstream callers can use them for name
/// postings and root-summary refresh on the concrete writer.
BloomVisitor::ChunkStatistics persist_bloom_sink_writes(
    indexer::IndexBatchSink& db, int file_id,
    const std::vector<std::string>& extra_dim_names,
    const std::vector<BloomVisitor::ChunkState>& chunks,
    const BloomVisitor::ChunkIndexerConfig& config) {
    BloomVisitor::ChunkStatistics file_statistics;

    // Accumulate file-level blooms per slot.
    std::array<BloomFilter, BloomVisitor::BF_COUNT> file_fixed_blooms = {
        BloomFilter(config.expected_entries_per_chunk,
                    config.false_positive_rate),
        BloomFilter(config.expected_entries_per_chunk,
                    config.false_positive_rate),
        BloomFilter(config.expected_entries_per_chunk,
                    config.false_positive_rate),
        BloomFilter(config.expected_entries_per_chunk,
                    config.false_positive_rate),
        BloomFilter(config.expected_entries_per_chunk,
                    config.false_positive_rate),
        BloomFilter(config.expected_entries_per_chunk,
                    config.false_positive_rate),
        BloomFilter(config.expected_entries_per_chunk,
                    config.false_positive_rate),
    };
    std::vector<BloomFilter> file_extra_blooms;
    file_extra_blooms.reserve(extra_dim_names.size());
    for (std::size_t i = 0; i < extra_dim_names.size(); ++i) {
        file_extra_blooms.emplace_back(config.expected_entries_per_chunk,
                                       config.false_positive_rate);
    }

    std::vector<unsigned char> blob;

    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk = chunks[i];
        auto checkpoint_idx = static_cast<std::uint64_t>(i);

        // Fixed blooms
        for (std::size_t b = 0; b < BloomVisitor::BF_COUNT; ++b) {
            const BloomFilter& bf = chunk.fixed_blooms[b];
            bf.serialize_into(blob);
            db.insert_chunk_bloom_filter(
                file_id, checkpoint_idx, std::string(FIXED_BLOOM_NAMES[b]),
                std::span<const unsigned char>(blob.data(), blob.size()),
                static_cast<std::uint64_t>(bf.num_entries()));
            file_fixed_blooms[b].merge_from(bf);
        }
        // Extra blooms
        for (std::size_t e = 0;
             e < extra_dim_names.size() && e < chunk.extra_blooms.size(); ++e) {
            const BloomFilter& bf = chunk.extra_blooms[e];
            bf.serialize_into(blob);
            db.insert_chunk_bloom_filter(
                file_id, checkpoint_idx, extra_dim_names[e],
                std::span<const unsigned char>(blob.data(), blob.size()),
                static_cast<std::uint64_t>(bf.num_entries()));
            file_extra_blooms[e].merge_from(bf);
        }

        db.insert_chunk_statistics(file_id, checkpoint_idx, chunk.statistics);
        file_statistics.merge_from(chunk.statistics);

        // Fixed dim_stats
        for (std::size_t d = 0; d < BloomVisitor::FD_COUNT; ++d) {
            db.insert_chunk_dimension_stats(file_id, checkpoint_idx,
                                            chunk.fixed_dim_stats[d],
                                            config.value_counts_cap);
        }
        // Extra dim_stats
        for (const auto& ds : chunk.extra_dim_stats) {
            db.insert_chunk_dimension_stats(file_id, checkpoint_idx, ds,
                                            config.value_counts_cap);
        }
    }

    // File-level blooms
    for (std::size_t b = 0; b < BloomVisitor::BF_COUNT; ++b) {
        const BloomFilter& bf = file_fixed_blooms[b];
        bf.serialize_into(blob);
        db.insert_file_bloom_filter(
            file_id, std::string(FIXED_BLOOM_NAMES[b]),
            std::span<const unsigned char>(blob.data(), blob.size()),
            static_cast<std::uint64_t>(bf.num_entries()));
    }
    for (std::size_t e = 0; e < extra_dim_names.size(); ++e) {
        const BloomFilter& bf = file_extra_blooms[e];
        bf.serialize_into(blob);
        db.insert_file_bloom_filter(
            file_id, extra_dim_names[e],
            std::span<const unsigned char>(blob.data(), blob.size()),
            static_cast<std::uint64_t>(bf.num_entries()));
    }

    for (std::size_t b = 0; b < BloomVisitor::BF_COUNT; ++b) {
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

    // Name dictionary + postings. name_id is a pure FNV1a hash of the name
    // so this is safe on any sink backend (RocksDB or SST). The dictionary
    // entries are idempotent; duplicate inserts across workers are folded
    // together at ingest time via `ingest_behind=true`.
    std::unordered_map<std::string, std::uint64_t> file_name_ids;
    file_name_ids.reserve(file_statistics.name_counts.size());
    for (const auto& [name, _] : file_statistics.name_counts) {
        const auto name_id = hash::fnv1a_hash(name);
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
    indexer::IndexDatabaseWriterContext& db, int file_id,
    const BloomVisitor::ChunkStatistics& file_statistics,
    std::size_t num_chunks, bool refresh_root_summaries) {
    if (!refresh_root_summaries) return;
    const bool had_existing_file_summary = db.has_file_scalar_stats(file_id);
    db.refresh_root_summaries_after_file_write(
        file_id, file_statistics, num_chunks, had_existing_file_summary);
}

}  // namespace

BloomVisitor::ChunkState::ChunkState() = default;

BloomVisitor::BloomVisitor(ChunkIndexerConfig config,
                           std::vector<std::string> dimensions) {
    config_ = std::move(config);
    // `dimensions` historically includes fixed + extras. Extract extras only
    // (anything not matching a fixed bloom slot).
    for (auto& dim : dimensions) {
        if (fixed_bloom_index(dim) < 0) {
            extra_dim_names_.push_back(std::move(dim));
        }
    }
    // Also pick up config_.extra_dimensions (kept for backwards compat with
    // callers that set extras there and pass only defaults in `dimensions`).
    for (const auto& dim : config_.extra_dimensions) {
        bool already = false;
        for (const auto& e : extra_dim_names_) {
            if (e == dim) {
                already = true;
                break;
            }
        }
        if (!already && fixed_bloom_index(dim) < 0) {
            extra_dim_names_.push_back(dim);
        }
    }
}

void BloomVisitor::begin(std::size_t /*num_checkpoints*/) {
    chunks_.clear();
    chunks_base_idx_ = 0;
    file_acc_.extra_blooms.clear();
    file_acc_.statistics = ChunkStatistics{};
    file_acc_.num_chunks_emitted = 0;
    file_acc_.initialized = false;
}

void BloomVisitor::on_checkpoint(std::size_t /*checkpoint_idx*/) {}

void BloomVisitor::ensure_chunk(std::size_t checkpoint_idx) {
    if (checkpoint_idx < chunks_base_idx_) return;
    const std::size_t local = checkpoint_idx - chunks_base_idx_;
    if (local < chunks_.size()) return;
    auto old_size = chunks_.size();
    chunks_.resize(local + 1);
    for (std::size_t i = old_size; i < chunks_.size(); ++i) {
        auto& chunk = chunks_[i];
        // Initialize fixed blooms with configured params.
        for (std::size_t b = 0; b < BF_COUNT; ++b) {
            chunk.fixed_blooms[b] =
                BloomFilter(config_.expected_entries_per_chunk,
                            config_.false_positive_rate);
        }
        // Initialize fixed dim_stats metadata.
        for (std::size_t d = 0; d < FD_COUNT; ++d) {
            auto& ds = chunk.fixed_dim_stats[d];
            ds.dimension = std::string(FIXED_DIM_NAMES[d]);
            ds.value_type =
                (d == FD_PID || d == FD_TID || d == FD_TS || d == FD_DUR)
                    ? "uint"
                    : "string";
        }
        // Initialize extras.
        chunk.extra_blooms.clear();
        chunk.extra_dim_stats.clear();
        chunk.extra_blooms.reserve(extra_dim_names_.size());
        chunk.extra_dim_stats.resize(extra_dim_names_.size());
        for (std::size_t e = 0; e < extra_dim_names_.size(); ++e) {
            chunk.extra_blooms.emplace_back(config_.expected_entries_per_chunk,
                                            config_.false_positive_rate);
            chunk.extra_dim_stats[e].dimension = extra_dim_names_[e];
            chunk.extra_dim_stats[e].value_type = "string";
        }
    }
}

void BloomVisitor::on_event(const EventRecord& record) {
    if (record.checkpoint_idx < chunks_base_idx_) return;
    ensure_chunk(record.checkpoint_idx);

    const auto& ev = record.ev;
    ChunkState& chunk = chunks_[record.checkpoint_idx - chunks_base_idx_];

    if (ev.is_metadata()) {
        if (record.has_args) {
            std::string_view hash_val = dom_string(record.args_dom, "value");
            std::string_view resolved = dom_string(record.args_dom, "name");

            if (!hash_val.empty() && !resolved.empty()) {
                std::string_view dim;
                if (ev.name == "HH") {
                    dim = DIM_HHASH;
                } else if (ev.name == "FH") {
                    dim = DIM_FHASH;
                } else if (ev.name == "SH") {
                    dim = DIM_SHASH;
                }
                if (!dim.empty()) {
                    // Outer StringViewMap: transparent find, emplace on miss.
                    auto outer_it = chunk.hash_resolutions.find(dim);
                    if (outer_it == chunk.hash_resolutions.end()) {
                        outer_it = chunk.hash_resolutions
                                       .emplace(std::string(dim),
                                                StringViewMap<std::string>{})
                                       .first;
                    }
                    auto& inner = outer_it->second;
                    // Inner StringViewMap: find + emplace/update.
                    auto inner_it = inner.find(hash_val);
                    if (inner_it == inner.end()) {
                        inner.emplace(std::string(hash_val),
                                      std::string(resolved));
                    } else {
                        inner_it->second.assign(resolved.data(),
                                                resolved.size());
                    }
                }
            }
        }
    } else {
        chunk.statistics.update_from_event(ev.name, ev.cat, ev.pid, ev.tid,
                                           ev.ts, ev.dur);

        // Observe a fixed slot: adds to bloom (if bloom_idx >= 0) and to
        // dim_stats.
        auto observe_fixed = [&chunk](int bloom_idx, std::size_t dim_idx,
                                      std::string_view val) {
            if (val.empty()) return;
            if (bloom_idx >= 0) {
                chunk.fixed_blooms[bloom_idx].add(val);
            }
            chunk.fixed_dim_stats[dim_idx].observe(val);
        };

        observe_fixed(BF_NAME, FD_NAME, ev.name);
        observe_fixed(BF_CAT, FD_CAT, ev.cat);

        if (ev.pid != last_pid_ || last_pid_len_ == 0) {
            auto [pp, _1] = std::to_chars(
                last_pid_buf_, last_pid_buf_ + sizeof(last_pid_buf_), ev.pid);
            last_pid_len_ = static_cast<std::uint8_t>(pp - last_pid_buf_);
            last_pid_ = ev.pid;
        }
        if (ev.tid != last_tid_ || last_tid_len_ == 0) {
            auto [tp, _2] = std::to_chars(
                last_tid_buf_, last_tid_buf_ + sizeof(last_tid_buf_), ev.tid);
            last_tid_len_ = static_cast<std::uint8_t>(tp - last_tid_buf_);
            last_tid_ = ev.tid;
        }
        std::string_view pid_sv(last_pid_buf_, last_pid_len_);
        std::string_view tid_sv(last_tid_buf_, last_tid_len_);

        observe_fixed(BF_PID, FD_PID, pid_sv);
        observe_fixed(BF_TID, FD_TID, tid_sv);

        char pt_buf[52];
        std::memcpy(pt_buf, last_pid_buf_, last_pid_len_);
        pt_buf[last_pid_len_] = ':';
        std::memcpy(pt_buf + last_pid_len_ + 1, last_tid_buf_, last_tid_len_);
        std::string_view pt_sv(pt_buf, last_pid_len_ + 1 + last_tid_len_);
        // pid_tid has no bloom slot — only dim_stats.
        observe_fixed(-1, FD_PID_TID, pt_sv);

        chunk.fixed_dim_stats[FD_TS].observe_range_only(ev.ts);
        chunk.fixed_dim_stats[FD_DUR].observe_range_only(ev.dur);

        if (record.has_args) {
            std::string_view hhash = dom_string(record.args_dom, "hhash");
            observe_fixed(BF_HHASH, FD_HHASH, hhash);

            std::string_view fhash = dom_string(record.args_dom, "fhash");
            observe_fixed(BF_FHASH, FD_FHASH, fhash);

            std::string_view shash = dom_string(record.args_dom, "cmd_hash");
            if (shash.empty()) {
                shash = dom_string(record.args_dom, "exec_hash");
            }
            observe_fixed(BF_SHASH, FD_SHASH, shash);

            std::string scratch;
            for (std::size_t e = 0; e < extra_dim_names_.size(); ++e) {
                auto r = record.args_dom[extra_dim_names_[e]];
                if (r.error()) continue;
                if (dom_value_to_string(r.value_unsafe(), scratch) &&
                    !scratch.empty()) {
                    chunk.extra_blooms[e].add(scratch);
                    chunk.extra_dim_stats[e].observe(scratch);
                }
            }
        }

        chunk.events_processed++;
    }
}

std::unique_ptr<DftEventVisitor> BloomVisitor::create_parallel_slice() const {
    std::vector<std::string> dims;
    dims.reserve(BloomVisitor::BF_COUNT + extra_dim_names_.size());
    for (auto sv : FIXED_BLOOM_NAMES) dims.emplace_back(sv);
    for (const auto& d : extra_dim_names_) dims.push_back(d);
    return std::make_unique<BloomVisitor>(config_, std::move(dims));
}

void BloomVisitor::merge_parallel_slice(DftEventVisitor& slice_base) {
    auto* slice = dynamic_cast<BloomVisitor*>(&slice_base);
    if (!slice) return;

    for (std::size_t slice_i = chunks_base_idx_;
         slice_i < slice->chunks_.size(); ++slice_i) {
        auto& src = slice->chunks_[slice_i];
        if (src.events_processed == 0) continue;
        const std::size_t parent_local = slice_i - chunks_base_idx_;
        ensure_chunk(slice_i);
        auto& dst = chunks_[parent_local];

        for (std::size_t b = 0; b < BF_COUNT; ++b) {
            dst.fixed_blooms[b].merge_from(src.fixed_blooms[b]);
        }
        for (std::size_t e = 0;
             e < src.extra_blooms.size() && e < dst.extra_blooms.size(); ++e) {
            dst.extra_blooms[e].merge_from(src.extra_blooms[e]);
        }

        for (std::size_t d = 0; d < FD_COUNT; ++d) {
            auto& sds = src.fixed_dim_stats[d];
            auto& dds = dst.fixed_dim_stats[d];
            if (sds.value_counts) {
                if (!dds.value_counts) dds.value_counts.emplace();
                for (const auto& [k, v] : *sds.value_counts) {
                    (*dds.value_counts)[k] += v;
                }
                dds.distinct_count = dds.value_counts->size();
            }
            if (dds.min_value.empty() ||
                (!sds.min_value.empty() && sds.min_value < dds.min_value)) {
                dds.min_value = sds.min_value;
            }
            if (sds.max_value > dds.max_value) {
                dds.max_value = sds.max_value;
            }
        }
        for (std::size_t e = 0;
             e < src.extra_dim_stats.size() && e < dst.extra_dim_stats.size();
             ++e) {
            auto& sds = src.extra_dim_stats[e];
            auto& dds = dst.extra_dim_stats[e];
            if (sds.value_counts) {
                if (!dds.value_counts) dds.value_counts.emplace();
                for (const auto& [k, v] : *sds.value_counts) {
                    (*dds.value_counts)[k] += v;
                }
                dds.distinct_count = dds.value_counts->size();
            }
            if (dds.min_value.empty() ||
                (!sds.min_value.empty() && sds.min_value < dds.min_value)) {
                dds.min_value = sds.min_value;
            }
            if (sds.max_value > dds.max_value) {
                dds.max_value = sds.max_value;
            }
        }

        dst.statistics.merge_from(src.statistics);

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
}

void BloomVisitor::finalize(indexer::IndexDatabaseWriterContext& db,
                            int file_id) {
    auto file_statistics = persist_bloom_sink_writes(
        db, file_id, extra_dim_names_, chunks_, config_);
    persist_bloom_concrete_tail(db, file_id, file_statistics, chunks_.size(),
                                /*refresh_root_summaries=*/true);
}

void BloomVisitor::finalize_sink_only(indexer::IndexBatchSink& sink,
                                      int file_id) {
    flush_per_checkpoint_to_sink(sink, file_id);
    finalize_file_to_sink(sink, file_id);
}

void BloomVisitor::flush_per_checkpoint_to_sink(indexer::IndexBatchSink& sink,
                                                int file_id) {
    if (chunks_.empty()) return;

    if (!file_acc_.initialized) {
        for (std::size_t b = 0; b < BF_COUNT; ++b) {
            file_acc_.fixed_blooms[b] =
                BloomFilter(config_.expected_entries_per_chunk,
                            config_.false_positive_rate);
        }
        file_acc_.extra_blooms.reserve(extra_dim_names_.size());
        for (std::size_t e = 0; e < extra_dim_names_.size(); ++e) {
            file_acc_.extra_blooms.emplace_back(
                config_.expected_entries_per_chunk,
                config_.false_positive_rate);
        }
        file_acc_.initialized = true;
    }

    std::vector<unsigned char> blob;
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        const auto& chunk = chunks_[i];
        const auto checkpoint_idx =
            static_cast<std::uint64_t>(chunks_base_idx_ + i);

        for (std::size_t b = 0; b < BF_COUNT; ++b) {
            const BloomFilter& bf = chunk.fixed_blooms[b];
            bf.serialize_into(blob);
            sink.insert_chunk_bloom_filter(
                file_id, checkpoint_idx, std::string(FIXED_BLOOM_NAMES[b]),
                std::span<const unsigned char>(blob.data(), blob.size()),
                static_cast<std::uint64_t>(bf.num_entries()));
            file_acc_.fixed_blooms[b].merge_from(bf);
        }
        for (std::size_t e = 0;
             e < extra_dim_names_.size() && e < chunk.extra_blooms.size();
             ++e) {
            const BloomFilter& bf = chunk.extra_blooms[e];
            bf.serialize_into(blob);
            sink.insert_chunk_bloom_filter(
                file_id, checkpoint_idx, extra_dim_names_[e],
                std::span<const unsigned char>(blob.data(), blob.size()),
                static_cast<std::uint64_t>(bf.num_entries()));
            file_acc_.extra_blooms[e].merge_from(bf);
        }

        sink.insert_chunk_statistics(file_id, checkpoint_idx, chunk.statistics);
        file_acc_.statistics.merge_from(chunk.statistics);

        for (std::size_t d = 0; d < FD_COUNT; ++d) {
            sink.insert_chunk_dimension_stats(file_id, checkpoint_idx,
                                              chunk.fixed_dim_stats[d],
                                              config_.value_counts_cap);
        }
        for (const auto& ds : chunk.extra_dim_stats) {
            sink.insert_chunk_dimension_stats(file_id, checkpoint_idx, ds,
                                              config_.value_counts_cap);
        }

        for (const auto& [name, _] : chunk.statistics.name_counts) {
            const auto name_id = hash::fnv1a_hash(name);
            sink.insert_name_chunk_posting(name_id, file_id, checkpoint_idx);
        }
    }

    file_acc_.num_chunks_emitted += chunks_.size();
    chunks_base_idx_ += chunks_.size();
    chunks_.clear();
}

void BloomVisitor::finalize_file_to_sink(indexer::IndexBatchSink& sink,
                                         int file_id) {
    if (!file_acc_.initialized && chunks_.empty()) return;
    flush_per_checkpoint_to_sink(sink, file_id);

    std::vector<unsigned char> blob;
    for (std::size_t b = 0; b < BF_COUNT; ++b) {
        const BloomFilter& bf = file_acc_.fixed_blooms[b];
        bf.serialize_into(blob);
        sink.insert_file_bloom_filter(
            file_id, std::string(FIXED_BLOOM_NAMES[b]),
            std::span<const unsigned char>(blob.data(), blob.size()),
            static_cast<std::uint64_t>(bf.num_entries()));
    }
    for (std::size_t e = 0; e < extra_dim_names_.size(); ++e) {
        const BloomFilter& bf = file_acc_.extra_blooms[e];
        bf.serialize_into(blob);
        sink.insert_file_bloom_filter(
            file_id, extra_dim_names_[e],
            std::span<const unsigned char>(blob.data(), blob.size()),
            static_cast<std::uint64_t>(bf.num_entries()));
    }

    for (std::size_t b = 0; b < BF_COUNT; ++b) {
        sink.insert_index_dimension(file_id, std::string(FIXED_BLOOM_NAMES[b]));
    }
    for (const auto& dim : extra_dim_names_) {
        sink.insert_index_dimension(file_id, dim);
    }
    sink.insert_index_dimension(file_id, std::string(DIM_TS));
    sink.insert_index_dimension(file_id, std::string(DIM_DUR));

    sink.insert_file_scalar_stats(file_id, file_acc_.statistics,
                                  file_acc_.num_chunks_emitted);
    sink.insert_file_category_counts(file_id,
                                     file_acc_.statistics.category_counts);
    sink.insert_file_name_counts(file_id, file_acc_.statistics.name_counts);
    sink.insert_file_pid_tid_counts(file_id,
                                    file_acc_.statistics.pid_tid_counts);

    for (const auto& [name, _] : file_acc_.statistics.name_counts) {
        const auto name_id = hash::fnv1a_hash(name);
        sink.insert_name_dictionary_entry(name_id, name);
        sink.insert_name_file_posting(name_id, file_id);
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::visitors
