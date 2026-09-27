#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_FOLD_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_FOLD_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_core.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/association_tracker.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>

#include <cstdint>
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
                const dftracer::utils::StringIntern& intern)
        : ev_(ev), intern_(intern) {}

    bool is_metadata() const {
        return ev_.phase == trace::RecordPhase::METADATA;
    }
    bool is_counter() const { return ev_.phase == trace::RecordPhase::COUNTER; }
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
    static constexpr std::uint32_t NO_ID = dftracer::utils::StringIntern::NO_ID;

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
    const dftracer::utils::StringIntern& intern_;
};

/// Builds the aggregation index tier (AGGREGATION + SYSTEM_METRICS records) as
/// a fold on the fused scan, so it rides the same single parse as BloomFold /
/// DictFold. `build_intern` is the fused-scan intern that produced the events
/// (resolves the POD's ids); `agg_intern` is the per-index aggregation intern
/// table whose ids the serialized keys embed. write emits merge
/// operands + the intern dictionary; the tracker / observed-key / time-bound
/// outputs are returned out-of-band (they are not column-family records).
class AggregationFold : public trace::views::detail::Fold {
   public:
    AggregationFold(dftracer::utils::StringIntern& build_intern,
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
            index::schemas::dft::agg::aggregate_event(acc, state_, config_,
                                                      agg_intern_->intern);
        }
    }

    void seal_unit(const trace::views::detail::ScanUnit&) override {}
    void drop_unit(const trace::views::detail::ScanUnit&) override {}
    void merge(trace::views::detail::Fold& other) override;

    coro::CoroTask<bool> finalize(
        const trace::views::detail::CoverageSet&) override {
        co_return false;
    }

    /// Emit the accumulated aggregation + system-metrics merge operands and the
    /// intern dictionary into `w`, with the dftracer.agg manifest entry of the
    /// fold's file when it has one. Aggregation keys are file-independent.
    void write(index::store::IndexWrite& w);

    const StringViewSet& observed_extra_keys() const {
        return state_.observed_extra_keys;
    }
    const StringViewSet& observed_custom_metrics() const {
        return state_.observed_custom_metrics;
    }
    const StringViewSet& observed_system_metrics() const {
        return state_.observed_system_metrics;
    }
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
    dftracer::utils::StringIntern* build_intern_;
    index::schemas::dft::agg::AggInternPtr agg_intern_;
    index::schemas::dft::agg::AggregationConfig config_;
    int file_id_;
    index::schemas::dft::agg::AggState state_;
    std::shared_ptr<index::schemas::dft::agg::AssociationTracker> tracker_;
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_FOLD_H
