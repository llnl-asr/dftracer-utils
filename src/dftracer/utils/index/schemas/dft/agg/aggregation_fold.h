#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_FOLD_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_FOLD_H

#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/ptr_hash.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial_update.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_core.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/chunk_columns.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace dftracer::utils::index::store {
class IndexWrite;
}

namespace dftracer::utils::index::schemas::dft::agg {

/// The aggregation core's Accessor over an owned FoldEvent. hhash and fhash
/// come from the POD's dedicated fields, not from `args`. The metric arg values
/// clamp like ArgsValueProxy::get<uint64>.
class PodAccessor {
   public:
    PodAccessor(const trace::views::detail::FoldEvent& ev,
                const dftracer::utils::dstd::StringIntern& intern,
                const MetricArgIds& metric_ids)
        : ev_(ev), intern_(intern), metric_ids_(metric_ids) {}

    bool has_phase() const { return ev_.phase != trace::RecordPhase::Unknown; }
    bool is_metadata() const {
        return ev_.phase == trace::RecordPhase::Metadata;
    }
    bool is_counter() const { return ev_.phase == trace::RecordPhase::Counter; }
    bool is_aggregated() const {
        return ev_.phase == trace::RecordPhase::Aggregated;
    }
    bool is_profile() const { return is_counter() && cat() != "sys"; }
    bool is_system() const { return is_counter() && cat() == "sys"; }
    std::string_view name() const { return resolve(ev_.name_id); }
    std::string_view cat() const { return resolve(ev_.cat_id); }
    std::uint64_t pid() const { return ev_.pid; }
    std::uint64_t tid() const { return ev_.tid; }
    std::uint64_t ts() const { return ev_.ts; }
    std::uint64_t dur() const { return ev_.dur; }
    bool has_dur() const { return ev_.has_dur; }

    std::string_view arg_string(std::string_view k) const {
        if (k == "hhash") return resolve(ev_.hhash_id);
        if (k == "fhash") return resolve(ev_.fhash_id);
        const auto* v = find(k);
        if (v)
            if (const auto* id = std::get_if<std::uint32_t>(v))
                return resolve(*id);
        return {};
    }

    void metric_args(MetricArgs& m) const {
        for (const auto& [kid, val] : ev_.args) {
            for (std::size_t i = 0; i < METRIC_ARG_COUNT; ++i) {
                if (metric_ids_.ids[i] != kid) continue;
                m.add(static_cast<MetricArg>(i), as_uint(val));
                break;
            }
        }
    }

   private:
    static constexpr std::uint32_t NO_ID =
        dftracer::utils::dstd::StringIntern::NO_ID;

    using ArgValue = trace::views::detail::FoldEvent::ArgValue;

    static std::uint64_t as_uint(const ArgValue& v) {
        if (const auto* d = std::get_if<double>(&v)) return clamp_uint(*d);
        if (const auto* i = std::get_if<std::int64_t>(&v))
            return *i < 0 ? 0 : static_cast<std::uint64_t>(*i);
        if (const auto* u = std::get_if<std::uint64_t>(&v)) return *u;
        return 0;
    }
    const ArgValue* find(std::string_view key) const {
        if (ev_.args.empty()) return nullptr;
        const std::uint32_t id = intern_.find(key);
        if (id == NO_ID) return nullptr;
        for (const auto& [kid, v] : ev_.args)
            if (kid == id) return &v;
        return nullptr;
    }
    std::string_view resolve(std::uint32_t id) const {
        return id == NO_ID ? std::string_view{} : intern_.resolve(id);
    }
    // Reproduce ArgsValueProxy::get<uint64_t> applied to a double.
    static std::uint64_t clamp_uint(double d) {
        if (d >= 0 &&
            d <= static_cast<double>(std::numeric_limits<std::uint64_t>::max()))
            return static_cast<std::uint64_t>(d);
        return 0;
    }

    const trace::views::detail::FoldEvent& ev_;
    const dftracer::utils::dstd::StringIntern& intern_;
    const MetricArgIds& metric_ids_;
};

/// Builds the chunk partials, column blocks and dictionary entries of a file as
/// a fold on the fused scan. `build_intern` resolves the ids of the events.
/// `agg_intern` is the per-index table whose ids the partial keys embed. The
/// time-bound outputs are returned out-of-band.
class AggregationFold : public trace::views::detail::Fold {
   public:
    AggregationFold(dftracer::utils::dstd::StringIntern& build_intern,
                    index::schemas::dft::agg::AggInternPtr agg_intern,
                    index::schemas::dft::agg::AggregationConfig config,
                    int file_id = -1)
        : build_intern_(&build_intern),
          metric_ids_(build_intern),
          agg_intern_(std::move(agg_intern)),
          config_(std::move(config)),
          file_id_(file_id) {}

    bool accepts(const trace::views::detail::ScanShape&) const override {
        return true;
    }
    bool needs_args() const override { return true; }

    std::unique_ptr<trace::views::detail::Fold> slice() const override {
        return std::make_unique<AggregationFold>(*build_intern_, agg_intern_,
                                                 config_, file_id_);
    }

