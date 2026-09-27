#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/trace/views/event_source.h>

#include <string_view>
#include <utility>

namespace dftracer::utils::index::extensions {

namespace {

namespace df = dftracer::utils::dataframe;
namespace views = trace::views::detail;

// Whether `n` is `ph in ["M", 4]` or `ph == "M"`: the metadata phase.
bool metadata_phase(const duql::QueryNode& n) {
    const auto* in = std::get_if<duql::InNode>(&n.data);
    if (in) {
        if (in->field.any || in->field.path != "ph" || in->values.set)
            return false;
        for (const auto& v : in->values.elements) {
            const auto* s = std::get_if<std::string>(&v.value);
            const auto* i = std::get_if<std::int64_t>(&v.value);
            const auto* u = std::get_if<std::uint64_t>(&v.value);
            if (!(s && *s == "M") && !(i && *i == 4) && !(u && *u == 4))
                return false;
        }
        return true;
    }
    const auto* c = std::get_if<duql::CompareNode>(&n.data);
    const auto* s = c ? std::get_if<std::string>(&c->value.value) : nullptr;
    return c && c->op == duql::CompareOp::EQ && !c->field.any &&
           c->field.path == "ph" && s && *s == "M";
}

// The top-level `and` terms of `n` that compare a field with a string.
void equalities(const duql::QueryNode& n,
                std::vector<std::pair<std::string, std::string>>& out) {
    if (const auto* a = std::get_if<duql::AndNode>(&n.data)) {
        equalities(*a->left, out);
        equalities(*a->right, out);
        return;
    }
    if (metadata_phase(n)) {
        out.emplace_back("ph", "M");
        return;
    }
    const auto* c = std::get_if<duql::CompareNode>(&n.data);
    if (!c || c->op != duql::CompareOp::EQ || c->field.any) return;
    if (const auto* s = std::get_if<std::string>(&c->value.value))
        out.emplace_back(c->field.path, *s);
}

// `n` without its top-level `and` terms that test the metadata phase; null
// when nothing else is left.
duql::QueryNodePtr without_phase(const duql::QueryNode& n) {
    if (const auto* a = std::get_if<duql::AndNode>(&n.data)) {
        auto l = without_phase(*a->left);
        auto r = without_phase(*a->right);
        if (!l) return r;
        if (!r) return l;
        return duql::make_node(duql::AndNode{std::move(l), std::move(r)});
    }
    if (metadata_phase(n)) return nullptr;
    return duql::clone(n);
}

RowSetFold::Value cell(const views::PodSource& src, std::string_view path) {
    using V = RowSetFold::Value;
    if (const auto* sp = src.special(path)) {
        switch (*sp) {
            case views::FoldEvent::Special::FALSE_VALUE:
                return false;
            case views::FoldEvent::Special::TRUE_VALUE:
                return true;
            case views::FoldEvent::Special::NULL_VALUE:
                return V{};
            default:
                return src.value(path);
        }
    }
    if (!src.has(path)) return V{};
    if (const auto n = src.number_typed(path)) {
        switch (n->domain) {
            case df::FieldStatDomain::I64:
                return n->i;
            case df::FieldStatDomain::U64:
                return n->u;
            case df::FieldStatDomain::F64:
                return n->d;
        }
    }
    return src.value(path);
}

std::uint64_t value_bytes(const RowSetFold::Value& v) {
    if (const auto* s = std::get_if<std::string>(&v)) return s->size() + 8;
    return 8;
}

std::string text_of(const RowSetFold::Value& v) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return {};
            else if constexpr (std::is_same_v<T, bool>)
                return x ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::string>)
                return x;
            else
                return std::to_string(x);
        },
        v);
}

