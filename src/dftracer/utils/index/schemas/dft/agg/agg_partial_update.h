#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_UPDATE_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_UPDATE_H

#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_core.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace dftracer::utils::index::schemas::dft::agg {

/// The sink that folds one event into an AggPartial. `arg_id` maps an arg name
/// to its per-root id.
template <class IdOf>
class PartialSink {
   public:
    PartialSink(AggPartial& partial, std::uint64_t pid,
                const AggregationConfig& cfg, IdOf& arg_id)
        : p_(partial),
          arg_id_(arg_id),
          pid_(pid),
          percentiles_(cfg.compute_percentiles) {}

    void add_count(std::uint64_t n) { p_.count += n; }
    void span(std::uint64_t ts, std::uint64_t len) {
        p_.add_span(pid_, ts, ts + len);
    }
    void duration(std::uint64_t dur) {
        ++p_.count;
        p_.duration.add(dur);
        ++p_.duration_values;
        if (percentiles_) p_.duration_q.add(static_cast<double>(dur));
    }
    void size(std::uint64_t v) {
        p_.size.add(v);
        ++p_.size_values;
        if (percentiles_) p_.size_q.add(static_cast<double>(v));
    }
    void offset(std::uint64_t v) {
        p_.offset.add(v);
        ++p_.offset_values;
        if (percentiles_) p_.offset_q.add(static_cast<double>(v));
    }
    void merge_duration(const dataframe::FieldStat& s) { p_.duration.merge(s); }
    void merge_size(const dataframe::FieldStat& s) { p_.size.merge(s); }
    void merge_offset(const dataframe::FieldStat& s) { p_.offset.merge(s); }
    void custom(std::string_view key, std::uint64_t u) {
        ArgStat& a = p_.arg(arg_id_(key));
        a.stat.add(u);
        ++a.values;
        if (percentiles_) a.quantiles.add(static_cast<double>(u));
    }
    dataframe::FieldStat& custom_stat(std::string_view key) {
        return p_.arg(arg_id_(key)).stat;
    }
    void note_custom(std::string_view) {}

   private:
    AggPartial& p_;
    IdOf& arg_id_;
    std::uint64_t pid_;
    bool percentiles_;
};

/// Whether the per-name partial holds the event: not metadata, and not a
/// system event that is not pre-aggregated (those keep their own per-bucket
/// records).
template <class Acc>
bool partial_holds(const Acc& acc) {
    if (acc.is_metadata()) return false;
    return !acc.is_system() || acc.arg_is_number("count") ||
           acc.arg_is_number("dftu_cnt") || acc.arg_is_number("dft_cnt");
}

/// The kind of the partial an event that partial_holds belongs to.
template <class Acc>
PartialKind partial_kind(const Acc& acc) {
    return acc.is_profile()  ? PartialKind::Profile
           : acc.is_system() ? PartialKind::System
                             : PartialKind::Event;
}

/// The values of the extra group keys of `cfg` on an event, joined by 0x1F in
/// the order of the keys; empty when the event has none of them.
template <class Acc>
void extra_values(const Acc& acc, const AggregationConfig& cfg,
                  std::string& out) {
    out.clear();
    bool any = false;
    bool first = true;
    for (const auto& k : cfg.extra_group_keys) {
        if (!first) out += '\x1f';
        first = false;
        const auto v = acc.arg_string(k);
        if (!v.empty()) any = true;
        out.append(v);
    }
    if (!any) out.clear();
}

/// Folds an event that partial_holds into `partial` by the rules of
/// aggregate_event. `arg_id(std::string_view) -> std::uint32_t` maps an arg
/// name to its id.
template <class Acc, class IdOf>
void fold_into_partial(AggPartial& partial, const Acc& acc,
                       const AggregationConfig& cfg, IdOf&& arg_id) {
    const auto fhash = acc.arg_string("fhash");
    partial.observe(acc.pid(), acc.tid(),
                    fhash.empty() ? 0 : agg_hash_of_hex(fhash));
    PartialSink<IdOf> sink(partial, acc.pid(), cfg, arg_id);
    accumulate_event_metrics(acc, cfg, sink);
}

/// Folds one event into `partial` by the rules of aggregate_event. Returns
/// false, and leaves `partial` alone, for an event the per-name partial does
/// not hold (see partial_holds). The caller picks the partial by the
/// lower-cased category and the name.
template <class Acc, class IdOf>
bool update_partial(AggPartial& partial, const Acc& acc,
                    const AggregationConfig& cfg, IdOf&& arg_id) {
    if (!partial_holds(acc)) return false;
    fold_into_partial(partial, acc, cfg, arg_id);
    return true;
}

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_UPDATE_H
