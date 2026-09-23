#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/trace_viewer_detail.h>

#include <cstddef>
#include <string>
#include <vector>

namespace dftracer::utils::python::trace_viewer_detail {

namespace {

// name | cat | pid | tid | fhash | hhash | io_cat | acc_pat | rank | arg:<key>
// "fn(key)" or "fn(key, 'a', 'b')" -> transform + inner key text; returns
// false when `t` is not a call, leaving `t` to parse as a plain key.
bool split_transform(const std::string& t, GroupKey::Transform& tf,
                     std::vector<std::string>& targs, std::string& inner) {
    const auto lp = t.find('(');
    if (lp == std::string::npos || t.back() != ')') return false;
    const std::string fn = t.substr(0, lp);
    if (fn == "dirname") {
        tf = GroupKey::Transform::Dirname;
    } else if (fn == "basename") {
        tf = GroupKey::Transform::Basename;
    } else if (fn == "lower") {
        tf = GroupKey::Transform::Lower;
    } else if (fn == "bucket") {
        tf = GroupKey::Transform::Bucket;
    } else {
        return false;
    }
    std::string body = t.substr(lp + 1, t.size() - lp - 2);
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : body) {
        if (ch == ',') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    parts.push_back(cur);
    auto trim = [](std::string x) {
        const auto b = x.find_first_not_of(" \t'\"");
        const auto e = x.find_last_not_of(" \t'\"");
        return b == std::string::npos ? std::string() : x.substr(b, e - b + 1);
    };
    if (parts.empty()) return false;
    inner = trim(parts[0]);
    for (std::size_t i = 1; i < parts.size(); ++i)
        targs.push_back(trim(parts[i]));
    return !inner.empty();
}

}  // namespace

bool parse_group_key(const char* s, GroupKey& out) {
    std::string t(s);
    GroupKey::Transform tf = GroupKey::Transform::None;
    std::vector<std::string> targs;
    std::string inner;
    if (split_transform(t, tf, targs, inner)) t = inner;
    if (t == "name") {
        out = GroupKey::name();
    } else if (t == "cat") {
        out = GroupKey::cat();
    } else if (t == "pid") {
        out = GroupKey::pid();
    } else if (t == "tid") {
        out = GroupKey::tid();
    } else if (t == "fhash") {
        out = GroupKey::fhash();
    } else if (t == "hhash") {
        out = GroupKey::hhash();
    } else if (t == "io_cat") {
        out = GroupKey::io_cat();
    } else if (t == "acc_pat") {
        out = GroupKey::acc_pat();
    } else if (t == "file_path" || t == "resolved.fpath" || t == "r.fpath") {
        // resolved.fpath/hostname resolve fhash/hhash, so group_by resolves the
        // same aliases as select().
        out = GroupKey::file_path();
    } else if (t == "file_name") {
        out = GroupKey::file_name();
    } else if (t == "host_name" || t == "resolved.hostname" ||
               t == "r.hostname" || t == "resolved.host" || t == "r.host") {
        out = GroupKey::host_name();
    } else if (t == "rank") {
        out = GroupKey::rank();
    } else if (t.rfind("arg:", 0) == 0) {
        out = GroupKey::of_arg(t.substr(4));
    } else {
        // Schemaless fallback: any other name is a field, resolved like a
        // filter field (bare name = top-level then args; dotted/bracketed path
        // = that path from the event root).
        out = GroupKey::field(t);
    }
    out.transform = tf;
    out.transform_args = std::move(targs);
    return true;
}

// "count" | "op:field" | "argmax:field:by"  (op in sum/min/max/mean/var/std)
bool parse_agg_spec(const char* s, AggSpec& out) {
    std::string t(s);
    auto c1 = t.find(':');
    std::string op = t.substr(0, c1);
    std::string field, by;
    if (c1 != std::string::npos) {
        std::string rest = t.substr(c1 + 1);
        auto c2 = rest.find(':');
        field = rest.substr(0, c2);
        if (c2 != std::string::npos) by = rest.substr(c2 + 1);
    }
    if (op == "count")
        out = AggSpec(AggOp::Count, "", "", "");
    else if (op == "sum")
        out = AggSpec(AggOp::Sum, field);
    else if (op == "sumsq")
        out = AggSpec(AggOp::SumSq, field);
    else if (op == "min")
        out = AggSpec(AggOp::Min, field);
    else if (op == "max")
        out = AggSpec(AggOp::Max, field);
    else if (op == "mean")
        out = AggSpec(AggOp::Mean, field);
    else if (op == "var")
        out = AggSpec(AggOp::Var, field);
    else if (op == "std")
        out = AggSpec(AggOp::Std, field);
    else if (op == "skew")
        out = AggSpec(AggOp::Skew, field);
    else if (op == "kurt")
        out = AggSpec(AggOp::Kurt, field);
    else if (op == "hist")
        out = AggSpec(AggOp::Hist, field);
    else if (op == "argmax")
        out = AggSpec(AggOp::ArgMax, field, "", by);
    else if (op == "set_union" || op == "uniq")
        out = AggSpec(AggOp::SetUnion, field);
    else if (op == "busy")
        out = AggSpec(AggOp::Busy, field.empty() ? "dur" : field);
    else if (op == "concurrency")
        out = AggSpec(AggOp::Concurrency, field.empty() ? "dur" : field);
    else if (op == "utilization")
        out = AggSpec(AggOp::Utilization, field.empty() ? "dur" : field);
    else if (op == "active")
        out = AggSpec(AggOp::Active, field.empty() ? "dur" : field);
    else if (op == "pct")
        // pct:field:q  (explicit quantile)
        out = AggSpec(AggOp::Pct, field, "", "",
                      by.empty() ? 0.0 : std::stod(by));
    else if (op.size() >= 2 && op[0] == 'p') {
        // pNN shorthand: p50/p90/p99/p999 -> 0.NN
        double denom = 1.0;
        for (std::size_t i = 1; i < op.size(); ++i) {
            if (op[i] < '0' || op[i] > '9') return false;
            denom *= 10.0;
        }
        out = AggSpec(AggOp::Pct, field, op + "_" + field, "",
                      std::stod(op.substr(1)) / denom);
    } else
        return false;
    return true;
}

PyObject* tv_phase(TraceViewerObject* self, PyObject* arg) {
    const char* s = as_utf8(arg);
    if (!s) return nullptr;
    int ph;
    std::string t(s);
    if (t == "events")
        ph = (int)Phase::Events;
    else if (t == "counters")
        ph = (int)Phase::Counters;
    else if (t == "aggregated")
        ph = (int)Phase::Aggregated;
    else if (t == "metadata")
        ph = (int)Phase::Metadata;
    else if (t == "any")
        ph = (int)Phase::Any;
    else {
        PyErr_SetString(PyExc_ValueError,
                        "phase must be 'events', 'counters', 'aggregated', "
                        "'metadata', or 'any'");
        return nullptr;
    }
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->phase = ph;
    return (PyObject*)c;
}

}  // namespace dftracer::utils::python::trace_viewer_detail