std::vector<std::uint8_t> validity(const std::vector<RowSetFold::Value>& c) {
    std::vector<std::uint8_t> bits((c.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < c.size(); ++i)
        if (!std::holds_alternative<std::monostate>(c[i]))
            bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return bits;
}

// One column: the type all its values share; integers and doubles together
// as doubles; any other mix as text.
df::Series series_of(const std::vector<RowSetFold::Value>& c) {
    const auto n = static_cast<std::int64_t>(c.size());
    std::size_t kind = 0;
    bool mixed = false;
    bool numbers = true;
    for (const auto& v : c) {
        if (std::holds_alternative<std::monostate>(v)) continue;
        const std::size_t k = v.index();
        numbers = numbers && k >= 2 && k <= 4;
        if (kind && kind != k) mixed = true;
        kind = k;
    }
    const auto bits = validity(c);
    if (kind == 0) return df::Series::nulls(df::TypeId::String, n);
    if (!mixed && kind == 1) {
        std::vector<std::uint8_t> on((c.size() + 7) / 8, 0);
        for (std::size_t i = 0; i < c.size(); ++i)
            if (const auto* b = std::get_if<bool>(&c[i]); b && *b)
                on[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        return df::Series::flat(df::TypeId::Bool, on.data(), n, bits.data());
    }
    if (!mixed && kind == 2) {
        std::vector<std::int64_t> v(c.size(), 0);
        for (std::size_t i = 0; i < c.size(); ++i)
            if (const auto* x = std::get_if<std::int64_t>(&c[i])) v[i] = *x;
        return df::Series::flat_i64(v.data(), n, bits.data());
    }
    if (!mixed && kind == 3) {
        std::vector<std::uint64_t> v(c.size(), 0);
        for (std::size_t i = 0; i < c.size(); ++i)
            if (const auto* x = std::get_if<std::uint64_t>(&c[i])) v[i] = *x;
        return df::Series::flat(df::TypeId::Uint64, v.data(), n, bits.data());
    }
    if (numbers) {
        std::vector<double> v(c.size(), 0);
        for (std::size_t i = 0; i < c.size(); ++i)
            std::visit(
                [&](const auto& x) {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_arithmetic_v<T> &&
                                  !std::is_same_v<T, bool>)
                        v[i] = static_cast<double>(x);
                },
                c[i]);
        return df::Series::flat_f64(v.data(), n, bits.data());
    }
    std::vector<std::string> text;
    text.reserve(c.size());
    for (const auto& v : c) text.push_back(text_of(v));
    const std::vector<std::string_view> views(text.begin(), text.end());
    return df::Series::strings(views, bits.data());
}

}  // namespace

RowSetFold::RowSetFold(dftracer::utils::StringIntern& intern,
                       std::vector<IndexedRowSet> rowsets,
                       std::uint64_t budget_bytes)
    : intern_(&intern),
      rowsets_(std::move(rowsets)),
      budget_(budget_bytes),
      pending_(rowsets_.size()),
      sealed_(rowsets_.size()),
      seen_(rowsets_.size()) {
    for (std::size_t r = 0; r < rowsets_.size(); ++r) {
        pending_[r].columns.resize(rowsets_[r].columns.size());
        sealed_[r].columns.resize(rowsets_[r].columns.size());
        Guard g;
        std::vector<std::pair<std::string, std::string>> eq;
        if (rowsets_[r].filter) equalities(rowsets_[r].filter->root(), eq);
        for (const auto& [field, value] : eq) {
            if (field == "ph" && value == "M")
                g.phase = trace::RecordPhase::METADATA;
            if (field == "name") g.name = intern_->get_or_insert(value);
        }
        if (g.phase) {
            if (auto rest = without_phase(rowsets_[r].filter->root()))
                g.rest = duql::Query::from_node(std::move(rest));
        }
        guards_.push_back(std::move(g));
    }
}

std::unique_ptr<trace::views::detail::Fold> RowSetFold::slice() const {
    return std::make_unique<RowSetFold>(*intern_, rowsets_, budget_);
}

