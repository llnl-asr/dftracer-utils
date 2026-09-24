#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_EVENT_AGGREGATOR_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_EVENT_AGGREGATOR_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_output.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dftracer::utils::index::store {
class RocksDatabase;
}

namespace dftracer::utils::index::schemas::dft::agg {

class EventAggregator {
   public:
    EventAggregator();

    /// Aggregates into the tier of `db`, opened with agg::tier::open.
    explicit EventAggregator(std::shared_ptr<index::store::RocksDatabase> db);

    /// The table its keys resolve against, shared with anything else holding
    /// this index open.
    const AggInternPtr& intern_table() const { return intern_; }
    StringIntern& intern() const { return intern_->intern; }

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

    /// Widen the stored time bounds with the in-memory min/max time bucket, so
    /// a later read-only reopen can recover the trace origin. finalize() does
    /// this too; the SST build path needs it called explicitly after
    /// merge_chunk().
    void persist_time_bounds();

    std::shared_ptr<index::store::RocksDatabase> db() const { return db_; }

   private:
    void merge_chunk_memory(ChunkAggregationOutput&& chunk_output);
    void merge_chunk_rocksdb(ChunkAggregationOutput&& chunk_output);

    bool rocksdb_mode_ = false;

    /// In-memory state
    EventAggregatorOutput state_;
    StringViewSet unique_files_;

    /// RocksDB state
    AggInternPtr intern_;
    std::shared_ptr<index::store::RocksDatabase> db_;
    std::atomic<std::size_t> total_events_{0};
    std::atomic<std::size_t> total_bytes_{0};
    std::vector<std::shared_ptr<AssociationTracker>> trackers_;

    std::set<std::uint32_t> observed_extra_key_ids_;
    std::set<std::string> observed_custom_metric_names_;

    std::atomic<std::uint64_t> min_time_bucket_{UINT64_MAX};
    std::atomic<std::uint64_t> max_time_bucket_{0};
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_EVENT_AGGREGATOR_H
