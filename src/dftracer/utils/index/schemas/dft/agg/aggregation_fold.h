#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_FOLD_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_FOLD_H

#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/ptr_hash.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_core.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/association_tracker.h>
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

/// Reads the aggregation core's Accessor concept off an owned FoldEvent,
/// reproducing DFTracerEvent/ArgsValueProxy semantics so a fold-built
/// aggregation tier is byte-identical to the DOM-driven visitor. hhash/fhash
/// are read from the POD's dedicated fields (they are not in `args`); every
/// numeric arg is stored as a double, so arg_uint clamps exactly like
/// ArgsValueProxy::get<uint64>.
class PodAccessor {
   public:
    PodAccessor(const trace::views::detail::FoldEvent& ev,
                const dftracer::utils::dstd::StringIntern& intern)
        : ev_(ev), intern_(intern) {}

    bool is_metadata() const {
        return ev_.phase == trace::RecordPhase::Metadata;
    }
    bool is_counter() const { return ev_.phase == trace::RecordPhase::Counter; }
    bool is_profile() const { return is_counter() && cat() != "sys"; }
    bool is_system() const { return is_counter() && cat() == "sys"; }
    std::string_view name() const { return resolve(ev_.name_id); }
    std::string_view cat() const { return resolve(ev_.cat_id); }
    std::uint64_t pid() const { return ev_.pid; }
    std::uint64_t tid() const { return ev_.tid; }
    std::uint64_t ts() const { return ev_.ts; }
    std::uint64_t dur() const { return ev_.dur; }

    bool arg_exists(std::string_view k) const {
        if (k == "hhash") return ev_.hhash_id != NO_ID;
        if (k == "fhash") return ev_.fhash_id != NO_ID;
        return find(k) != nullptr;
    }
    bool arg_is_number(std::string_view k) const {
        if (k == "hhash" || k == "fhash") return false;
        const auto* v = find(k);
        return v && !std::holds_alternative<std::uint32_t>(*v);
    }
    std::uint64_t arg_uint(std::string_view k) const {
        if (k == "hhash" || k == "fhash") return 0;
        const auto* v = find(k);
        if (v) {
            if (const auto* d = std::get_if<double>(v)) return clamp_uint(*d);
            if (const auto* i = std::get_if<std::int64_t>(v))
                return *i < 0 ? 0 : static_cast<std::uint64_t>(*i);
            if (const auto* u = std::get_if<std::uint64_t>(v)) return *u;
        }
        return 0;
    }
    double arg_double(std::string_view k) const {
        if (k == "hhash" || k == "fhash") return 0.0;
        const auto* v = find(k);
        if (v) {
            if (const auto* d = std::get_if<double>(v)) return *d;
            if (const auto* i = std::get_if<std::int64_t>(v))
                return static_cast<double>(*i);
            if (const auto* u = std::get_if<std::uint64_t>(v))
                return static_cast<double>(*u);
        }
        return 0.0;
    }
    std::string_view arg_string(std::string_view k) const {
        if (k == "hhash") return resolve(ev_.hhash_id);
        if (k == "fhash") return resolve(ev_.fhash_id);
        const auto* v = find(k);
        if (v)
            if (const auto* id = std::get_if<std::uint32_t>(v))
                return resolve(*id);
        return {};
    }

    template <class Fn>
    void for_each_numeric_arg(Fn&& fn) const {
        for (const auto& [kid, val] : ev_.args) {
            if (const auto* d = std::get_if<double>(&val))
                fn(resolve(kid), clamp_uint(*d), *d);
            else if (const auto* i = std::get_if<std::int64_t>(&val))
                fn(resolve(kid), *i < 0 ? 0 : static_cast<std::uint64_t>(*i),
                   static_cast<double>(*i));
            else if (const auto* u = std::get_if<std::uint64_t>(&val))
                fn(resolve(kid), *u, static_cast<double>(*u));
        }
    }

   private:
    static constexpr std::uint32_t NO_ID =
        dftracer::utils::dstd::StringIntern::NO_ID;

