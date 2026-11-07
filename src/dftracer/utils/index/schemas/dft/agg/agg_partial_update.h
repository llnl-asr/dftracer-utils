// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_UPDATE_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_UPDATE_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_core.h>

#include <cstdint>

namespace dftracer::utils::index::schemas::dft::agg {

class PartialSink {
   public:
    PartialSink(AggPartial& partial, std::uint64_t pid,
                const AggregationConfig& cfg, TimeBin* bin)
        : p_(partial),
          bin_(bin),
          pid_(pid),
          percentiles_(cfg.compute_percentiles) {}

    void add_count(std::uint64_t n) {
        p_.count += n;
        if (bin_ != nullptr) bin_->count += n;
    }
    void span(std::uint64_t ts, std::uint64_t len) {
        p_.add_span(pid_, ts, ts + len);
    }
    void duration(std::uint64_t dur) {
        ++p_.count;
        p_.duration.add(dur);
        ++p_.duration_values;
        if (percentiles_) p_.duration_q.add(static_cast<double>(dur));
        if (bin_ != nullptr) {
            ++bin_->count;
            ++bin_->duration_n;
            bin_->duration_sum += dur;
        }
    }
    void size(std::uint64_t v) {
        p_.size.add(v);
        ++p_.size_values;
        if (percentiles_) p_.size_q.add(static_cast<double>(v));
        if (bin_ != nullptr) {
            ++bin_->size_n;
            bin_->size_sum += v;
        }
    }
    void offset(std::uint64_t v) {
        p_.offset.add(v);
        ++p_.offset_values;
        if (percentiles_) p_.offset_q.add(static_cast<double>(v));
    }
    // A counter's sums come from unsigned args, so they are whole numbers.
    void merge_duration(const dataframe::FieldStat& s) {
        p_.duration.merge(s);
        if (bin_ != nullptr) {
            bin_->duration_n += s.n;
            bin_->duration_sum += static_cast<std::uint64_t>(s.sum);
        }
    }
    void merge_size(const dataframe::FieldStat& s) {
        p_.size.merge(s);
        if (bin_ != nullptr) {
            bin_->size_n += s.n;
            bin_->size_sum += static_cast<std::uint64_t>(s.sum);
        }
    }
    void merge_offset(const dataframe::FieldStat& s) { p_.offset.merge(s); }

   private:
    AggPartial& p_;
    TimeBin* bin_;
    std::uint64_t pid_;
    bool percentiles_;
};

/// Whether the per-name partial holds the event. A record whose phase the View
/// drops (an unknown letter, such as an instant) is left out, so the index
/// answers as a scan does.
template <class Acc>
bool partial_holds(const Acc& acc) {
    return acc.has_phase() && !acc.is_metadata();
}

template <class Acc>
PartialKind partial_kind(const Acc& acc) {
    return acc.is_profile()      ? PartialKind::Profile
           : acc.is_system()     ? PartialKind::System
           : acc.is_aggregated() ? PartialKind::Aggregated
                                 : PartialKind::Event;
}

/// The extra group key values of `cfg`, joined by 0x1F in key order. Empty when
/// the event has none of them.
template <class Acc>
void extra_values(const Acc& acc, const AggregationConfig& cfg,
                  dstd::String& out) {
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

/// Folds an event that partial_holds into `partial` and, when not null, `bin`.
template <class Acc>
void fold_into_partial(AggPartial& partial, const Acc& acc,
                       const AggregationConfig& cfg, TimeBin* bin = nullptr) {
    const auto fhash = acc.arg_string("fhash");
    partial.observe(acc.pid(), acc.tid(),
                    fhash.empty() ? 0 : agg_hash_of_hex(fhash));
    PartialSink sink(partial, acc.pid(), cfg, bin);
    accumulate_event_metrics(acc, cfg, sink);
}

/// Returns false, leaving `partial` alone, for an event partial_holds rejects.
/// The caller picks the partial by the lower-cased category and the name.
template <class Acc>
bool update_partial(AggPartial& partial, const Acc& acc,
                    const AggregationConfig& cfg) {
    if (!partial_holds(acc)) return false;
    fold_into_partial(partial, acc, cfg);
    return true;
}

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_UPDATE_H
