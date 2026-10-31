#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/comparator/comparison_aggregation.h>
#include <dftracer/utils/trace/comparator/comparison_collapse.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <tuple>
#include <utility>
#include <variant>

namespace dftracer::utils::trace::comparator {

namespace {

constexpr int SHAPE_CATEGORIES = 1;
constexpr int SHAPE_NAMES = 2;
constexpr int SHAPE_PIDS = 4;

template <class T>
void sort_unique(dstd::Vector<T>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

template <class T>
void intersect_set(bool& has, dstd::Vector<T>& dst, const dstd::Vector<T>& src,
                   bool src_has) {
    if (!src_has) return;
    if (!has) {
        dst = src;
        has = true;
        return;
    }
    dstd::Vector<T> out;
    std::set_intersection(dst.begin(), dst.end(), src.begin(), src.end(),
                          std::back_inserter(out));
    dst = std::move(out);
}

template <class T>
dstd::Vector<T> union_set(const dstd::Vector<T>& a, const dstd::Vector<T>& b) {
    dstd::Vector<T> out;
    std::set_union(a.begin(), a.end(), b.begin(), b.end(),
                   std::back_inserter(out));
    return out;
}

int shape_of(const ComparisonFilter& f) {
    return (f.has_categories ? SHAPE_CATEGORIES : 0) |
           (f.has_names ? SHAPE_NAMES : 0) | (f.has_pids ? SHAPE_PIDS : 0);
}

bool is_category(const duql::FieldNode& f) { return !f.any && f.path == "cat"; }

void lower_literals(duql::ArrayNode& values) {
    for (auto& e : values.elements)
        if (auto* text = std::get_if<std::string>(&e.value))
            for (char& c : *text)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    values.set.reset();
}

// The text of a leaf on `cat` with the category compared lower-cased: the
// field becomes lower(cat) and the literals are lower-cased, and a pattern
// turns case-insensitive.
template <class Leaf>
dstd::String lowered_leaf(Leaf leaf) {
    const std::string text = duql::to_string(duql::QueryNode(std::move(leaf)));
    constexpr dstd::StringView FIELD = "cat";
    dstd::String out("lower(cat)");
    out.append(dstd::StringView(text).substr(FIELD.size()));
    return out;
}

dstd::String join(const dstd::String& left, dstd::StringView op,
                  const dstd::String& right) {
    dstd::String out("(");
    out += left;
    out += ") ";
    out.append(op);
    out += " (";
    out += right;
    out += ")";
    return out;
}

dstd::String fold_category(const duql::QueryNode& n) {
    if (const auto* c = std::get_if<duql::CompareNode>(&n.data)) {
        if (!is_category(c->field)) return dstd::String(duql::to_string(n));
        duql::CompareNode leaf = *c;
        if (auto* text = std::get_if<std::string>(&leaf.value.value))
            for (char& ch : *text)
                if (ch >= 'A' && ch <= 'Z')
                    ch = static_cast<char>(ch - 'A' + 'a');
        return lowered_leaf(std::move(leaf));
    }
    if (const auto* in = std::get_if<duql::InNode>(&n.data)) {
        if (!is_category(in->field)) return dstd::String(duql::to_string(n));
        duql::InNode leaf = *in;
        lower_literals(leaf.values);
        return lowered_leaf(std::move(leaf));
    }
    if (const auto* in = std::get_if<duql::NotInNode>(&n.data)) {
        if (!is_category(in->field)) return dstd::String(duql::to_string(n));
        duql::NotInNode leaf = *in;
        lower_literals(leaf.values);
        return lowered_leaf(std::move(leaf));
    }
    if (const auto* m = std::get_if<duql::MatchNode>(&n.data)) {
        if (!is_category(m->field)) return dstd::String(duql::to_string(n));
        duql::MatchNode leaf = *m;
        if (leaf.op == duql::MatchOp::Like) leaf.op = duql::MatchOp::Ilike;
        if (leaf.op == duql::MatchOp::Regex) leaf.op = duql::MatchOp::Iregex;
        leaf.compiled.reset();
        return dstd::String(duql::to_string(duql::QueryNode(std::move(leaf))));
    }
    if (const auto* a = std::get_if<duql::AndNode>(&n.data))
        return join(fold_category(*a->left), "and", fold_category(*a->right));
    if (const auto* o = std::get_if<duql::OrNode>(&n.data))
        return join(fold_category(*o->left), "or", fold_category(*o->right));
    if (const auto* nt = std::get_if<duql::NotNode>(&n.data))
        return dstd::String("not (") + fold_category(*nt->operand) + ")";
    return dstd::String(duql::to_string(n));
}

struct Translator {
    dstd::Optional<ComparisonFilter> fail() { return std::nullopt; }

    static dstd::Optional<std::uint64_t> as_uint(const duql::LiteralValue& v) {
        if (const auto* u = std::get_if<std::uint64_t>(&v)) return *u;
        if (const auto* i = std::get_if<std::int64_t>(&v))
            if (*i >= 0) return static_cast<std::uint64_t>(*i);
        return std::nullopt;
    }

    // Restricts `field` to the literals, or fails when the field or a
    // literal is not one a set can hold.
    dstd::Optional<ComparisonFilter> set_of(
        const duql::FieldNode& field,
        const dstd::Vector<const duql::LiteralValue*>& values) {
        ComparisonFilter f;
        if (field.any) return fail();
        if (field.path == "cat" || field.path == "name") {
            dstd::Vector<dstd::String> out;
            for (const auto* v : values) {
                const auto* text = std::get_if<std::string>(v);
                if (text == nullptr) return fail();
                std::string storage;
                const dstd::StringView s =
                    field.path == "cat"
                        ? trace::internal::to_lower_ascii(*text, storage)
                        : dstd::StringView(*text);
                out.emplace_back(s);
            }
            sort_unique(out);
            if (field.path == "cat") {
                f.has_categories = true;
                f.categories = std::move(out);
            } else {
                f.has_names = true;
                f.names = std::move(out);
            }
            return f;
        }
        if (field.path == "pid") {
            for (const auto* v : values) {
                const auto id = as_uint(*v);
                if (!id) return fail();
                f.pids.push_back(*id);
            }
            sort_unique(f.pids);
            f.has_pids = true;
            return f;
        }
        return fail();
    }

    dstd::Optional<ComparisonFilter> compare(const duql::CompareNode& c) {
        if (c.field.path == "ts" && !c.field.any) {
            const auto v = as_uint(c.value.value);
            if (!v) return fail();
            ComparisonFilter f;
            switch (c.op) {
                case duql::CompareOp::Eq:
                    f.ts_begin = *v;
                    f.ts_end = *v + 1;
                    break;
                case duql::CompareOp::Ge:
                    f.ts_begin = *v;
                    break;
                case duql::CompareOp::Gt:
                    f.ts_begin = *v + 1;
                    break;
                case duql::CompareOp::Lt:
                    f.ts_end = *v;
                    break;
                case duql::CompareOp::Le:
                    f.ts_end = *v + 1;
                    break;
                case duql::CompareOp::Ne:
                    return fail();
            }
            return f;
        }
        if (c.op != duql::CompareOp::Eq) return fail();
        return set_of(c.field, {&c.value.value});
    }

    dstd::Optional<ComparisonFilter> in_list(const duql::InNode& in) {
        dstd::Vector<const duql::LiteralValue*> values;
        for (const auto& e : in.values.elements) values.push_back(&e.value);
        return set_of(in.field, values);
    }

    dstd::Optional<ComparisonFilter> conj(const duql::AndNode& a) {
        auto l = node(*a.left);
        if (!l) return std::nullopt;
        auto r = node(*a.right);
        if (!r) return std::nullopt;
        intersect_set(l->has_categories, l->categories, r->categories,
                      r->has_categories);
        intersect_set(l->has_names, l->names, r->names, r->has_names);
        intersect_set(l->has_pids, l->pids, r->pids, r->has_pids);
        l->ts_begin = std::max(l->ts_begin, r->ts_begin);
        l->ts_end = std::min(l->ts_end, r->ts_end);
        return l;
    }

    dstd::Optional<ComparisonFilter> disj(const duql::OrNode& o) {
        auto l = node(*o.left);
        if (!l) return std::nullopt;
        auto r = node(*o.right);
        if (!r) return std::nullopt;
        const int shape = shape_of(*l);
        const bool one_field = shape == SHAPE_CATEGORIES ||
                               shape == SHAPE_NAMES || shape == SHAPE_PIDS;
        if (!one_field || shape != shape_of(*r) || l->ts_begin != 0 ||
            l->ts_end != UINT64_MAX || r->ts_begin != 0 ||
            r->ts_end != UINT64_MAX)
            return fail();
        l->categories = union_set(l->categories, r->categories);
        l->names = union_set(l->names, r->names);
        l->pids = union_set(l->pids, r->pids);
        return l;
    }

    dstd::Optional<ComparisonFilter> node(const duql::QueryNode& n) {
        if (const auto* c = std::get_if<duql::CompareNode>(&n.data))
            return compare(*c);
        if (const auto* in = std::get_if<duql::InNode>(&n.data))
            return in_list(*in);
        if (const auto* a = std::get_if<duql::AndNode>(&n.data))
            return conj(*a);
        if (const auto* o = std::get_if<duql::OrNode>(&n.data)) return disj(*o);
        return fail();
    }
};

// A cat/name pair of a row, as ids of the shared table.
struct RowGroup {
    GroupKey key;
    bool kept = false;
    bool data_transfer = false;
};

// Resolves the (cat, name) ids of one query result to the shared table once
// per pair.
class GroupCache {
   public:
    /// With a null `dst` the keys stay empty.
    GroupCache(const AggQueryResult& result, const ComparisonFilter& filter,
               dstd::StringIntern* dst)
        : result_(result), filter_(filter), dst_(dst) {}

    const RowGroup& of(std::uint32_t cat, std::uint32_t name) {
        const std::uint64_t id = (std::uint64_t{cat} << 32) | name;
        auto it = groups_.find(id);
        if (it != groups_.end()) return it->second;
        const dstd::StringView c = result_.text(cat);
        const dstd::StringView n = result_.text(name);
        RowGroup g;
        g.kept = filter_.keeps(c, n);
        if (g.kept) {
            if (dst_ != nullptr) {
                g.key.cat_id = dst_->get_or_insert(c);
                g.key.name_id = dst_->get_or_insert(n);
            }
            g.data_transfer = internal::is_data_transfer_op(c, n);
        }
        return groups_.emplace(id, g).first->second;
    }

   private:
    const AggQueryResult& result_;
    const ComparisonFilter& filter_;
    dstd::StringIntern* dst_;
    dstd::HashMap<std::uint64_t, RowGroup> groups_;
};

struct WindowKey {
    GroupKey group;
    std::uint64_t window = 0;
    bool operator==(const WindowKey&) const = default;
};

struct WindowKeyHash {
    using is_avalanching = void;
    std::size_t operator()(const WindowKey& k) const noexcept {
        return static_cast<std::size_t>(hash::splitmix64(
            hash::splitmix64((std::uint64_t{k.group.cat_id} << 32) |
                             k.group.name_id) ^
            k.window));
    }
};

// Per-window max across processes for a (cat, name, window).
struct WindowMax {
    double count = 0.0;
    double dur_mean = 0.0;
    double size_mean = 0.0;
    double xfer_size = 0.0;  // total_bytes / count
    double bandwidth = 0.0;  // total_bytes / total_dur_sec
};

struct GroupAccum {
    dstd::Vector<double> counts;
    dstd::Vector<double> dur_means;
    dstd::Vector<double> size_means;
    dstd::Vector<double> xfers;
    dstd::Vector<double> bws;
};

double safe_div(double a, double b) { return b > 0.0 ? a / b : 0.0; }

std::pair<double, double> mean_stdev(const dstd::Vector<double>& v) {
    if (v.empty()) return {0.0, 0.0};
    double sum = 0.0;
    for (double x : v) sum += x;
    double mean = sum / static_cast<double>(v.size());
    if (v.size() < 2) return {mean, 0.0};
    double sq = 0.0;
    for (double x : v) {
        double d = x - mean;
        sq += d * d;
    }
    return {mean, std::sqrt(sq / (static_cast<double>(v.size()) - 1.0))};
}

}  // namespace

bool ComparisonFilter::keeps(dstd::StringView cat,
                             dstd::StringView name) const {
    if (has_categories &&
        !std::binary_search(categories.begin(), categories.end(), cat,
                            [](const auto& a, const auto& b) {
                                return dstd::StringView(a) <
                                       dstd::StringView(b);
                            }))
        return false;
    if (has_names && !std::binary_search(names.begin(), names.end(), name,
                                         [](const auto& a, const auto& b) {
                                             return dstd::StringView(a) <
                                                    dstd::StringView(b);
                                         }))
        return false;
    return true;
}

bool ComparisonFilter::same_cut(const ComparisonFilter& other) const {
    if (expr.has_value() != other.expr.has_value()) return false;
    if (expr && expr->source() != other.expr->source()) return false;
    return has_pids == other.has_pids && pids == other.pids &&
           ts_begin == other.ts_begin && ts_end == other.ts_end;
}

index::schemas::dft::agg::AggregationConfig comparison_index_config() {
    index::schemas::dft::agg::AggregationConfig config;
    config.compute_percentiles = true;
    return config;
}

Result<ComparisonFilter> filter_from_query(const duql::Query& query) {
    Translator t;
    auto f = t.node(query.root());
    if (f) return std::move(*f);
    ComparisonFilter out;
    const dstd::String text = fold_category(query.root());
    auto folded = duql::Query::from_string(text);
    // The folded text is the query's own with `cat` wrapped in lower(), so a
    // parse failure is a bug of the rewrite.
    if (!folded)
        return make_error(ErrorCode::Internal,
                          "comparator: cannot fold the categories of " +
                              query.source() + ": " + folded.error().format());
    out.expr = std::move(*folded);
    return out;
}

TraceMetadata extract_metadata(const AggQueryResult& merged,
                               const ComparisonFilter& filter,
                               std::size_t file_count) {
    TraceMetadata meta;

    index::schemas::dft::agg::AggPartial all;
    std::uint64_t earliest_ts = UINT64_MAX;
    std::uint64_t latest_te = 0;

    GroupCache groups(merged, filter, nullptr);
    for (const auto& [key, p] : merged.rows) {
        const RowGroup& g = groups.of(key.cat, key.name);
        if (!g.kept) continue;
        all.pids.merge(p.pids);
        all.pid_tids.merge(p.pid_tids);
        all.files.merge(p.files);
        all.pid_files.merge(p.pid_files);

        meta.total_io_time_us += p.duration.sum;
        if (g.data_transfer) meta.total_bytes += p.size.sum;

        if (p.ts < earliest_ts) earliest_ts = p.ts;
        if (p.te > latest_te) latest_te = p.te;
    }

    // Both fall back to trace-file count when the trace carries no fhash.
    meta.file_count = all.files.empty() ? file_count : all.files.count();
    meta.proc_file_count =
        all.pid_files.empty() ? meta.file_count : all.pid_files.count();

    meta.process_count = all.pids.count();
    meta.thread_count = all.pid_tids.count();
    if (latest_te > earliest_ts) {
        meta.makespan_us = static_cast<double>(latest_te - earliest_ts);
    }
    return meta;
}

CollapsedMap collapse_by_group(const AggQueryResult& windows,
                               const AggQueryResult& merged,
                               const ComparisonFilter& filter,
                               dstd::StringIntern& dst) {
    dstd::HashMap<WindowKey, WindowMax, WindowKeyHash> per_window;
    {
        GroupCache groups(windows, filter, &dst);
        for (const auto& [key, p] : windows.rows) {
            const RowGroup& g = groups.of(key.cat, key.name);
            if (!g.kept) continue;
            auto& w = per_window[WindowKey{g.key, key.window}];

            double cnt = static_cast<double>(p.count);
            double dm = std::isnan(p.duration.mean()) ? 0.0 : p.duration.mean();
            double sm = std::isnan(p.size.mean()) ? 0.0 : p.size.mean();
            if (cnt > w.count) w.count = cnt;
            if (dm > w.dur_mean) w.dur_mean = dm;
            if (sm > w.size_mean) w.size_mean = sm;

            if (g.data_transfer) {
                double total_bytes = p.size.sum;
                double total_dur_us = p.duration.sum;
                double xfer = safe_div(total_bytes, cnt);
                double bw = safe_div(total_bytes, total_dur_us / 1e6);
                if (xfer > w.xfer_size) w.xfer_size = xfer;
                if (bw > w.bandwidth) w.bandwidth = bw;
            }
        }
    }

    dstd::HashMap<GroupKey, GroupAccum, GroupKeyHash> accum;
    for (const auto& [wk, w] : per_window) {
        auto& a = accum[wk.group];
        a.counts.push_back(w.count);
        a.dur_means.push_back(w.dur_mean);
        a.size_means.push_back(w.size_mean);
        a.xfers.push_back(w.xfer_size);
        a.bws.push_back(w.bandwidth);
    }

    CollapsedMap result;
    result.reserve(accum.size());
    for (auto& [gk, a] : accum) {
        CollapsedMetrics cm;
        cm.num_windows = a.dur_means.size();
        cm.count_mean = mean_stdev(a.counts).first;
        std::tie(cm.dur_mean_of_means, cm.dur_stdev_of_means) =
            mean_stdev(a.dur_means);
        std::tie(cm.size_mean_of_means, cm.size_stdev_of_means) =
            mean_stdev(a.size_means);
        std::tie(cm.xfer_mean, cm.xfer_stdev) = mean_stdev(a.xfers);
        std::tie(cm.bw_mean, cm.bw_stdev) = mean_stdev(a.bws);
        result.emplace(gk, std::move(cm));
    }

    GroupCache groups(merged, filter, &dst);
    for (const auto& [key, p] : merged.rows) {
        const RowGroup& g = groups.of(key.cat, key.name);
        if (!g.kept) continue;
        auto it = result.find(g.key);
        if (it != result.end()) it->second.merged.merge(p);
    }
    return result;
}

coro::CoroTask<Result<SideAggregate>> aggregate_side(
    dstd::Span<const std::string> files, dstd::StringView index_dir,
    const dstd::Vector<ComparisonFilter>& filters, std::uint64_t window_us,
    std::size_t workers, dstd::StringIntern& strings) {
    SideAggregate side;
    side.groups.resize(filters.size());

    AggQuery base;
    {
        dstd::Map<dstd::String, std::size_t> root_of;
        for (const auto& f : files) {
            dstd::String root(
                internal::determine_index_path(f, std::string(index_dir)));
            auto it = root_of.find(root);
            if (it == root_of.end()) {
                it = root_of.emplace(root, base.sources.size()).first;
                AggQuerySource src;
                src.index_path = std::move(root);
                base.sources.push_back(std::move(src));
            }
            base.sources[it->second].files.emplace_back(f);
        }
    }
    base.config = comparison_index_config();
    base.workers = workers;

    // Filters that cut the events alike share their queries.
    dstd::Vector<dstd::Vector<std::size_t>> cuts;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (filters[i].keeps_nothing()) continue;
        bool placed = false;
        for (auto& cut : cuts)
            if (filters[cut.front()].same_cut(filters[i])) {
                cut.push_back(i);
                placed = true;
                break;
            }
        if (!placed) cuts.push_back({i});
    }