void RowSetFold::step(const trace::views::detail::FoldBatch& batch) {
    if (abandoned_ || rowsets_.empty()) return;
    for (const auto& e : batch.events) {
        const views::PodSource src(e, *intern_);
        for (std::size_t r = 0; r < rowsets_.size(); ++r) {
            const Guard& g = guards_[r];
            if (!e.by_path) {
                if (g.phase && e.phase != *g.phase) continue;
                if (g.name && e.name_id != *g.name) continue;
            }
            const IndexedRowSet& rs = rowsets_[r];
            const duql::Query* filter = !e.by_path && g.phase
                                            ? (g.rest ? &*g.rest : nullptr)
                                        : rs.filter ? &*rs.filter
                                                    : nullptr;
            if (filter && !views::pod_matches(*filter, e, *intern_, scratch_))
                continue;
            auto& cols = pending_[r].columns;
            for (std::size_t c = 0; c < rs.columns.size(); ++c) {
                Value v = cell(src, rs.columns[c].second);
                bytes_ += value_bytes(v);
                cols[c].push_back(std::move(v));
            }
        }
        if (bytes_ > budget_) {
            abandoned_ = true;
            for (auto& p : pending_)
                for (auto& c : p.columns) c = {};
            for (auto& s : sealed_)
                for (auto& c : s.columns) c = {};
            return;
        }
    }
}

void RowSetFold::absorb(std::size_t r, Rows& from) {
    auto& to = sealed_[r].columns;
    const std::size_t rows = from.columns.empty() ? 0 : from.columns[0].size();
    if (!rowsets_[r].distinct) {
        for (std::size_t c = 0; c < to.size(); ++c) {
            to[c].insert(to[c].end(),
                         std::make_move_iterator(from.columns[c].begin()),
                         std::make_move_iterator(from.columns[c].end()));
            from.columns[c].clear();
        }
        return;
    }
    std::string key;
    for (std::size_t i = 0; i < rows; ++i) {
        key.clear();
        for (const auto& col : from.columns) {
            key.push_back(static_cast<char>('0' + col[i].index()));
            key += text_of(col[i]);
            key.push_back('\x1f');
        }
        if (seen_[r].contains(key)) {
            for (const auto& col : from.columns) bytes_ -= value_bytes(col[i]);
            continue;
        }
        seen_[r].insert(key);
        for (std::size_t c = 0; c < to.size(); ++c)
            to[c].push_back(std::move(from.columns[c][i]));
    }
    for (auto& col : from.columns) col.clear();
}

void RowSetFold::seal_unit(const trace::views::detail::ScanUnit&) {
    for (std::size_t r = 0; r < pending_.size(); ++r) absorb(r, pending_[r]);
}

void RowSetFold::drop_unit(const trace::views::detail::ScanUnit&) {
    for (auto& p : pending_)
        for (auto& c : p.columns) c.clear();
}

void RowSetFold::merge(trace::views::detail::Fold& slice) {
    auto& other = static_cast<RowSetFold&>(slice);
    abandoned_ = abandoned_ || other.abandoned_;
    bytes_ += other.bytes_;
    for (std::size_t r = 0; r < sealed_.size(); ++r)
        absorb(r, other.sealed_[r]);
}

void RowSetFold::write(index::store::IndexWrite& w, int file_id) const {
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    if (abandoned_ || rowsets_.empty()) return;
    for (std::size_t r = 0; r < rowsets_.size(); ++r) {
        df::DataFrame f;
        for (std::size_t c = 0; c < rowsets_[r].columns.size(); ++c) {
            f.names.push_back(rowsets_[r].columns[c].first);
            f.columns.push_back(series_of(sealed_[r].columns[c]));
        }
        const std::vector<std::uint8_t> ipc = f.to_ipc();
        index::store::records::put_rowset(
            w, file_id, rowsets_[r].name,
            std::string_view(reinterpret_cast<const char*>(ipc.data()),
                             ipc.size()));
    }
    index::store::records::put_manifest(
        w, file_id, index::store::IndexExtension::ROWSET, 0);
#else
    (void)w;
    (void)file_id;
#endif
}

}  // namespace dftracer::utils::index::extensions
