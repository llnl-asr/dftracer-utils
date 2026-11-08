// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_AGG_EVENT_FOLD_H
#define DFTRACER_UTILS_TRACE_AGG_EVENT_FOLD_H

#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_core.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/views/fold_event.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::trace {

/// The result id of a text a filter refuses, and the marker of an id not looked
/// up yet.
inline constexpr std::uint32_t AGG_REJECT = 0xFFFFFFFFu;
inline constexpr std::uint32_t AGG_UNSET = 0xFFFFFFFEu;

/// The (cat, name, host, extra, kind) of a partial, as result ids. The extra id
/// is NO_ID for none. A per-file partial (see AggEventRules::file_rows) has
/// NO_ID for the four ids and the file of its events in `file` and
/// `file_text`.
struct AggGroup {
    std::uint32_t cat;
    std::uint32_t name;
    std::uint32_t host;
    std::uint32_t extra;
    AggKind kind;
    std::uint64_t file = 0;
    std::uint32_t file_text = 0xFFFFFFFFu;

    bool operator==(const AggGroup&) const = default;
};

struct AggGroupHash {
    using is_avalanching = void;
    std::size_t operator()(const AggGroup& g) const noexcept;
};

/// Sets `key.file` and `key.file_text` for the file hash text `fhash`, as the
/// index fold keys it: a hash of 16 hex digits is its value and any other text
/// is kept in `strings`. An empty text is no file.
void set_file_key(AggKey& key, dstd::StringView fhash,
                  dstd::StringIntern& strings);

/// The state every event of one query is folded with, shared by its files,
/// chunks and workers.
class AggEventRules {
   public:
    /// `read` resolves the ids of the events folded and `strings` holds the key
    /// and value texts of the rows.
    AggEventRules(const AggQuery& q, dstd::StringIntern& read,
                  dstd::StringIntern& strings);

    std::uint64_t bucket(std::uint64_t ts) const {
        return index::schemas::dft::agg::compute_time_bucket(ts, window_cfg);
    }

    const AggQuery& q;
    /// The query is answered from the per-file partials: it groups by file
    /// alone, so the events fold by (file, kind) in place of their category,
    /// name and host.
    bool file_rows = false;
    dstd::StringIntern& strings;
    dstd::StringIntern& read;
    /// The ids of the fixed metric args in `read`.
    index::schemas::dft::agg::MetricArgIds read_metric_ids;
    index::schemas::dft::agg::AggregationConfig window_cfg;
    index::schemas::dft::agg::AggregationConfig read_cfg;
    dstd::HashSet<dstd::String> categories;
    dstd::HashSet<dstd::String> names;
    dstd::HashSet<dstd::String> hosts;
    dstd::Vector<std::uint64_t> pids;
};

/// id -> result id, looked up once per id. Ids past MAX_CACHED_ID are looked up
/// each time.
class AggIdCache {
   public:
    template <class Fn>
    std::uint32_t get(std::uint32_t id, Fn&& compute) {
        if (id >= MAX_CACHED_ID) return compute(id);
        if (id >= ids_.size())
            ids_.resize(std::max<std::size_t>(id + 1, ids_.size() * 2),
                        AGG_UNSET);
        if (ids_[id] == AGG_UNSET) ids_[id] = compute(id);
        return ids_[id];
    }

   private:
    static constexpr std::uint32_t MAX_CACHED_ID = 1u << 20;
    dstd::Vector<std::uint32_t> ids_;
};

/// Folds events into the rows of one file, chunk or worker by the rules of
/// `rules`.
class AggEventFolder {
   public:
    AggEventFolder(AggEventRules& rules, AggQueryResult::Rows& rows,
                   AggQueryStats& stats)
        : r_(rules), rows_(rows), stats_(stats) {}

    /// Adds `ev` to the rows. `wanted` keeps only the groups a re-read asks for
    /// (null keeps every group).
    void add_event(const views::detail::FoldEvent& ev,
                   const dstd::HashSet<AggGroup, AggGroupHash>* wanted);

   private:
    /// The result id of a category text, lower-cased, REJECT when a filter does
    /// not hold it.
    std::uint32_t read_cat(std::uint32_t id);
    std::uint32_t read_name(std::uint32_t id);
    std::uint32_t read_host(std::uint32_t id);
    /// The result id of the values of the extra group keys of the event, joined
    /// by 0x1F; NO_ID when it has none of them.
    template <class Acc>
    std::uint32_t extra_id(const Acc& acc);

    AggEventRules& r_;
    AggQueryResult::Rows& rows_;
    AggQueryStats& stats_;
    AggIdCache build_cat_, build_name_, build_host_;
    dstd::String extra_buf_;
};

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_TRACE_AGG_EVENT_FOLD_H
