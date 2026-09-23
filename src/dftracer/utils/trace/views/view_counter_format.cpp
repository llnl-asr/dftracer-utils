#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_counter_format.h>
#include <dftracer/utils/trace/views/view_executor.h>

#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace dftracer::utils::trace::views::detail {

using json::append_json_escaped;

namespace {

void append_number(double v, std::string& out) {
    if (v == static_cast<double>(static_cast<std::int64_t>(v)) && v < 9.0e15 &&
        v > -9.0e15) {
        out += std::to_string(static_cast<std::int64_t>(v));
    } else {
        out += std::to_string(v);
    }
}

int col_index(const std::vector<std::string>& cols, const std::string& name) {
    for (std::size_t i = 0; i < cols.size(); ++i)
        if (cols[i] == name) return static_cast<int>(i);
    return -1;
}

}  // namespace

std::string counter_line(const std::vector<std::string>& group_cols,
                         const std::vector<std::string>& keys,
                         const std::vector<std::string>& value_cols,
                         const std::vector<double>& values) {
    const int i_bucket = col_index(group_cols, "time_bucket");
    const int i_name = col_index(group_cols, "name");
    const int i_cat = col_index(group_cols, "cat");
    const int i_pid = col_index(group_cols, "pid");
    const int i_tid = col_index(group_cols, "tid");

    const std::string& name =
        i_name >= 0 ? keys[i_name] : (i_cat >= 0 ? keys[i_cat] : "");
    const std::string cat = i_cat >= 0 ? keys[i_cat] : "";

    std::string s = "{\"name\":\"";
    append_json_escaped(s, name);
    s += "\",\"cat\":\"";
    append_json_escaped(s, cat);
    s += "\",\"pid\":";
    s += i_pid >= 0 && !keys[i_pid].empty() ? keys[i_pid] : "0";
    s += ",\"tid\":";
    s += i_tid >= 0 && !keys[i_tid].empty() ? keys[i_tid] : "0";
    s += ",\"ts\":";
    s += i_bucket >= 0 && !keys[i_bucket].empty() ? keys[i_bucket] : "0";
    s += ",\"ph\":";
    s += std::to_string(phase_to_int(RecordPhase::COUNTER));
    s += ",\"type\":";
    s += std::to_string(event_type_to_int(EventType::PSUTIL));
    s += ",\"args\":{";

    bool first = true;
    for (std::size_t j = 0; j < value_cols.size(); ++j) {
        if (!first) s += ",";
        first = false;
        s += "\"";
        append_json_escaped(s, value_cols[j]);
        s += "\":";
        append_number(values[j], s);
    }
    for (std::size_t k = 0; k < group_cols.size(); ++k) {
        const auto& c = group_cols[k];
        if (c == "time_bucket" || c == "name" || c == "cat" || c == "pid" ||
            c == "tid")
            continue;
        if (!first) s += ",";
        first = false;
        s += "\"";
        append_json_escaped(s, c);
        s += "\":\"";
        append_json_escaped(s, keys[k]);
        s += "\"";
    }
    s += "}}";
    return s;
}

namespace {

double col_cell_double(const dftracer::utils::dataframe::Series& c,
                       std::int64_t r) {
    using dftracer::utils::dataframe::TypeId;
    switch (c.type()) {
        case TypeId::Int64:
            return static_cast<double>(c.data<std::int64_t>()[r]);
        case TypeId::Uint64:
            return static_cast<double>(c.data<std::uint64_t>()[r]);
        case TypeId::Float64:
            return c.data<double>()[r];
        default:
            return 0.0;
    }
}

}  // namespace

void emit_counters_from_state(const dftracer::utils::dataframe::AggState& state,
                              const ViewPlan& plan, ExportSink& sink) {
    namespace df = dftracer::utils::dataframe;
    df::DataFrame f = finalize_engine_result(state, plan);

    std::vector<std::string> group_cols;
    if (plan.time_bucket_us > 0) group_cols.push_back("time_bucket");
    for (const auto& gk : plan.group_by)
        group_cols.push_back(group_col_name(gk));

    // Columns that are group keys, spec values, or occupancy metadata are not
    // dyn args; the rest of the frame is the per-arg dyn columns.
    std::set<std::string> non_dyn(group_cols.begin(), group_cols.end());
    non_dyn.insert("busy_cell_us");
    std::vector<std::string> spec_cols;
    if (plan.agg.empty()) {
        spec_cols.push_back("count");
        non_dyn.insert("count");
    } else {
        for (const auto& spec : plan.agg) {
            const std::string nm = agg_col_name(spec);
            non_dyn.insert(nm);
            if (spec.op == AggOp::ArgMax) continue;  // counter args are numeric
            spec_cols.push_back(nm);
        }
    }
    std::vector<std::string> dyn_cols;
    for (const std::string& nm : f.names)
        if (!non_dyn.count(nm)) dyn_cols.push_back(nm);

    std::vector<int> gi(group_cols.size());
    for (std::size_t k = 0; k < group_cols.size(); ++k)
        gi[k] = static_cast<int>(f.column_index(group_cols[k]));
    std::vector<int> si(spec_cols.size());
    for (std::size_t k = 0; k < spec_cols.size(); ++k)
        si[k] = static_cast<int>(f.column_index(spec_cols[k]));
    std::vector<int> di(dyn_cols.size());
    for (std::size_t k = 0; k < dyn_cols.size(); ++k)
        di[k] = static_cast<int>(f.column_index(dyn_cols[k]));

    const std::int64_t nrows = f.num_rows();
    for (std::int64_t r = 0; r < nrows; ++r) {
        std::vector<std::string> keys(group_cols.size());
        for (std::size_t k = 0; k < group_cols.size(); ++k) {
            const df::Series& c = f.columns[static_cast<std::size_t>(gi[k])];
            if (!c.is_null(r)) keys[k] = std::string(c.string_at(r));
        }
        std::vector<std::string> value_cols;
        std::vector<double> values;
        for (std::size_t k = 0; k < spec_cols.size(); ++k) {
            value_cols.push_back(spec_cols[k]);
            values.push_back(
                col_cell_double(f.columns[static_cast<std::size_t>(si[k])], r));
        }
        // Sparse per group: emit only the args this group actually saw,
        // matching emit_group_counter iterating a.dyn.
        for (std::size_t k = 0; k < dyn_cols.size(); ++k) {
            const df::Series& c = f.columns[static_cast<std::size_t>(di[k])];
            if (c.is_null(r)) continue;
            const double v = col_cell_double(c, r);
            if (std::isnan(v)) continue;
            value_cols.push_back(dyn_cols[k]);
            values.push_back(v);
        }
        sink.write(counter_line(group_cols, keys, value_cols, values));
        sink.write("\n");
    }
}

}  // namespace dftracer::utils::trace::views::detail
