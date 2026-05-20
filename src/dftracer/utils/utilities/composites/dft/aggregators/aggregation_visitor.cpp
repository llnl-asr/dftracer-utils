#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_logic.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_visitor.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_tracker.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics_serialization.h>
#include <dftracer/utils/utilities/composites/dft/args_map.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>

namespace dftracer::utils::utilities::composites::dft::aggregators {

namespace rcf = dftracer::utils::rocksdb::cf;

namespace {

inline bool is_reserved_arg(std::string_view k) {
    if (k.empty()) return false;
    switch (k[0]) {
        case 'h':
            return k == "hhash";
        case 'f':
            return k == "fhash";
        case 'd':
            return k == "dur" || k == "dur_sum" || k == "dur_min" ||
                   k == "dur_max" || k == "dft_cnt";
        case 'r':
            return k == "ret" || k == "ret_sum" || k == "ret_min" ||
                   k == "ret_max";
        case 'o':
            return k == "offset" || k == "offset_sum" || k == "offset_min" ||
                   k == "offset_max";
    }
    return false;
}

inline bool is_preagg_suffix(std::string_view k) {
    if (k.size() <= 4) return false;
    std::string_view tail = k.substr(k.size() - 4);
    return tail == "_sum" || tail == "_min" || tail == "_max";
}

}  // namespace

namespace {

/// Derive a unique per-file batch_id from a staging prefix + the file
/// path. Uses FNV1a so concurrent visitors processing different files
/// land in disjoint subdirectories under the staging root.
std::string make_per_file_batch_id(std::string_view prefix,
                                   std::string_view file_path) {
    std::uint64_t fnv_basis = 1469598103934665603ULL;
    std::uint64_t fnv_prime = 1099511628211ULL;
    std::uint64_t h = fnv_basis;
    for (unsigned char c : file_path) {
        h ^= c;
        h *= fnv_prime;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(h));
    std::string out;
    out.reserve(prefix.size() + 1 + 16);
    out.append(prefix);
    out.push_back('_');
    out.append(hex, 16);
    return out;
}

}  // namespace

AggregationVisitor::AggregationVisitor(
    std::shared_ptr<rocksdb::RocksDatabase> db, std::uint32_t config_hash,
    AggregationConfig config, std::string file_path)
    : db_(std::move(db)),
      config_hash_(config_hash),
      config_(std::move(config)),
      file_path_(std::move(file_path)) {
    if (config_.track_process_parents || !config_.boundary_events.empty()) {
        tracker_ = std::make_shared<AssociationTracker>();
    }
    local_buffer_.reserve(65536);
    key_buf_.reserve(128);
    val_buf_.reserve(256);
}

AggregationVisitor::AggregationVisitor(std::string staging_dir,
                                       std::string batch_id_prefix,
                                       std::uint32_t config_hash,
                                       AggregationConfig config,
                                       std::string file_path)
    : sst_staging_dir_(std::move(staging_dir)),
      sst_batch_prefix_(make_per_file_batch_id(batch_id_prefix, file_path)),
      config_hash_(config_hash),
      config_(std::move(config)),
      file_path_(std::move(file_path)) {
    if (config_.track_process_parents || !config_.boundary_events.empty()) {
        tracker_ = std::make_shared<AssociationTracker>();
    }
    local_buffer_.reserve(65536);
    key_buf_.reserve(128);
    val_buf_.reserve(256);
    // First SST writer; rotated after each flush in seal_local_buffer.
    sst_sink_ = std::make_unique<indexer::IndexDatabaseSstWriterContext>(
        sst_staging_dir_,
        sst_batch_prefix_ + "_" + std::to_string(sst_flush_counter_++));
}

void AggregationVisitor::begin(std::size_t /*num_checkpoints*/) {}

void AggregationVisitor::on_checkpoint(std::size_t /*checkpoint_idx*/) {}

void AggregationVisitor::on_event(const EventRecord& record) {
    const auto& ev = record.ev;
    if (ev.is_metadata()) {
        return;
    }

    if (tracker_) {
        tracker_->extract_from_event(ev.name, ev.pid, ev.ts, ev.dur, ev.args,
                                     config_);
    }

    bool is_preaggregated_system = false;
    if (ev.is_system()) {
        auto cnt = ev.args["count"];
        auto dft_cnt = ev.args["dft_cnt"];
        bool is_preaggregated = cnt.is_number() || dft_cnt.is_number();
        if (!is_preaggregated) {
            handle_system_event(record);
            return;
        }
        is_preaggregated_system = true;
    }

    AggMapType map_type = AggMapType::EVENT;
    if (is_preaggregated_system) {
        map_type = AggMapType::SYSTEM;
    } else if (ev.is_profile()) {
        map_type = AggMapType::PROFILE;
    }

    auto hhash = ev.args["hhash"].get<std::string_view>();
    auto fhash = ev.args["fhash"].get<std::string_view>();
    // Counter (ph="C") events report stats for the period ending at ev.ts, so
    // a boundary-aligned timestamp belongs to the bucket it summarizes (the
    // one before it). Plain events keep their own timestamp.
    auto bucket_ts =
        (map_type == AggMapType::PROFILE && ev.ts > 0) ? ev.ts - 1 : ev.ts;
    auto time_bucket = compute_time_bucket(bucket_ts, ev.dur, config_);

    if (time_bucket < min_time_bucket_) min_time_bucket_ = time_bucket;
    if (time_bucket > max_time_bucket_) max_time_bucket_ = time_bucket;

    std::vector<std::pair<std::string_view, std::string_view>> extra_keys_vec;
    std::vector<std::pair<std::string_view, std::string_view>>* extra_ptr =
        nullptr;
    if (!config_.extra_group_keys.empty()) {
        for (const auto& extra_key : config_.extra_group_keys) {
            auto value = ev.args[extra_key].get<std::string_view>();
            if (!value.empty()) {
                extra_keys_vec.emplace_back(extra_key, value);
                observed_extra_keys_.emplace(extra_key);
            }
        }
        if (!extra_keys_vec.empty()) extra_ptr = &extra_keys_vec;
    }

    serialize_agg_key_into(key_buf_, config_hash_, map_type, ev.cat, ev.name,
                           ev.pid, ev.tid, hhash, fhash, time_bucket,
                           extra_ptr);

    AggregationMetrics* entry_ptr;
    if (last_entry_ != nullptr && last_key_ == key_buf_) {
        entry_ptr = last_entry_;
    } else {
        auto [it, inserted] =
            local_buffer_.try_emplace(key_buf_, config_.sketch_accuracy);
        entry_ptr = &it->second;
        last_entry_ = entry_ptr;
        last_key_ = it->first;
    }
    auto& entry = *entry_ptr;
    const bool compute_percentiles = config_.compute_percentiles;

    std::uint64_t ev_count = 1;
    if (ev.is_counter()) {
        auto a_count = ev.args["dft_cnt"];
        if (!a_count.exists()) a_count = ev.args["count"];
        ev_count = a_count.exists() ? a_count.get<std::uint64_t>() : 1;
        entry.count += ev_count;

        auto a_dur = ev.args["dur_sum"];
        if (!a_dur.exists()) a_dur = ev.args["dur"];
        if (a_dur.exists()) {
            MetricStats tmp(config_.sketch_accuracy);
            tmp.count = ev_count;
            tmp.total = a_dur.get<std::uint64_t>();
            auto a_dur_min = ev.args["dur_min"];
            if (!a_dur_min.exists()) a_dur_min = a_dur;
            tmp.min = a_dur_min.get<std::uint64_t>();
            auto a_dur_max = ev.args["dur_max"];
            if (!a_dur_max.exists()) a_dur_max = a_dur;
            tmp.max = a_dur_max.get<std::uint64_t>();
            if (tmp.count > 0) {
                tmp.mean = static_cast<double>(tmp.total) /
                           static_cast<double>(tmp.count);
            }
            entry.duration.merge_from(tmp);
        }

        auto a_size = ev.args["ret_sum"];
        if (!a_size.exists()) a_size = ev.args["ret"];
        if (a_size.exists()) {
            MetricStats tmp(config_.sketch_accuracy);
            tmp.count = ev_count;
            tmp.total = a_size.get<std::uint64_t>();
            auto a_min = ev.args["ret_min"];
            if (!a_min.exists()) a_min = a_size;
            tmp.min = a_min.get<std::uint64_t>();
            auto a_max = ev.args["ret_max"];
            if (!a_max.exists()) a_max = a_size;
            tmp.max = a_max.get<std::uint64_t>();
            if (tmp.count > 0) {
                tmp.mean = static_cast<double>(tmp.total) /
                           static_cast<double>(tmp.count);
            }
            entry.size.merge_from(tmp);
        }

        // offset has no meaningful "sum"; a counter event may carry only
        // offset_min/offset_max, so trigger on any of the offset args.
        auto a_off_sum = ev.args["offset_sum"];
        auto a_off_plain = ev.args["offset"];
        auto a_off_min = ev.args["offset_min"];
        auto a_off_max = ev.args["offset_max"];
        if (a_off_sum.exists() || a_off_plain.exists() || a_off_min.exists() ||
            a_off_max.exists()) {
            MetricStats tmp(config_.sketch_accuracy);
            tmp.count = ev_count;
            tmp.total = a_off_sum.exists() ? a_off_sum.get<std::uint64_t>()
                        : a_off_plain.exists()
                            ? a_off_plain.get<std::uint64_t>()
                            : 0;
            tmp.min = a_off_min.exists()     ? a_off_min.get<std::uint64_t>()
                      : a_off_plain.exists() ? a_off_plain.get<std::uint64_t>()
                                             : tmp.total;
            tmp.max = a_off_max.exists()     ? a_off_max.get<std::uint64_t>()
                      : a_off_plain.exists() ? a_off_plain.get<std::uint64_t>()
                                             : tmp.total;
            if (tmp.count > 0) {
                tmp.mean = static_cast<double>(tmp.total) /
                           static_cast<double>(tmp.count);
            }
            entry.offset.merge_from(tmp);
        }

        entry.update_timestamp(ev.ts, config_.time_interval_us);
    } else {
        entry.update_duration(ev.dur, compute_percentiles);
        entry.update_timestamp(ev.ts, ev.dur);

        auto ret = ev.args["ret"];
        if (ret.exists() && internal::is_data_transfer_op(ev.cat, ev.name)) {
            entry.update_size(ret.get<std::uint64_t>(), compute_percentiles);
        }
        auto off = ev.args["offset"];
        if (off.exists()) {
            entry.update_offset(off.get<std::uint64_t>(), compute_percentiles);
        }
    }

    if (config_.track_default_args) {
        const bool is_counter_ev = ev.is_counter();
        ev.args.for_each_member([&](std::string_view k, ArgsValueProxy v) {
            if (!v.is_number()) return;
            if (is_reserved_arg(k)) return;
            if (is_counter_ev && is_preagg_suffix(k)) return;
            for (const auto& gk : config_.extra_group_keys) {
                if (gk == k) return;
            }
            for (const auto& cf : config_.custom_metric_fields) {
                if (cf == k) return;
            }
            entry.update_custom_metric(k, v.get<std::uint64_t>(),
                                       compute_percentiles);
        });
    }

    for (const auto& field : config_.custom_metric_fields) {
        if (ev.is_counter()) {
            std::string sum_key = std::string(field) + "_sum";
            auto a_sum = ev.args[sum_key];
            if (!a_sum.exists()) a_sum = ev.args[field];
            std::string min_key = std::string(field) + "_min";
            auto a_min = ev.args[min_key];
            if (!a_min.exists()) a_min = ev.args[field];
            std::string max_key = std::string(field) + "_max";
            auto a_max = ev.args[max_key];
            if (!a_max.exists()) a_max = ev.args[field];
            if (a_sum.exists() && a_sum.is_number()) {
                if (!entry.custom_metrics) {
                    entry.custom_metrics = std::make_unique<CustomMetricsMap>();
                }
                auto& cm = *entry.custom_metrics;
                auto cm_it = cm.find(field);
                if (cm_it == cm.end()) {
                    cm_it = cm.emplace(std::string(field),
                                       MetricStats(config_.sketch_accuracy))
                                .first;
                }
                auto& stats = cm_it->second;
                stats.count += ev_count;
                stats.total += a_sum.get<std::uint64_t>();
                if (a_min.exists() && a_min.is_number()) {
                    stats.min = std::min(stats.min, a_min.get<std::uint64_t>());
                }
                if (a_max.exists() && a_max.is_number()) {
                    stats.max = std::max(stats.max, a_max.get<std::uint64_t>());
                }
                if (stats.count > 0) {
                    stats.mean = static_cast<double>(stats.total) /
                                 static_cast<double>(stats.count);
                }
                observed_custom_metrics_.insert(field);
            }
        } else {
            auto field_val = ev.args[field];
            if (field_val.exists() && field_val.is_number()) {
                entry.update_custom_metric(
                    field, field_val.get<std::uint64_t>(), compute_percentiles);
            }
        }
    }

    events_processed_++;

    if (local_buffer_.size() >= FLUSH_THRESHOLD) {
        seal_local_buffer();
    }
}

void AggregationVisitor::handle_system_event(const EventRecord& record) {
    const auto& ev = record.ev;

    auto hhash = ev.args["hhash"].get<std::string_view>();
    auto time_bucket = compute_time_bucket(ev.ts, ev.dur, config_);

    if (time_bucket < min_time_bucket_) min_time_bucket_ = time_bucket;
    if (time_bucket > max_time_bucket_) max_time_bucket_ = time_bucket;

    serialize_system_key_into(system_key_buf_, hhash, ev.name, time_bucket);

    auto [it, inserted] =
        system_buffer_.try_emplace(system_key_buf_, config_.sketch_accuracy);
    auto& entry = it->second;

    entry.count++;
    entry.update_timestamp(ev.ts);

    const bool compute_percentiles = config_.compute_percentiles;

    ev.args.for_each_member([&](std::string_view k, ArgsValueProxy v) {
        if (!v.is_number()) return;
        if (k == "hhash" || k == "fhash") return;

        double val = v.get<double>();
        entry.update_metric(k, val, compute_percentiles);
        observed_system_metrics_.insert(std::string(k));
    });

    events_processed_++;

    if (system_buffer_.size() >= FLUSH_THRESHOLD) {
        seal_local_buffer();
    }
}

void AggregationVisitor::seal_local_buffer() {
    if (local_buffer_.empty() && system_buffer_.empty()) return;

    for (const auto& [key, metrics] : local_buffer_) {
        if (metrics.custom_metrics) {
            for (const auto& [name, _] : *metrics.custom_metrics) {
                observed_custom_metrics_.insert(name);
            }
        }
    }

    if (sst_sink_) {
        // Distributed mode: flush the current in-memory maps into the
        // active per-flush SstWriterContext, then rotate to a fresh one
        // so the next flush (or on_file_complete) writes to its own SST
        // with a fresh, strictly-ascending key space.
        for (auto& [k, m] : local_buffer_) {
            serialize_agg_value_into(val_buf_, m);
            sst_sink_->insert_aggregation_merge(k, val_buf_);
        }
        local_buffer_.clear();
        last_entry_ = nullptr;
        last_key_ = {};

        for (auto& [k, m] : system_buffer_) {
            serialize_system_value_into(system_val_buf_, m);
            sst_sink_->insert_system_metrics_merge(k, system_val_buf_);
        }
        system_buffer_.clear();

        flush_intern_dictionary(*sst_sink_);

        // Commit this flush's SSTs and open a new SstWriterContext for
        // the next flush. Only rotate if something was actually written;
        // an empty commit produces no paths and no-ops.
        auto a = sst_sink_->commit();
        if (!a.empty()) sst_artifacts_.push_back(std::move(a));
        sst_sink_ = std::make_unique<indexer::IndexDatabaseSstWriterContext>(
            sst_staging_dir_,
            sst_batch_prefix_ + "_" + std::to_string(sst_flush_counter_++));
        return;
    }

    // Legacy mode: flush to a RocksDatabase batch; commit at
    // on_file_complete.
    if (!db_) return;
    auto batch = db_->begin_batch();
    for (auto& [k, m] : local_buffer_) {
        serialize_agg_value_into(val_buf_, m);
        db_->merge(batch, rcf::AGGREGATION, k, val_buf_);
    }
    local_buffer_.clear();
    last_entry_ = nullptr;
    last_key_ = {};

    for (auto& [k, m] : system_buffer_) {
        serialize_system_value_into(system_val_buf_, m);
        db_->merge(batch, rcf::SYSTEM_METRICS, k, system_val_buf_);
    }
    system_buffer_.clear();

    flush_intern_dictionary(*db_, batch);
    pending_batches_.push_back(std::move(batch));
}

coro::CoroTask<void> AggregationVisitor::on_file_complete() {
    seal_local_buffer();

    if (sst_sink_) {
        // Commit any final residue (the rotated-to-fresh SstWriterContext
        // that seal_local_buffer left behind). An empty commit returns
        // empty paths which we skip.
        auto a = sst_sink_->commit();
        if (!a.empty()) sst_artifacts_.push_back(std::move(a));
        sst_sink_.reset();
        co_return;
    }

    if (pending_batches_.empty()) co_return;
    for (auto& batch : pending_batches_) {
        db_->commit_batch(batch);
    }
    pending_batches_.clear();
}

void AggregationVisitor::flush_to_batch(rocksdb::RocksDatabase::Batch& batch) {
    // Legacy-only helper, used by aggregator_utility for draining a
    // batch-write phase. SST mode never calls this.
    if (!db_) return;
    for (auto& [k, m] : local_buffer_) {
        serialize_agg_value_into(val_buf_, m);
        db_->merge(batch, rcf::AGGREGATION, k, val_buf_);
    }
    local_buffer_.clear();

    for (auto& [k, m] : system_buffer_) {
        serialize_system_value_into(system_val_buf_, m);
        db_->merge(batch, rcf::SYSTEM_METRICS, k, system_val_buf_);
    }
    system_buffer_.clear();

    flush_intern_dictionary(*db_, batch);
}

ChunkAggregationOutput AggregationVisitor::take_output() {
    if (tracker_) {
        tracker_->finalize();
    }

    ChunkAggregationOutput output;
    output.file_path = std::move(file_path_);
    output.events_processed = events_processed_;
    output.success = true;
    output.local_tracker = std::move(tracker_);
    output.min_time_bucket = min_time_bucket_;
    output.max_time_bucket = max_time_bucket_;

    return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