    void step(const trace::views::detail::FoldBatch& batch) override {
        for (const auto& ev : batch.events) {
            PodAccessor acc(ev, *build_intern_, metric_ids_);
            if (!partial_holds(acc)) continue;
            index::schemas::dft::agg::aggregate_event(acc, state_, config_);
            if (file_id_ >= 0) {
                if (batch.unit.checkpoint_idx != chunk_) {
                    close_chunk();
                    chunk_ = batch.unit.checkpoint_idx;
                }
                add_to_partial(ev, acc);
                add_to_columns(ev);
            }
        }
    }

    void seal_unit(const trace::views::detail::ScanUnit&) override {}
    void drop_unit(const trace::views::detail::ScanUnit&) override {}
    void merge(trace::views::detail::Fold& other) override;

    coro::CoroTask<bool> finalize(
        const trace::views::detail::CoverageSet&) override {
        co_return false;
    }

    /// Emits the partials, column blocks and referenced dictionary entries into
    /// `w`, and the file's manifest entry when the fold has a file.
    void write(index::store::IndexWrite& w);

    std::uint64_t min_time_bucket() const { return state_.min_time_bucket; }
    std::uint64_t max_time_bucket() const { return state_.max_time_bucket; }
    std::size_t events_processed() const { return state_.events_processed; }

   private:
    static constexpr std::uint32_t NO_ID =
        dftracer::utils::dstd::StringIntern::NO_ID;

    struct SlotKey {
        std::uint32_t cat;
        std::uint32_t name;
        std::uint32_t host;
        std::uint32_t extra;
        PartialKind kind;

        bool operator==(const SlotKey&) const = default;
    };

    struct SlotKeyHash {
        using is_avalanching = void;
        std::size_t operator()(const SlotKey& k) const noexcept {
            using dftracer::utils::hash::splitmix64;
            return static_cast<std::size_t>(splitmix64(
                splitmix64(splitmix64((std::uint64_t{k.cat} << 32) | k.name) ^
                           ((std::uint64_t{k.host} << 32) | k.extra)) ^
                static_cast<std::uint64_t>(k.kind)));
        }
    };

    struct ThreadBin {
        std::uint64_t pid;
        std::uint64_t tid;
        std::uint32_t at;
    };

    /// A slot is reused by every chunk and holds nothing until `used`.
    struct Slot {
        AggPartial partial;
        dftracer::utils::dstd::Vector<ThreadBin> threads;
        SlotKey key;
        std::uint32_t last_thread = 0;
        bool used = false;

        Slot(double accuracy, SlotKey k) : partial(accuracy), key(k) {}
    };

    /// The stored value, layout header included, in a buffer of exactly its
    /// size.
    struct Closed {
        dftracer::utils::dstd::Vector<std::uint8_t> value;
        std::uint64_t chunk;
        SlotKey key;
    };

    struct ClosedColumns {
        dftracer::utils::dstd::Vector<std::uint8_t> value;
        std::uint64_t chunk;
    };

    void add_to_partial(const trace::views::detail::FoldEvent& ev,
                        const PodAccessor& acc);
    void add_to_columns(const trace::views::detail::FoldEvent& ev);
    TimeBin& bin_of(Slot& slot, std::uint64_t pid, std::uint64_t tid,
                    std::uint64_t bin);
    /// Cached by `build_id`.
    std::uint32_t root_id(dstd::Vector<std::uint32_t>& cache,
                          std::uint32_t build_id, bool lower);
    void close_chunk();
    void close_columns();

    dftracer::utils::dstd::StringIntern* build_intern_;
    index::schemas::dft::agg::MetricArgIds metric_ids_;
    index::schemas::dft::agg::AggInternPtr agg_intern_;
    index::schemas::dft::agg::AggregationConfig config_;
    int file_id_;
    index::schemas::dft::agg::AggState state_;

    std::uint64_t chunk_ = UINT64_MAX;
    dftracer::utils::dstd::Vector<Slot> slots_;
    dftracer::utils::dstd::HashMap<SlotKey, std::uint32_t, SlotKeyHash>
        slot_of_;
    std::uint32_t last_slot_ = NO_ID;
    std::uint32_t last_cat_ = NO_ID;
    std::uint32_t last_name_ = NO_ID;
    std::uint32_t last_host_ = NO_ID;
    PartialKind last_kind_ = PartialKind::Event;
    std::uint32_t empty_id_ = NO_ID;
    std::string extra_buf_;
    dftracer::utils::dstd::Vector<std::uint32_t> cat_ids_;
    dftracer::utils::dstd::Vector<std::uint32_t> name_ids_;
    dftracer::utils::dstd::Vector<std::uint32_t> host_ids_;
    dftracer::utils::dstd::Vector<std::uint32_t> fhash_ids_;
    dftracer::utils::dstd::Vector<std::uint32_t> path_ids_;
    /// The column block keeps the category case. The partials use `cat_ids_`.
    dftracer::utils::dstd::Vector<std::uint32_t> cat_text_ids_;
    dftracer::utils::dstd::Vector<std::uint8_t> scratch_;
    dftracer::utils::dstd::Vector<Closed> closed_;
    index::schemas::dft::agg::ChunkColumnBuilder columns_;
    dftracer::utils::dstd::Vector<ClosedColumns> closed_columns_;
    /// Duplicates included. Cleared once written.
    dftracer::utils::dstd::Vector<std::uint32_t> referenced_;
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_FOLD_H
