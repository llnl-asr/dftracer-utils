#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_serialization.h>
#include <dftracer/utils/index/schemas/dft/agg/association_tracker.h>
#include <dftracer/utils/index/schemas/dft/agg/event_aggregator.h>
#include <dftracer/utils/index/store/key_codec.h>

namespace dftracer::utils::index::schemas::dft::agg {

EventAggregator::EventAggregator()
    : rocksdb_mode_(false), intern_(make_intern_table()) {}

EventAggregator::EventAggregator(
    std::shared_ptr<index::store::RocksDatabase> db)
    : rocksdb_mode_(true), db_(std::move(db)) {
    intern_ = intern_for_index(db_->path());
    tier::load_dictionary(*db_, *intern_);
}

void EventAggregator::merge_chunk(ChunkAggregationOutput&& chunk_output) {
    if (rocksdb_mode_) {
        merge_chunk_rocksdb(std::move(chunk_output));
    } else {
        merge_chunk_memory(std::move(chunk_output));
    }
}

void EventAggregator::merge_chunk_memory(
    ChunkAggregationOutput&& chunk_output) {
    if (!chunk_output.success) return;

    state_.total_events_processed += chunk_output.events_processed;
    state_.total_bytes_processed += chunk_output.bytes_processed;
    unique_files_.insert(chunk_output.file_path);

    auto merge_into = [](AggregationMap& dst, AggregationMap& src) {
        for (auto& [key, metrics] : src) {
            auto it = dst.find(key);
            if (it == dst.end()) {
                dst.emplace(key, std::move(metrics));
            } else {
                it->second.merge_from(metrics);
            }
        }
    };
    merge_into(state_.aggregations, chunk_output.aggregations);
    merge_into(state_.profile_aggregations, chunk_output.profile_aggregations);
    merge_into(state_.system_aggregations, chunk_output.system_aggregations);

    if (chunk_output.local_tracker) {
        state_.trackers.push_back(std::move(chunk_output.local_tracker));
    }

    update_time_bounds(chunk_output.min_time_bucket);
    update_time_bounds(chunk_output.max_time_bucket);
}

void EventAggregator::merge_chunk_rocksdb(
    ChunkAggregationOutput&& chunk_output) {
    if (!chunk_output.success) return;

    total_events_ += chunk_output.events_processed;
    total_bytes_ += chunk_output.bytes_processed;

    update_time_bounds(chunk_output.min_time_bucket);
    update_time_bounds(chunk_output.max_time_bucket);

    unique_files_.insert(std::move(chunk_output.file_path));

    if (chunk_output.local_tracker) {
        trackers_.push_back(std::move(chunk_output.local_tracker));
    }
}

void EventAggregator::add_observed_extra_key(const std::string& key) {
    auto& intern = intern_->intern;
    observed_extra_key_ids_.insert(intern.get_or_insert(key));
}

void EventAggregator::add_observed_custom_metric(const std::string& name) {
    observed_custom_metric_names_.insert(name);
}

EventAggregatorOutput EventAggregator::finalize() {
    if (rocksdb_mode_) {
        EventAggregatorOutput output;
        output.intern = intern_;
        output.total_events_processed = total_events_.load();
        output.total_bytes_processed = total_bytes_.load();
        output.total_files_processed = unique_files_.size();
        output.trackers = std::move(trackers_);

        scan([&output](AggMapType map_type, const AggregationKey& key,
                       AggregationMetrics& metrics) {
            switch (map_type) {
                case AggMapType::PROFILE:
                    output.profile_aggregations.emplace(key,
                                                        std::move(metrics));
                    break;
                case AggMapType::SYSTEM:
                    output.system_aggregations.emplace(key, std::move(metrics));
                    break;
                default:
                    output.aggregations.emplace(key, std::move(metrics));
                    break;
            }
            return true;
        });

        output.success = true;

        persist_time_bounds();

        DFTRACER_UTILS_LOG_INFO(
            "Aggregation complete: %zu unique keys, %zu total events, %zu "
            "files",
            output.aggregations.size(), output.total_events_processed,
            output.total_files_processed);

        return output;
    }

    state_.intern = intern_;
    state_.total_files_processed = unique_files_.size();
    state_.success = true;

    DFTRACER_UTILS_LOG_INFO(
        "Aggregation complete: %zu unique keys, %zu total events, %zu files",
        state_.aggregations.size(), state_.total_events_processed,
        state_.total_files_processed);

    return std::move(state_);
}

std::size_t EventAggregator::scan(ScanCallback callback) const {
    if (!rocksdb_mode_) {
        std::size_t count = 0;
        auto scan_map = [&](const AggregationMap& map, AggMapType map_type) {
            for (auto& [key, metrics] : map) {
                count++;
                auto& mutable_metrics =
                    const_cast<AggregationMetrics&>(metrics);
                if (!callback(map_type, key, mutable_metrics)) return false;
            }
            return true;
        };
        if (!scan_map(state_.aggregations, AggMapType::EVENT)) return count;
        if (!scan_map(state_.profile_aggregations, AggMapType::PROFILE))
            return count;
        scan_map(state_.system_aggregations, AggMapType::SYSTEM);
        return count;
    }

    return scan_shard_range(0, AGG_KEY_NUM_SHARDS, callback);
}

std::size_t EventAggregator::scan_shard_range_raw_fn(
    std::uint16_t shard_begin, std::uint16_t shard_end, RawScanCallbackFn fn,
    void* ctx, std::string_view after) const {
    if (!rocksdb_mode_ || !db_) return 0;
    return tier::for_each_row(
        *db_, shard_begin, shard_end,
        [fn, ctx](std::string_view k, std::string_view v) {
            return fn(ctx, k, v);
        },
        after);
}

std::size_t EventAggregator::scan_shard_range(std::uint16_t shard_begin,
                                              std::uint16_t shard_end,
                                              ScanCallback callback) const {
    if (!rocksdb_mode_ || !db_) return 0;
    return tier::for_each_row(
        *db_, shard_begin, shard_end,
        [&callback](std::string_view k, std::string_view v) {
            auto deserialized = deserialize_agg_key(k);
            auto metrics = deserialize_agg_value(v);
            return callback(deserialized.map_type, deserialized.key, metrics);
        });
}

namespace {

std::string serialize_observed_columns(
    const std::set<std::uint32_t>& extra_key_ids,
    const std::set<std::string>& custom_metric_names,
    const StringIntern& intern) {
    namespace rocks = index::store;
    std::string out;
    auto put_str = [&](std::string_view s) {
        rocks::KeyCodec::append_be32(out, static_cast<std::uint32_t>(s.size()));
        out.append(s.data(), s.size());
    };

    rocks::KeyCodec::append_be32(
        out, static_cast<std::uint32_t>(extra_key_ids.size()));
    for (auto id : extra_key_ids) put_str(intern.resolve(id));

    rocks::KeyCodec::append_be32(
        out, static_cast<std::uint32_t>(custom_metric_names.size()));
    for (const auto& name : custom_metric_names) put_str(name);

    return out;
}

void deserialize_observed_columns(std::string_view data,
                                  std::set<std::uint32_t>& extra_key_ids,
                                  std::set<std::string>& custom_metric_names,
                                  StringIntern& intern) {
    namespace rocks = index::store;
    std::size_t off = 0;
    auto read_u32 = [&]() -> std::uint32_t {
        if (off + 4 > data.size()) return 0;
        auto v = rocks::KeyCodec::decode_be32(data.substr(off, 4));
        off += 4;
        return v;
    };
    auto read_str = [&]() -> std::string_view {
        auto len = read_u32();
        if (off + len > data.size()) return {};
        auto sv = data.substr(off, len);
        off += len;
        return sv;
    };

    auto n_extra = read_u32();
    for (std::uint32_t i = 0; i < n_extra; ++i) {
        auto sv = read_str();
        if (!sv.empty()) extra_key_ids.insert(intern.get_or_insert(sv));
    }

    auto n_metrics = read_u32();
    for (std::uint32_t i = 0; i < n_metrics; ++i) {
        auto sv = read_str();
        if (!sv.empty()) custom_metric_names.emplace(sv);
    }
}

}  // namespace

EventAggregator::ObservedColumns EventAggregator::observed_columns() {
    if (rocksdb_mode_ && db_) {
        if (auto val = tier::read_observed_columns(*db_); val && !val->empty())
            deserialize_observed_columns(*val, observed_extra_key_ids_,
                                         observed_custom_metric_names_,
                                         intern_->intern);
        tier::write_observed_columns(
            *db_, serialize_observed_columns(observed_extra_key_ids_,
                                             observed_custom_metric_names_,
                                             intern_->intern));
    }

    ObservedColumns result;
    result.extra_key_ids.assign(observed_extra_key_ids_.begin(),
                                observed_extra_key_ids_.end());
    result.custom_metric_names.assign(observed_custom_metric_names_.begin(),
                                      observed_custom_metric_names_.end());
    return result;
}

std::unique_ptr<AssociationTracker> EventAggregator::build_global_tracker() {
    auto tracker = std::make_unique<AssociationTracker>();

    for (const auto& t : trackers_) {
        if (t) tracker->merge(*t);
    }
    trackers_.clear();

    if (rocksdb_mode_ && db_)
        if (auto val = tier::read_tracker(*db_); val && !val->empty())
            tracker->merge(AssociationTracker::deserialize(*val));

    tracker->finalize();

    if (rocksdb_mode_ && db_) tier::write_tracker(*db_, tracker->serialize());

    return tracker;
}

void EventAggregator::update_time_bounds(std::uint64_t time_bucket) {
    std::uint64_t old_min = min_time_bucket_.load(std::memory_order_relaxed);
    while (time_bucket < old_min &&
           !min_time_bucket_.compare_exchange_weak(old_min, time_bucket,
                                                   std::memory_order_relaxed)) {
    }

    std::uint64_t old_max = max_time_bucket_.load(std::memory_order_relaxed);
    while (time_bucket > old_max &&
           !max_time_bucket_.compare_exchange_weak(old_max, time_bucket,
                                                   std::memory_order_relaxed)) {
    }
}

void EventAggregator::persist_time_bounds() {
    if (!rocksdb_mode_ || !db_) return;
    auto min_tb = min_time_bucket_.load(std::memory_order_relaxed);
    auto max_tb = max_time_bucket_.load(std::memory_order_relaxed);
    // min_tb == UINT64_MAX is the only "no events seen" sentinel; a real
    // bucket range can legitimately be [0, 0] (relative time, first bucket).
    if (min_tb != UINT64_MAX && min_tb <= max_tb)
        tier::widen_time_bounds(*db_, {min_tb, max_tb});
}

std::uint64_t EventAggregator::min_time_bucket() const {
    return min_time_bucket_.load(std::memory_order_relaxed);
}

std::uint64_t EventAggregator::max_time_bucket() const {
    return max_time_bucket_.load(std::memory_order_relaxed);
}

EventAggregator::TimeBoundsResult EventAggregator::query_time_bounds() const {
    TimeBoundsResult result;

    if (rocksdb_mode_ && db_)
        if (auto bounds = tier::read_time_bounds(*db_)) {
            result.min_time_bucket = bounds->min_bucket;
            result.max_time_bucket = bounds->max_bucket;
            result.valid = true;
            return result;
        }

    std::uint64_t min_val = min_time_bucket_.load(std::memory_order_relaxed);
    std::uint64_t max_val = max_time_bucket_.load(std::memory_order_relaxed);
    result.min_time_bucket = min_val;
    result.max_time_bucket = max_val;
    result.valid =
        (min_val != UINT64_MAX && max_val != 0 && min_val <= max_val);
    return result;
}

}  // namespace dftracer::utils::index::schemas::dft::agg
