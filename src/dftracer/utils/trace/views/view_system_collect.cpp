// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/chunk_columns.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_system_collect.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace dftracer::utils::trace::views::detail {

namespace {

namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace store = dftracer::utils::index::store;
namespace dataframe = dftracer::utils::dataframe;

constexpr std::string_view SYS_CATEGORY = "sys";

// The args that mark a record as an already aggregated counter, which the
// counters of a typed collect do not take a second time.
bool is_preagg_count(std::string_view path) {
    return path == "count" || path == "dftu_cnt" || path == "dft_cnt";
}

// The two hash args a counter group does not keep as metrics.
bool is_hash_arg(std::string_view path) {
    return path == "hhash" || path == "fhash";
}

// The plan's time_bucket and group_by do not apply here, as the counters keep
// their own (host, name, time_bucket) grain.
std::optional<index::Snapshot> covering_index(const ViewPlan& plan) {
    if (plan.files.empty()) return std::nullopt;
    const std::string& index_path = plan.files.front().index_path;
    if (index_path.empty()) return std::nullopt;
    try {
        auto snapshot = common::unwrap(index::plan::open_snapshot(index_path));
        const auto config = common::unwrap(agg::tier::read_config(snapshot));
        if (!config) return std::nullopt;
        std::set<std::string> current;
        for (const auto& file : common::unwrap(snapshot.files())) {
            if (snapshot.extension(file, store::IndexExtension::Agg,
                                   config->params_hash) ==
                index::Freshness::Current)
                current.emplace(file.path().data(), file.path().size());
        }
        if (current.size() != plan.files.size()) return std::nullopt;
        for (const auto& f : plan.files) {
            if (f.index_path != index_path) return std::nullopt;
            if (!current.count(store::internal::get_logical_path(f.file_path)))
                return std::nullopt;
        }
        return snapshot;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

struct GroupKey {
    std::uint16_t shard = 0;
    dstd::String host;
    dstd::String name;
    std::uint64_t time_bucket = 0;
};

bool operator<(const GroupKey& a, const GroupKey& b) {
    if (a.shard != b.shard) return a.shard < b.shard;
    if (a.host != b.host) return a.host < b.host;
    if (a.name != b.name) return a.name < b.name;
    return a.time_bucket < b.time_bucket;
}

struct GroupKeyLess {
    bool operator()(const GroupKey& a, const GroupKey& b) const {
        return a < b;
    }
};

using Groups = dstd::Map<GroupKey, agg::SystemAggregationMetrics, GroupKeyLess>;

bool holds_row(const agg::ChunkColumn& column, std::uint64_t row) {
    return std::binary_search(column.rows.begin(), column.rows.end(), row);
}

double value_of(const agg::ChunkColumn& column, std::size_t at) {
    switch (column.kind) {
        case agg::ColumnKind::Int64:
            return static_cast<double>(column.int64_at(at));
        case agg::ColumnKind::Uint64:
            return static_cast<double>(column.uint64_at(at));
        case agg::ColumnKind::Double:
            return column.double_at(at);
    }
    return 0.0;
}

// end() when the column does not hold the row.
std::size_t row_index(const agg::ChunkColumn& column, std::uint64_t row) {
    const auto it =
        std::lower_bound(column.rows.begin(), column.rows.end(), row);
    if (it == column.rows.end() || *it != row) return column.bits.size();
    return static_cast<std::size_t>(it - column.rows.begin());
}

// Fills `out` with the group of a sys counter row, false for any other row.
bool counter_group(const agg::ChunkColumns& block, std::uint64_t r,
                   const agg::AggregationConfig& cfg, GroupKey& out,
                   agg::SystemAggregationMetrics& out_metrics) {
    if (block.phase[r] !=
        static_cast<std::uint8_t>(trace::RecordPhase::Counter))
        return false;
    const dstd::String& cat = block.texts[block.cat[r]];
    if (std::string_view(cat) != SYS_CATEGORY) return false;
    for (const agg::ChunkColumn& column : block.columns) {
        if (holds_row(column, r) && is_preagg_count(block.texts[column.path]))
            return false;
    }
    const dstd::String& host = block.texts[block.host[r]];
    const dstd::String& name = block.texts[block.name[r]];
    const std::uint64_t bucket = agg::compute_time_bucket(block.ts[r], cfg);
    out.host = host;
    out.name = name;
    out.time_bucket = bucket;
    out.shard = agg::system_shard(host, name);
    out_metrics.count += 1;
    out_metrics.update_timestamp(block.ts[r]);
    for (const agg::ChunkColumn& column : block.columns) {
        const std::size_t at = row_index(column, r);
        if (at == column.bits.size()) continue;
        const std::string_view path = block.texts[column.path];
        if (is_hash_arg(path)) continue;
        out_metrics.update_metric(path, value_of(column, at));
    }
    return true;
}

// One row per group whose shard lies in [begin, end), a column per metric name
// (the sorted union) and NaN where a group lacks one.
dataframe::DataFrame build_counters_frame(const Groups& groups, int shard_begin,
                                          int shard_end) {
    const std::int64_t begin = shard_begin;
    const std::int64_t end =
        shard_end <= 0 ? agg::AGG_KEY_NUM_SHARDS : shard_end;
    const auto kept = [&](const GroupKey& key) {
        return static_cast<std::int64_t>(key.shard) >= begin &&
               static_cast<std::int64_t>(key.shard) < end;
    };
    std::set<dstd::String> name_set;
    for (const auto& [key, metrics] : groups) {
        if (!kept(key)) continue;
        if (metrics.metrics)
            for (const auto& [n, _] : *metrics.metrics) name_set.insert(n);
    }
    dstd::Vector<dstd::String> metric_names(name_set.begin(), name_set.end());

    // Per-metric means, so the value columns stay Float64.
    dstd::Vector<dstd::String> hosts, names;
    dstd::Vector<double> time_bucket, ts, te, count;
    dstd::Vector<dstd::Vector<double>> metric_vals(metric_names.size());
    for (const auto& [key, metrics] : groups) {
        if (!kept(key)) continue;
        hosts.push_back(key.host);
        names.push_back(key.name);
        time_bucket.push_back(static_cast<double>(key.time_bucket));
        ts.push_back(static_cast<double>(metrics.ts));
        te.push_back(static_cast<double>(metrics.te));
        count.push_back(static_cast<double>(metrics.count));
        for (std::size_t i = 0; i < metric_names.size(); ++i) {
            double v = std::numeric_limits<double>::quiet_NaN();
            if (metrics.metrics) {
                const auto it = metrics.metrics->find(metric_names[i]);
                if (it != metrics.metrics->end()) v = it->second.mean();
            }
            metric_vals[i].push_back(v);
        }
    }

    dataframe::DataFrame b;
    auto add_str = [&](const char* nm, const dstd::Vector<dstd::String>& v) {
        dstd::Vector<std::string_view> views;
        views.reserve(v.size());
        for (const auto& s : v) views.emplace_back(s);
        b.names.emplace_back(nm);
        b.columns.push_back(dataframe::Series::strings(views));
    };
    auto add_f64 = [&](std::string_view nm, const dstd::Vector<double>& v) {
        b.names.emplace_back(nm);
        b.columns.push_back(dataframe::Series::flat_f64(
            v.data(), static_cast<std::int64_t>(v.size())));
    };
    add_str("host_hash", hosts);
    add_str("name", names);
    add_f64("time_bucket", time_bucket);
    add_f64("ts", ts);
    add_f64("te", te);
    add_f64("count", count);
    for (std::size_t i = 0; i < metric_names.size(); ++i)
        add_f64(metric_names[i], metric_vals[i]);
    return b;
}

}  // namespace

bool system_collect(const ViewPlan& plan, dataframe::DataFrame& out_table,
                    int shard_begin, int shard_end, const ProgressFn*) {
    auto index = covering_index(plan);
    if (!index) return false;
    if (shard_end <= 0) shard_end = agg::AGG_KEY_NUM_SHARDS;
    const auto stored = common::unwrap(agg::tier::read_config(*index));
    if (!stored) return false;
    agg::AggregationConfig cfg;
    cfg.time_interval_us = stored->time_interval_us;

    Groups groups;
    for (const auto& f : plan.files) {
        const auto file =
            index->file(store::internal::get_logical_path(f.file_path));
        if (!file) return false;
        common::unwrap(index::for_each_agg_chunk_columns(
            *index, *file, [&](std::uint64_t, const agg::ChunkColumns& block) {
                for (std::uint64_t r = 0; r < block.rows; ++r) {
                    GroupKey key;
                    agg::SystemAggregationMetrics metrics;
                    if (!counter_group(block, r, cfg, key, metrics)) continue;
                    auto it = groups.find(key);
                    if (it == groups.end())
                        it = groups.emplace(std::move(key), std::move(metrics))
                                 .first;
                    else
                        it->second.merge_from(metrics);
                }
                return true;
            }));
    }

    out_table = build_counters_frame(groups, shard_begin, shard_end);
    return true;
}

}  // namespace dftracer::utils::trace::views::detail
