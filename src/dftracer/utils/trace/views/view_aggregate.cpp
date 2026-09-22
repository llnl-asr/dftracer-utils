#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_resolver.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dataframe = dftracer::utils::dataframe;

namespace dftracer::utils::trace::views::detail {

AggSchema make_agg_schema(const ViewPlan& plan) {
    AggSchema s;
    s.spec_field.assign(plan.agg.size(), -1);
    s.spec_argmax.assign(plan.agg.size(), -1);
    s.spec_set.assign(plan.agg.size(), -1);
    auto field_index = [&](const std::string& f) -> int {
        for (std::size_t i = 0; i < s.fields.size(); ++i)
            if (s.fields[i] == f) return static_cast<int>(i);
        s.fields.push_back(f);
        return static_cast<int>(s.fields.size() - 1);
    };
    for (std::size_t i = 0; i < plan.agg.size(); ++i) {
        const auto& spec = plan.agg[i];
        if (spec.op == AggOp::ArgMax) {
            s.spec_argmax[i] = static_cast<int>(s.argmax_count++);
            continue;
        }
        // SetUnion collects a field's distinct string values, not a numeric
        // stat, so it gets its own slot and no FieldStat field.
        if (spec.op == AggOp::SetUnion) {
            if (!spec.field.empty())
                s.spec_set[i] = static_cast<int>(s.set_count++);
            continue;
        }
        if (is_occupancy_op(spec.op)) s.want_occupancy = true;
        // Count() reduces over rows (group count), not a field; every other op
        // (including Count(field)) reads its field's stat.
        if (spec.field.empty()) continue;
        s.spec_field[i] = field_index(spec.field);
    }
    s.field_scaled.assign(s.fields.size(), false);
    for (std::size_t i = 0; i < s.fields.size(); ++i)
        s.field_scaled[i] = (s.fields[i] == "ts" || s.fields[i] == "dur" ||
                             s.fields[i] == "te");
    // A field named by a Pct or Hist op gets one shared DDSketch slot (all
    // quantiles and the histogram on that field read the same sketch).
    s.field_sketch.assign(s.fields.size(), -1);
    for (std::size_t i = 0; i < plan.agg.size(); ++i) {
        if (plan.agg[i].op != AggOp::Pct && plan.agg[i].op != AggOp::Hist)
            continue;
        const int fi = s.spec_field[i];
        if (fi >= 0 && s.field_sketch[fi] < 0)
            s.field_sketch[fi] = static_cast<int>(s.sketch_count++);
    }
    if (s.want_occupancy) s.occ_cell_us = plan.occ_cell_us;
    for (const auto& r : plan.numeric_arg_aggs)
        if (r.op == AggOp::Pct) {
            s.dyn_sketch = true;
            break;
        }
    return s;
}

const AggSchema& ensure_schema(const ViewPlan& plan) {
    if (!plan.schema)
        plan.schema = std::make_shared<AggSchema>(make_agg_schema(plan));
    return *plan.schema;
}

const GroupResolver* ensure_resolver(const ViewPlan& plan) {
    bool needs = false;
    for (const auto& gk : plan.group_by)
        if (gk.kind == GroupKey::Kind::FilePath ||
            gk.kind == GroupKey::Kind::FileName ||
            gk.kind == GroupKey::Kind::HostName ||
            gk.kind == GroupKey::Kind::Rank) {
            needs = true;
            break;
        }
    if (!needs) return nullptr;
    if (!plan.resolver) {
        std::vector<std::string> paths;
        for (const auto& f : plan.files) {
            if (f.index_path.empty()) continue;
            bool seen = false;
            for (const auto& p : paths)
                if (p == f.index_path) {
                    seen = true;
                    break;
                }
            if (!seen) paths.push_back(f.index_path);
        }
        plan.resolver = std::make_shared<GroupResolver>(paths);
    }
    return plan.resolver.get();
}

void apply_ranks(const ViewPlan& plan,
                 std::unordered_map<std::uint64_t, std::string>& ranks) {
    if (ranks.empty()) return;
    ensure_resolver(plan);  // Rank counts as a resolved key, so this builds it
    if (!plan.resolver) return;
    for (auto& [pid, rank] : ranks)
        plan.resolver->set_rank(std::to_string(pid), std::move(rank));
    ranks.clear();
}

std::string resolve_group_value(const GroupResolver& r, GroupKey::Kind kind,
                                const std::string& hash) {
    if (kind == GroupKey::Kind::FilePath) return r.file_path(hash);
    if (kind == GroupKey::Kind::FileName) {
        const std::string& p = r.file_path(hash);
        const std::size_t slash = p.find_last_of('/');
        return slash == std::string::npos ? p : p.substr(slash + 1);
    }
    if (kind == GroupKey::Kind::HostName) return r.host_name(hash);
    if (kind == GroupKey::Kind::Rank) return r.rank(hash);
    return hash;
}

std::string apply_group_transform(const GroupKey& gk, std::string v) {
    switch (gk.transform) {
        case GroupKey::Transform::None:
            return v;
        case GroupKey::Transform::Dirname: {
            const auto slash = v.find_last_of('/');
            if (slash == std::string::npos) return std::string();
            return v.substr(0, slash);
        }
        case GroupKey::Transform::Basename: {
            const auto slash = v.find_last_of('/');
            return slash == std::string::npos ? v : v.substr(slash + 1);
        }
        case GroupKey::Transform::Lower: {
            for (auto& ch : v)
                ch = static_cast<char>(
                    ::tolower(static_cast<unsigned char>(ch)));
            return v;
        }
        case GroupKey::Transform::Bucket:
            for (const auto& b : gk.transform_args)
                if (v.find(b) != std::string::npos) return b;
            return std::string();
    }
    return v;
}

std::string group_col_name(const GroupKey& gk) {
    switch (gk.kind) {
        case GroupKey::Kind::Name:
            return "name";
        case GroupKey::Kind::Cat:
            return "cat";
        case GroupKey::Kind::Pid:
            return "pid";
        case GroupKey::Kind::Tid:
            return "tid";
        case GroupKey::Kind::Fhash:
            return "fhash";
        case GroupKey::Kind::Hhash:
            return "hhash";
        case GroupKey::Kind::IoCat:
            return "io_cat";
        case GroupKey::Kind::AccPat:
            return "acc_pat";
        case GroupKey::Kind::FilePath:
            return "file_path";
        case GroupKey::Kind::FileName:
            return "file_name";
        case GroupKey::Kind::HostName:
            return "host_name";
        case GroupKey::Kind::Rank:
            return "rank";
        case GroupKey::Kind::Arg:
        case GroupKey::Kind::Field:
            return gk.arg;
    }
    return {};
}

// Column name for a per-arg reduction: `<op>_<arg>`, matching the named-field
// convention (sum_field). The legacy mean path keeps the bare arg name. Pct
// uses the spec's out_name prefix (e.g. "p90_" from the pNN shorthand) so the
// quantile is legible; otherwise it falls back to "pct_".
std::string dyn_col_name(const AggSpec& spec, const std::string& key) {
    switch (spec.op) {
        case AggOp::Count:
            return "count_" + key;
        case AggOp::Sum:
            return "sum_" + key;
        case AggOp::Min:
            return "min_" + key;
        case AggOp::Max:
            return "max_" + key;
        case AggOp::SumSq:
            return "sumsq_" + key;
        case AggOp::Mean:
            return "mean_" + key;
        case AggOp::Var:
            return "var_" + key;
        case AggOp::Std:
            return "std_" + key;
        case AggOp::Skew:
            return "skew_" + key;
        case AggOp::Kurt:
            return "kurt_" + key;
        case AggOp::Pct: {
            std::string pfx = spec.out_name.empty() ? "pct_" : spec.out_name;
            if (pfx.back() != '_') pfx += '_';
            return pfx + key;
        }
        default:
            return key;
    }
}

std::string agg_col_name(const AggSpec& spec) {
    if (!spec.out_name.empty()) return spec.out_name;
    switch (spec.op) {
        case AggOp::Count:
            return "count";
        case AggOp::Sum:
            return "sum_" + spec.field;
        case AggOp::SumSq:
            return "sumsq_" + spec.field;
        case AggOp::Min:
            return "min_" + spec.field;
        case AggOp::Max:
            return "max_" + spec.field;
        case AggOp::Mean:
            return "mean_" + spec.field;
        case AggOp::Var:
            return "var_" + spec.field;
        case AggOp::Std:
            return "std_" + spec.field;
        case AggOp::Pct:
            return "pct_" + spec.field;  // Python sets a precise out_name
        case AggOp::Skew:
            return "skew_" + spec.field;
        case AggOp::Kurt:
            return "kurt_" + spec.field;
        case AggOp::Hist:
            return "hist_" + spec.field;
        case AggOp::ArgMax:
            return "argmax_" + spec.field;
        case AggOp::SetUnion:
            return "set_" + spec.field;
        case AggOp::Busy:
            return "busy";
        case AggOp::Concurrency:
            return "concurrency";
        case AggOp::Utilization:
            return "utilization";
        case AggOp::Active:
            return "active";
    }
    return {};
}

void project_columns(dftracer::utils::dataframe::DataFrame& batch,
                     const std::vector<std::string>& select) {
    if (select.empty()) return;
    const std::set<std::string> keep(select.begin(), select.end());
    std::vector<std::string> names;
    std::vector<dftracer::utils::dataframe::Series> columns;
    for (std::size_t i = 0; i < batch.names.size(); ++i) {
        if (keep.count(batch.names[i])) {
            names.push_back(std::move(batch.names[i]));
            columns.push_back(std::move(batch.columns[i]));
        }
    }
    batch.names = std::move(names);
    batch.columns = std::move(columns);
}

dataframe::DataFrame apply_agg_post_ops(dataframe::DataFrame batch,
                                        const ViewPlan& plan) {
    if (!plan.sort_col.empty())
        batch = batch.sort_by(plan.sort_col, plan.sort_desc);
    if (!plan.topk_col.empty())
        batch = batch.topk(plan.topk_col, plan.topk_k, plan.topk_largest);
    if (plan.offset || plan.limit) {
        const std::int64_t off = static_cast<std::int64_t>(plan.offset);
        const std::int64_t len = plan.limit
                                     ? static_cast<std::int64_t>(plan.limit)
                                     : batch.num_rows();
        batch = batch.slice(off, len);
    }
    project_columns(batch, plan.select);
    return batch;
}

}  // namespace dftracer::utils::trace::views::detail