    for (const auto& cut : cuts) {
        const ComparisonFilter& first = filters[cut.front()];
        AggQuery fine = base;
        fine.ts_begin = first.ts_begin;
        fine.ts_end = first.ts_end;
        if (first.has_pids) fine.pids = first.pids;
        // A set narrows the query only when every filter of the cut has one.
        bool all_cats = true;
        bool all_names = true;
        for (std::size_t i : cut) {
            all_cats = all_cats && filters[i].has_categories;
            all_names = all_names && filters[i].has_names;
        }
        for (std::size_t i : cut) {
            if (all_cats)
                fine.categories =
                    union_set(fine.categories, filters[i].categories);
            if (all_names) fine.names = union_set(fine.names, filters[i].names);
        }
        fine.filter = first.expr;
        AggQuery coarse = fine;

        fine.by_pid = true;
        fine.by_tid = true;
        fine.window_us = window_us;
        fine.quantiles = false;
        fine.args = false;
        coarse.quantiles = true;
        coarse.args = false;

        auto coarse_result = co_await run_agg_query(coarse);
        if (!coarse_result)
            co_return dstd::Unexpected(std::move(coarse_result).error());
        auto fine_result = co_await run_agg_query(fine);
        if (!fine_result)
            co_return dstd::Unexpected(std::move(fine_result).error());

        side.group_chunks_reread += fine_result->stats.chunks_reread;
        side.metadata_chunks_reread += coarse_result->stats.chunks_reread;
        for (std::size_t i : cut) {
            side.groups[i] = collapse_by_group(*fine_result, *coarse_result,
                                               filters[i], strings);
            if (i == 0)
                side.meta =
                    extract_metadata(*coarse_result, filters[i], files.size());
        }
    }
    if (!filters.empty() && filters[0].keeps_nothing())
        side.meta.file_count = side.meta.proc_file_count = files.size();
    co_return side;
}

}  // namespace dftracer::utils::trace::comparator
