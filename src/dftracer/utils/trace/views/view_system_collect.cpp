#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/chunk_columns.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/trace/views/view_system_collect.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <variant>

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

// The plan's event-side time_bucket/group_by do not apply here - the counters
// keep their own (host, name, time_bucket) grain. Every row is keyed by the
// same shard prefix, so it range-splits identically.
std::optional<store::IndexDatabase> covering_index(const ViewPlan& plan) {
    if (plan.files.empty()) return std::nullopt;
    const std::string& index_path = plan.files.front().index_path;
    if (index_path.empty()) return std::nullopt;
    try {
        store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
        const auto config = agg::tier::read_config(db);
        if (!config) return std::nullopt;
        std::set<std::string> current;
        for (const auto& [path, id] : db.query_all_file_info_ids()) {
            const auto state =
                db.extension_state(id, store::IndexExtension::Agg);
            if (state && state->ready &&
                state->version ==
                    store::layout::ext_version(store::layout::Ext::Agg) &&
                state->params_hash == config->params_hash)
                current.insert(path);
        }
        if (current.size() != plan.files.size()) return std::nullopt;
        for (const auto& f : plan.files) {
            if (f.index_path != index_path) return std::nullopt;
            if (!current.count(store::internal::get_logical_path(f.file_path)))
                return std::nullopt;
        }
        return db;
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

// Where `row` sits in a column that holds it, so its value reads without a
// second search; end() when it does not hold the row.
std::size_t row_index(const agg::ChunkColumn& column, std::uint64_t row) {
    const auto it =
        std::lower_bound(column.rows.begin(), column.rows.end(), row);
    if (it == column.rows.end() || *it != row) return column.bits.size();
    return static_cast<std::size_t>(it - column.rows.begin());
}

// Whether the block's row `r` is a sys counter the counters keep, and fills
// `out` with its group; false for any other row.
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

// The counters frame a set of groups gives: one row per group whose shard lies
// in [begin, end), a column per metric name (the sorted union, so the schema is
// stable) and NaN where a group lacks one.
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

    // The counters keep their own grain and a per-metric mean (a double), so
    // the value columns stay Float64 - no integer domain to preserve here.
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

// The aggregation config the counters of `plan` group with, or nothing when the
// plan does not span the files of one current aggregation tier.
std::optional<agg::AggregationConfig> counter_config(const ViewPlan& plan) {
    auto index = covering_index(plan);
    if (!index) return std::nullopt;
    const auto stored = agg::tier::read_config(*index);
    if (!stored) return std::nullopt;
    agg::AggregationConfig cfg;
    cfg.time_interval_us = stored->time_interval_us;
    return cfg;
}

}  // namespace

bool system_collect(const ViewPlan& plan, dataframe::DataFrame& out_table,
                    int shard_begin, int shard_end, const ProgressFn*) {
    auto index = covering_index(plan);
    if (!index) return false;
    if (shard_end <= 0) shard_end = agg::AGG_KEY_NUM_SHARDS;
    const auto stored = agg::tier::read_config(*index);
    if (!stored) return false;
    agg::AggregationConfig cfg;
    cfg.time_interval_us = stored->time_interval_us;

    Groups groups;
    for (const auto& f : plan.files) {
        const int fid = index->get_file_info_id(
            store::internal::get_logical_path(f.file_path));
        if (fid < 0) return false;
        bool answered = true;
        agg::tier::for_each_chunk_columns(
            *index, fid, [&](std::uint64_t, const agg::ChunkColumns& block) {
                bool holds_counters = false;
                for (std::uint64_t r = 0; r < block.rows && answered; ++r) {
                    GroupKey key;
                    agg::SystemAggregationMetrics metrics;
                    if (!counter_group(block, r, cfg, key, metrics)) continue;
                    holds_counters = true;
                    auto it = groups.find(key);
                    if (it == groups.end())
                        it = groups.emplace(std::move(key), std::move(metrics))
                                 .first;
                    else
                        it->second.merge_from(metrics);
                }
                // A dropped arg path may be a metric of one of those rows.
                if (holds_counters && !block.dropped.empty()) answered = false;
                return answered;
            });
        if (!answered) return false;
    }

    out_table = build_counters_frame(groups, shard_begin, shard_end);
    return true;
}

namespace {

// A string id of the scan's intern, empty when it names nothing.
std::string_view text_of(const dftracer::utils::dstd::StringIntern& intern,
                         std::uint32_t id) {
    if (id == dftracer::utils::dstd::StringIntern::NO_ID) return {};
    return intern.resolve(id);
}

// A value the counters keep as a metric: a real or an integer. A string id
// (uint32) names a dimension of the record, not a number.
bool is_metric(const FoldEvent::ArgValue& value) {
    return !std::holds_alternative<std::uint32_t>(value);
}

double number_of(const FoldEvent::ArgValue& value) {
    if (const auto* d = std::get_if<double>(&value)) return *d;
    if (const auto* i = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*i);
    return static_cast<double>(std::get<std::uint64_t>(value));
}

// The sys counters of a raw scan: the same groups the column blocks carry, read
// from the trace when a block left one of their metrics out.
class SystemCountersFold : public Fold {
   public:
    SystemCountersFold(const dftracer::utils::dstd::StringIntern& intern,
                       const agg::AggregationConfig& cfg)
        : intern_(&intern), cfg_(cfg) {}

    bool accepts(const ScanShape&) const override { return true; }
    bool needs_args() const override { return true; }
    bool wants_metadata() const override { return false; }

    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<SystemCountersFold>(*intern_, cfg_);
    }

    void step(const FoldBatch& batch) override {
        for (const FoldEvent& ev : batch.events) {
            if (ev.phase != RecordPhase::Counter) continue;
            if (text_of(*intern_, ev.cat_id) != SYS_CATEGORY) continue;
            bool preaggregated = false;
            for (const auto& [key, value] : ev.args) {
                if (!is_metric(value)) continue;
                if (is_preagg_count(text_of(*intern_, key))) {
                    preaggregated = true;
                    break;
                }
            }
            if (preaggregated) continue;
            GroupKey key;
            key.host = text_of(*intern_, ev.hhash_id);
            key.name = text_of(*intern_, ev.name_id);
            key.time_bucket = agg::compute_time_bucket(ev.ts, cfg_);
            key.shard = agg::system_shard(key.host, key.name);
            agg::SystemAggregationMetrics metrics;
            metrics.count += 1;
            metrics.update_timestamp(ev.ts);
            for (const auto& [arg, value] : ev.args) {
                if (!is_metric(value)) continue;
                const std::string_view path = text_of(*intern_, arg);
                if (is_hash_arg(path)) continue;
                metrics.update_metric(path, number_of(value));
            }
            auto it = groups_.find(key);
            if (it == groups_.end())
                groups_.emplace(std::move(key), std::move(metrics));
            else
                it->second.merge_from(metrics);
        }
    }

    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}

    void merge(Fold& other) override {
        for (auto& [key, metrics] :
             static_cast<SystemCountersFold&>(other).groups_) {
            auto it = groups_.find(key);
            if (it == groups_.end())
                groups_.emplace(std::move(key), std::move(metrics));
            else
                it->second.merge_from(metrics);
        }
    }

    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return true;
    }

    const Groups& groups() const { return groups_; }

   private:
    const dftracer::utils::dstd::StringIntern* intern_;
    agg::AggregationConfig cfg_;
    Groups groups_;
};

}  // namespace

coro::CoroTask<bool> system_scan_collect(const ViewPlan& plan,
                                         dataframe::DataFrame& out_table,
                                         int shard_begin, int shard_end,
                                         const ProgressFn*) {
    const auto config = counter_config(plan);
    if (!config) co_return false;
    dftracer::utils::dstd::StringIntern intern;
    SystemCountersFold fold(intern, *config);
    ViewDefinition vdef = make_vdef(plan, /*for_aggregation=*/false);
    std::array<Fold*, 1> folds{&fold};
    co_await fuse(plan, vdef, folds, intern);
    out_table = build_counters_frame(fold.groups(), shard_begin, shard_end);
    co_return true;
}

}  // namespace dftracer::utils::trace::views::detail