    const trace::views::detail::FoldEvent::ArgValue* find(
        std::string_view key) const {
        for (const auto& [kid, v] : ev_.args)
            if (resolve(kid) == key) return &v;
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
};

/// Builds the system metric rows and the chunk partials of a file (when the
/// fold has a file id) as a fold on the fused scan, so it rides the same single
/// parse as BloomFold / DictFold. `build_intern` is the fused-scan intern that
/// produced the events (resolves the POD's ids); `agg_intern` is the per-index
/// aggregation intern table whose ids the partial keys embed. write emits the
/// system merge operands, the partials and the intern dictionary; the tracker
/// and time-bound outputs are returned out-of-band.
class AggregationFold : public trace::views::detail::Fold {
   public:
    AggregationFold(dftracer::utils::dstd::StringIntern& build_intern,
                    index::schemas::dft::agg::AggInternPtr agg_intern,
                    index::schemas::dft::agg::AggregationConfig config,
                    int file_id = -1)
        : build_intern_(&build_intern),
          agg_intern_(std::move(agg_intern)),
          config_(std::move(config)),
          file_id_(file_id) {
        if (config_.track_process_parents || !config_.boundary_events.empty())
            tracker_ = std::make_shared<
                index::schemas::dft::agg::AssociationTracker>();
    }

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
            PodAccessor acc(ev, *build_intern_);
            // The tracker sees every non-metadata event, fed before the
            // aggregation classifies it (metadata dropped, then tracker, then
            // aggregate).
            if (acc.is_metadata()) continue;
            if (tracker_) tracker_->extract(acc, config_);
            index::schemas::dft::agg::aggregate_event(acc, state_, config_);
            if (file_id_ >= 0) {
                if (batch.unit.checkpoint_idx != chunk_) {
                    close_chunk();
                    chunk_ = batch.unit.checkpoint_idx;
                }
                add_to_partial(ev, acc);
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

    /// Emit the accumulated system-metrics merge operands, the partials and the
    /// intern dictionary into `w`, with the dftracer.agg manifest entry of the
    /// fold's file when it has one.
    void write(index::store::IndexWrite& w);

    std::uint64_t min_time_bucket() const { return state_.min_time_bucket; }
    std::uint64_t max_time_bucket() const { return state_.max_time_bucket; }
    std::size_t events_processed() const { return state_.events_processed; }

    /// The per-file process/boundary tracker (null when the config tracks
    /// neither). Returned raw (not finalized), matching the visitor's
    /// take_output().local_tracker; the merger finalizes the global tracker.
    std::shared_ptr<index::schemas::dft::agg::AssociationTracker>
    take_tracker() {
        return std::move(tracker_);
    }

   private:
    static constexpr std::uint32_t NO_ID =
        dftracer::utils::dstd::StringIntern::NO_ID;

    /// The root-dictionary ids of the category, name and host hash text of a
    /// partial.
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

    /// The partial being built for one (category, name, host) of the current
    /// chunk; a slot is reused by every chunk and holds nothing until `used`.
    struct Slot {
        AggPartial partial;
        SlotKey key;
        bool used = false;

        Slot(double accuracy, SlotKey k) : partial(accuracy), key(k) {}
    };

    /// A serialized partial of a closed chunk, in `arena_`.
    struct Closed {
        std::uint64_t chunk;
        std::size_t offset;
        std::size_t size;
        SlotKey key;
    };

    struct ArgId {
        std::uint32_t length;
        std::uint32_t id;
    };

    void add_to_partial(const trace::views::detail::FoldEvent& ev,
                        const PodAccessor& acc);
    /// The root-dictionary id of the string with `build_id` (lower-cased for
    /// `lower`), cached by that id.
    std::uint32_t root_id(dstd::Vector<std::uint32_t>& cache,
                          std::uint32_t build_id, bool lower);
    std::uint32_t arg_id(std::string_view key);
    /// Serializes the partials of the current chunk into `arena_` and clears
    /// them, keeping their capacity.
    void close_chunk();

    dftracer::utils::dstd::StringIntern* build_intern_;
    index::schemas::dft::agg::AggInternPtr agg_intern_;
    index::schemas::dft::agg::AggregationConfig config_;
    int file_id_;
    index::schemas::dft::agg::AggState state_;
    std::shared_ptr<index::schemas::dft::agg::AssociationTracker> tracker_;

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
    dftracer::utils::dstd::HashMap<const void*, ArgId, dftracer::utils::PtrHash,
                                   std::equal_to<const void*>>
        arg_ids_;
    dftracer::utils::dstd::Vector<std::uint8_t> arena_;
    dftracer::utils::dstd::Vector<Closed> closed_;
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_FOLD_H
