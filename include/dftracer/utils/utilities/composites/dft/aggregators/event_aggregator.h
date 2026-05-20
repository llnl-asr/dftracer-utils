#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_EVENT_AGGREGATOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_EVENT_AGGREGATOR_H

#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::rocksdb {
class RocksDatabase;
}

namespace dftracer::utils::utilities::composites::dft::aggregators {

class EventAggregator {
   public:
    EventAggregator();

    EventAggregator(std::shared_ptr<rocksdb::RocksDatabase> db,
                    std::uint32_t config_hash);

    void merge_chunk(ChunkAggregationOutput&& chunk_output);

    EventAggregatorOutput finalize();

    using ScanCallback = std::function<bool(AggMapType, const AggregationKey&,
                                            AggregationMetrics&)>;
    std::size_t scan(ScanCallback callback) const;
    std::size_t scan_shard_range(std::uint16_t shard_begin,
                                 std::uint16_t shard_end,
                                 ScanCallback callback) const;

    /// Type-erased raw scan. Use the templated overload below for zero-
    /// allocation calls.
    using RawScanCallbackFn = bool (*)(void* ctx, std::string_view key_bytes,
                                       std::string_view value_bytes);
    std::size_t scan_shard_range_raw_fn(std::uint16_t shard_begin,
                                        std::uint16_t shard_end,
                                        RawScanCallbackFn fn, void* ctx) const;

    /// Sequential scan of the SYSTEM_METRICS CF (no shard prefix in keys).
    std::size_t scan_system_metrics_raw_fn(RawScanCallbackFn fn,
                                           void* ctx) const;

    template <typename F>
    std::size_t scan_system_metrics_raw(F&& callback) const {
        auto adapter =
            +[](void* ctx, std::string_view k, std::string_view v) -> bool {
            return (*static_cast<std::decay_t<F>*>(ctx))(k, v);
        };
        return scan_system_metrics_raw_fn(adapter,
                                          static_cast<void*>(&callback));
    }

    /// Template wrapper: forwards any callable `(sv, sv) -> bool` into the
    /// raw scan with zero heap allocations. The adapter lambda is a captureless
    /// `+[]` so it decays to a plain function pointer.
    template <typename F>
    std::size_t scan_shard_range_raw(std::uint16_t shard_begin,
                                     std::uint16_t shard_end,
                                     F&& callback) const {
        auto adapter =
            +[](void* ctx, std::string_view k, std::string_view v) -> bool {
            return (*static_cast<std::decay_t<F>*>(ctx))(k, v);
        };
        return scan_shard_range_raw_fn(shard_begin, shard_end, adapter,
                                       static_cast<void*>(&callback));
    }

    /// Move trackers out without materializing the full aggregation map.
    std::vector<std::shared_ptr<AssociationTracker>> take_trackers();

    /// Merge fresh trackers with any persisted tracker from the DB,
    /// persist the result, and return the merged tracker.
    std::unique_ptr<AssociationTracker> build_global_tracker();

    struct ObservedColumns {
        std::vector<std::uint32_t> extra_key_ids;
        std::vector<std::string> custom_metric_names;
    };
    ObservedColumns observed_columns();
    void add_observed_extra_key(const std::string& key);
    void add_observed_custom_metric(const std::string& name);

    std::size_t total_events() const { return total_events_.load(); }
    std::size_t total_bytes() const { return total_bytes_.load(); }
    std::size_t total_files() const { return unique_files_.size(); }

    void update_time_bounds(std::uint64_t time_bucket);
    std::uint64_t min_time_bucket() const;
    std::uint64_t max_time_bucket() const;

    struct TimeBoundsResult {
        std::uint64_t min_time_bucket;
        std::uint64_t max_time_bucket;
        bool valid;
    };
    TimeBoundsResult query_time_bounds() const;

    bool is_rocksdb_mode() const { return rocksdb_mode_; }
    std::shared_ptr<rocksdb::RocksDatabase> db() const { return db_; }
    std::uint32_t config_hash() const { return config_hash_; }

    static std::shared_ptr<rocksdb::RocksDatabase> open_with_merge_operator(
        const std::string& index_path);

    /// Read-only variant for multi-process concurrent scan (e.g. MPI ranks
    /// covering disjoint shard-prefix ranges of a shared unified index).
    /// Multi-process RocksDB writes are forbidden; read-only opens do not
    /// hold the exclusive LOCK, so N ranks can open the same DB at once.
    static std::shared_ptr<rocksdb::RocksDatabase>
    open_read_only_with_merge_operator(const std::string& index_path);

   private:
    void merge_chunk_memory(ChunkAggregationOutput&& chunk_output);
    void merge_chunk_rocksdb(ChunkAggregationOutput&& chunk_output);

    bool rocksdb_mode_ = false;

    // In-memory state
    EventAggregatorOutput state_;
    std::unordered_set<std::string> unique_files_;

    // RocksDB state
    std::shared_ptr<rocksdb::RocksDatabase> db_;
    std::uint32_t config_hash_ = 0;
    std::atomic<std::size_t> total_events_{0};
    std::atomic<std::size_t> total_bytes_{0};
    std::vector<std::shared_ptr<AssociationTracker>> trackers_;

    std::set<std::uint32_t> observed_extra_key_ids_;
    std::set<std::string> observed_custom_metric_names_;

    std::atomic<std::uint64_t> min_time_bucket_{UINT64_MAX};
    std::atomic<std::uint64_t> max_time_bucket_{0};
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_EVENT_AGGREGATOR_H
