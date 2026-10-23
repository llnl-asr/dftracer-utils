#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_serialization.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_system_collect.h>

#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::trace::views::detail {

namespace {

namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace store = dftracer::utils::index::store;
namespace dataframe = dftracer::utils::dataframe;

// The system rows are index-wide, so they answer a plan only when the plan
// spans every file of one index and each file's partials are current. The
// database of that index; null otherwise.
std::shared_ptr<store::RocksDatabase> covering_index(const ViewPlan& plan) {
    if (plan.files.empty()) return nullptr;
    const std::string& index_path = plan.files.front().index_path;
    if (index_path.empty()) return nullptr;
    try {
        store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
        const auto config = agg::tier::read_config(*db.db());
        if (!config) return nullptr;
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
        if (current.size() != plan.files.size()) return nullptr;
        for (const auto& f : plan.files) {
            if (f.index_path != index_path) return nullptr;
            if (!current.count(store::internal::get_logical_path(f.file_path)))
                return nullptr;
        }
        return db.db();
    } catch (const std::exception&) {
        return nullptr;
    }
}

}  // namespace

bool system_collect(const ViewPlan& plan, dataframe::DataFrame& out_table,
                    int shard_begin, int shard_end, const ProgressFn*) {
    // The plan's event-side time_bucket/group_by do not apply here - system
    // rows carry their own (host,name,time_bucket) grain. The CF now has the
    // same shard prefix as the partials' system rows, so it range-splits
    // identically.
    const auto rdb = covering_index(plan);
    if (!rdb) return false;
    if (shard_end <= 0) shard_end = agg::AGG_KEY_NUM_SHARDS;

    auto for_each =
        [&](const std::function<void(std::string_view, std::string_view)>& cb) {
            agg::tier::for_each_system_row(
                *rdb, static_cast<std::uint16_t>(shard_begin),
                static_cast<std::uint16_t>(shard_end),
                [&cb](std::string_view k, std::string_view v) {
                    cb(k, v);
                    return true;
                });
        };

    // First pass: the sorted union of metric names, so the schema is stable.
    std::set<std::string> name_set;
    for_each([&](std::string_view, std::string_view val) {
        auto m = agg::deserialize_system_value(val);
        if (m.metrics)
            for (const auto& [n, _] : *m.metrics) name_set.insert(n);
    });
    const std::vector<std::string> metric_names(name_set.begin(),
                                                name_set.end());

    // System rows carry their own grain and a per-metric mean (a double), so
    // the value columns stay Float64 - no integer domain to preserve here.
    std::vector<std::string> hosts, names;
    std::vector<double> time_bucket, ts, te, count;
    std::vector<std::vector<double>> metric_vals(metric_names.size());
    for_each([&](std::string_view key, std::string_view val) {
        auto k = agg::deserialize_system_key(key);
        auto m = agg::deserialize_system_value(val);
        hosts.emplace_back(k.key.hhash);
        names.emplace_back(k.key.name);
        time_bucket.push_back(static_cast<double>(k.key.time_bucket));
        ts.push_back(static_cast<double>(m.ts));
        te.push_back(static_cast<double>(m.te));
        count.push_back(static_cast<double>(m.count));
        for (std::size_t i = 0; i < metric_names.size(); ++i) {
            double v = std::numeric_limits<double>::quiet_NaN();
            if (m.metrics) {
                auto it2 = m.metrics->find(metric_names[i]);
                if (it2 != m.metrics->end()) v = it2->second.mean();
            }
            metric_vals[i].push_back(v);
        }
    });

    dataframe::DataFrame b;
    auto add_str = [&](const char* nm, const std::vector<std::string>& v) {
        b.names.emplace_back(nm);
        b.columns.push_back(dataframe::Series::strings(v));
    };
    auto add_f64 = [&](const std::string& nm, const std::vector<double>& v) {
        b.names.push_back(nm);
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
    out_table = std::move(b);
    return true;
}

}  // namespace dftracer::utils::trace::views::detail
