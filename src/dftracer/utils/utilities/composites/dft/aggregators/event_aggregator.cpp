#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_merge_operator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_tracker.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics_merge_operator.h>
#include <rocksdb/table.h>

namespace dftracer::utils::utilities::composites::dft::aggregators {

namespace rcf = dftracer::utils::rocksdb::cf;
namespace rocks = dftracer::utils::rocksdb;

static constexpr std::string_view TIME_BOUNDS_DB_KEY = "__time_bounds__";

EventAggregator::EventAggregator() : rocksdb_mode_(false) {}

EventAggregator::EventAggregator(std::shared_ptr<rocksdb::RocksDatabase> db,
                                 std::uint32_t config_hash)
    : rocksdb_mode_(true), db_(std::move(db)), config_hash_(config_hash) {
    load_intern_dictionary(*db_);
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
    auto& intern = aggregation_intern();
    observed_extra_key_ids_.insert(intern.get_or_insert(key));
}

void EventAggregator::add_observed_custom_metric(const std::string& name) {
    observed_custom_metric_names_.insert(name);
}

EventAggregatorOutput EventAggregator::finalize() {
    if (rocksdb_mode_) {
        EventAggregatorOutput output;
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

        auto min_tb = min_time_bucket_.load(std::memory_order_relaxed);
        auto max_tb = max_time_bucket_.load(std::memory_order_relaxed);
        if (min_tb != UINT64_MAX && max_tb != 0 && min_tb <= max_tb) {
            std::string time_bounds_val = rocks::KeyCodec::encode_be64(min_tb);
            time_bounds_val += rocks::KeyCodec::encode_be64(max_tb);
            db_->put(TIME_BOUNDS_DB_KEY, time_bounds_val, rcf::AGGREGATION);
        }

        DFTRACER_UTILS_LOG_INFO(
            "Aggregation complete: %zu unique keys, %zu total events, %zu "
            "files",
            output.aggregations.size(), output.total_events_processed,
            output.total_files_processed);

        return output;
    }

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

std::size_t EventAggregator::scan_shard_range_raw_fn(std::uint16_t shard_begin,
                                                     std::uint16_t shard_end,
                                                     RawScanCallbackFn fn,
                                                     void* ctx) const {
    if (!rocksdb_mode_ || !db_) return 0;

    char begin_key[2];
    begin_key[0] = static_cast<char>(shard_begin >> 8);
    begin_key[1] = static_cast<char>(shard_begin);

    auto it = db_->new_iterator(rcf::AGGREGATION);
    std::size_t count = 0;
    for (it->Seek({begin_key, 2}); it->Valid(); it->Next()) {
        auto key_slice = it->key();
        if (key_slice.size() < 3) continue;
        std::uint16_t shard = (static_cast<std::uint8_t>(key_slice[0]) << 8) |
                              static_cast<std::uint8_t>(key_slice[1]);
        if (shard >= AGG_KEY_NUM_SHARDS) break;
        if (shard >= shard_end) break;

        auto val_slice = it->value();
        count++;
        if (!fn(ctx, std::string_view(key_slice.data(), key_slice.size()),
                std::string_view(val_slice.data(), val_slice.size())))
            break;
    }
    return count;
}

std::size_t EventAggregator::scan_system_metrics_raw_fn(RawScanCallbackFn fn,
                                                        void* ctx) const {
    if (!rocksdb_mode_ || !db_) return 0;

    auto it = db_->new_iterator(rcf::SYSTEM_METRICS);
    std::size_t count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        auto key_slice = it->key();
        auto val_slice = it->value();
        count++;
        if (!fn(ctx, std::string_view(key_slice.data(), key_slice.size()),
                std::string_view(val_slice.data(), val_slice.size())))
            break;
    }
    return count;
}

std::size_t EventAggregator::scan_shard_range(std::uint16_t shard_begin,
                                              std::uint16_t shard_end,
                                              ScanCallback callback) const {
    if (!rocksdb_mode_ || !db_) return 0;

    char begin_key[2];
    begin_key[0] = static_cast<char>(shard_begin >> 8);
    begin_key[1] = static_cast<char>(shard_begin);

    auto it = db_->new_iterator(rcf::AGGREGATION);
    std::size_t count = 0;
    for (it->Seek({begin_key, 2}); it->Valid(); it->Next()) {
        auto key_slice = it->key();
        if (key_slice.size() < 3) continue;
        std::uint16_t shard = (static_cast<std::uint8_t>(key_slice[0]) << 8) |
                              static_cast<std::uint8_t>(key_slice[1]);
        if (shard >= AGG_KEY_NUM_SHARDS) break;
        if (shard >= shard_end) break;

        auto val_slice = it->value();
        auto deserialized = deserialize_agg_key(
            std::string_view(key_slice.data(), key_slice.size()));
        auto metrics = deserialize_agg_value(
            std::string_view(val_slice.data(), val_slice.size()));

        count++;
        if (!callback(deserialized.map_type, deserialized.key, metrics)) break;
    }
    return count;
}

namespace {

std::string serialize_observed_columns(
    const std::set<std::uint32_t>& extra_key_ids,
    const std::set<std::string>& custom_metric_names) {
    namespace rocks = dftracer::utils::rocksdb;
    auto& intern = aggregation_intern();
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
                                  std::set<std::string>& custom_metric_names) {
    namespace rocks = dftracer::utils::rocksdb;
    auto& intern = aggregation_intern();
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

static constexpr std::string_view COLUMNS_DB_KEY = "__observed_columns__";

EventAggregator::ObservedColumns EventAggregator::observed_columns() {
    if (rocksdb_mode_ && db_) {
        std::string val;
        if (db_->get(COLUMNS_DB_KEY, &val, rcf::AGGREGATION).ok() &&
            !val.empty()) {
            deserialize_observed_columns(val, observed_extra_key_ids_,
                                         observed_custom_metric_names_);
        }

        auto serialized = serialize_observed_columns(
            observed_extra_key_ids_, observed_custom_metric_names_);
        db_->put(COLUMNS_DB_KEY, serialized, rcf::AGGREGATION);
    }

    ObservedColumns result;
    result.extra_key_ids.assign(observed_extra_key_ids_.begin(),
                                observed_extra_key_ids_.end());
    result.custom_metric_names.assign(observed_custom_metric_names_.begin(),
                                      observed_custom_metric_names_.end());
    return result;
}

std::vector<std::shared_ptr<AssociationTracker>>
EventAggregator::take_trackers() {
    return std::move(trackers_);
}

static constexpr std::string_view TRACKER_DB_KEY = "__tracker__";

std::unique_ptr<AssociationTracker> EventAggregator::build_global_tracker() {
    auto tracker = std::make_unique<AssociationTracker>();

    for (const auto& t : trackers_) {
        if (t) tracker->merge(*t);
    }
    trackers_.clear();

    if (rocksdb_mode_ && db_) {
        std::string val;
        if (db_->get(TRACKER_DB_KEY, &val, rcf::AGGREGATION).ok() &&
            !val.empty()) {
            tracker->merge(AssociationTracker::deserialize(val));
        }
    }

    tracker->finalize();

    if (rocksdb_mode_ && db_) {
        db_->put(TRACKER_DB_KEY, tracker->serialize(), rcf::AGGREGATION);
    }

    return tracker;
}

std::shared_ptr<rocksdb::RocksDatabase>
EventAggregator::open_with_merge_operator(const std::string& index_path) {
    auto agg_merge_op = std::make_shared<AggregationMergeOperator>();
    auto sys_merge_op = std::make_shared<SystemMetricsMergeOperator>();
    auto cf_override = [agg_merge_op, sys_merge_op](
                           const std::string& cf_name,
                           ::rocksdb::ColumnFamilyOptions& opts) {
        if (cf_name == rcf::AGGREGATION) {
            opts.merge_operator = agg_merge_op;

            ::rocksdb::BlockBasedTableOptions bbt;
            bbt.block_size = 32 * 1024;
            bbt.format_version = 5;
            bbt.index_block_restart_interval = 16;
            bbt.whole_key_filtering = false;
            opts.table_factory.reset(::rocksdb::NewBlockBasedTableFactory(bbt));

            opts.level0_file_num_compaction_trigger = 2;
            opts.max_bytes_for_level_multiplier = 20;

#ifdef DFTRACER_UTILS_ENABLE_ZSTD
            opts.compression = ::rocksdb::kZSTD;
            opts.compression_opts.level = 9;
            opts.compression_opts.max_dict_bytes = 262144;
            opts.compression_opts.zstd_max_train_bytes = 1048576;
            opts.compression_opts.enabled = true;
            opts.bottommost_compression = ::rocksdb::kZSTD;
            opts.bottommost_compression_opts.level = 9;
            opts.bottommost_compression_opts.max_dict_bytes = 262144;
            opts.bottommost_compression_opts.zstd_max_train_bytes = 1048576;
            opts.bottommost_compression_opts.enabled = true;
#elif defined(DFTRACER_UTILS_ENABLE_LZ4)
            opts.compression = ::rocksdb::kLZ4Compression;
            opts.bottommost_compression = ::rocksdb::kLZ4Compression;
#else
            opts.compression = ::rocksdb::kZlibCompression;
            opts.bottommost_compression = ::rocksdb::kZlibCompression;
#endif
        } else if (cf_name == rcf::SYSTEM_METRICS) {
            opts.merge_operator = sys_merge_op;
#ifdef DFTRACER_UTILS_ENABLE_ZSTD
            opts.compression = ::rocksdb::kZSTD;
            opts.bottommost_compression = ::rocksdb::kZSTD;
#elif defined(DFTRACER_UTILS_ENABLE_LZ4)
            opts.compression = ::rocksdb::kLZ4Compression;
            opts.bottommost_compression = ::rocksdb::kLZ4Compression;
#else
            opts.compression = ::rocksdb::kZlibCompression;
            opts.bottommost_compression = ::rocksdb::kZlibCompression;
#endif
        }
    };
    auto& mgr = rocksdb::RocksDBManager::instance();
    mgr.reset(index_path);
    return mgr.get_or_open(
        index_path, rocksdb::RocksDatabase::OpenMode::ReadWrite, cf_override);
}

std::shared_ptr<rocksdb::RocksDatabase>
EventAggregator::open_read_only_with_merge_operator(
    const std::string& index_path) {
    auto agg_merge_op = std::make_shared<AggregationMergeOperator>();
    auto sys_merge_op = std::make_shared<SystemMetricsMergeOperator>();
    auto cf_override = [agg_merge_op, sys_merge_op](
                           const std::string& cf_name,
                           ::rocksdb::ColumnFamilyOptions& opts) {
        if (cf_name == rcf::AGGREGATION) {
            opts.merge_operator = agg_merge_op;
        } else if (cf_name == rcf::SYSTEM_METRICS) {
            opts.merge_operator = sys_merge_op;
        }
    };
    auto& mgr = rocksdb::RocksDBManager::instance();
    mgr.reset(index_path);
    return mgr.get_or_open(
        index_path, rocksdb::RocksDatabase::OpenMode::ReadOnly, cf_override);
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
    if (min_tb != UINT64_MAX && max_tb != 0 && min_tb <= max_tb) {
        std::string time_bounds_val = rocks::KeyCodec::encode_be64(min_tb);
        time_bounds_val += rocks::KeyCodec::encode_be64(max_tb);
        db_->put(TIME_BOUNDS_DB_KEY, time_bounds_val, rcf::AGGREGATION);
    }
}

std::uint64_t EventAggregator::min_time_bucket() const {
    return min_time_bucket_.load(std::memory_order_relaxed);
}

std::uint64_t EventAggregator::max_time_bucket() const {
    return max_time_bucket_.load(std::memory_order_relaxed);
}

EventAggregator::TimeBoundsResult EventAggregator::query_time_bounds() const {
    TimeBoundsResult result;

    if (rocksdb_mode_ && db_) {
        std::string val;
        if (db_->get(TIME_BOUNDS_DB_KEY, &val, rcf::AGGREGATION).ok() &&
            val.size() >= 16) {
            result.min_time_bucket = rocks::KeyCodec::decode_be64(
                std::string_view(val).substr(0, 8));
            result.max_time_bucket = rocks::KeyCodec::decode_be64(
                std::string_view(val).substr(8, 8));
            result.valid = true;
            return result;
        }
    }

    std::uint64_t min_val = min_time_bucket_.load(std::memory_order_relaxed);
    std::uint64_t max_val = max_time_bucket_.load(std::memory_order_relaxed);
    result.min_time_bucket = min_val;
    result.max_time_bucket = max_val;
    result.valid =
        (min_val != UINT64_MAX && max_val != 0 && min_val <= max_val);
    return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
