// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/hash/value_hash.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/detail/ast.h>
#include <dftracer/utils/duql/numbers.h>
#include <dftracer/utils/duql/term.h>
#include <dftracer/utils/trace/index/extensions/bloom_filter.h>
#include <dftracer/utils/trace/index/extensions/extension_registry.h>
#include <dftracer/utils/trace/index/extensions/summaries.h>
#include <dftracer/utils/trace/index/plan/condition.h>
#include <dftracer/utils/trace/index/plan/file_index_data.h>
#include <dftracer/utils/trace/index/store/file.h>
#include <dftracer/utils/trace/index/tables.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace dftracer::utils::trace::index::plan {

using trace::index::extensions::kinds::Zone;

namespace {

// A literal as the index keys it; a bool is 1 or 0, as the build records it.
dstd::String literal_to_string(const duql::LiteralNode& lit) {
    return std::visit(
        [](auto&& v) -> dstd::String {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, dstd::String>)
                return dstd::String(v);
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "1" : "0";
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return dstd::to_string(v);
            else if constexpr (std::is_same_v<T, std::uint64_t>)
                return dstd::to_string(v);
            else if constexpr (std::is_same_v<T, double>)
                return trace::index::extensions::canonical_number_text(v);
            else
                return {};
        },
        lit.value);
}

// A literal as an extension value. A string value borrows `lit`.
void to_value(const duql::LiteralNode& lit,
              trace::index::extensions::ExtensionValue& out) {
    using trace::index::extensions::ExtensionValue;
    std::visit(
        [&out](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            out = ExtensionValue{};
            if constexpr (std::is_same_v<T, dstd::String>) {
                out.kind = ExtensionValue::Kind::String;
                out.str = &v;
            } else if constexpr (std::is_same_v<T, bool>) {
                out.kind = ExtensionValue::Kind::Bool;
                out.b = v;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out.kind = ExtensionValue::Kind::Int64;
                out.i = v;
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                if (v > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())) {
                    out.kind = ExtensionValue::Kind::UInt64;
                    out.u = v;
                } else {
                    out.kind = ExtensionValue::Kind::Int64;
                    out.i = static_cast<std::int64_t>(v);
                }
            } else {
                out.kind = ExtensionValue::Kind::Double;
                out.f = v;
            }
        },
        lit.value);
}

trace::index::extensions::ExtensionOp compare_op(duql::CompareOp op) {
    using trace::index::extensions::ExtensionOp;
    switch (op) {
        case duql::CompareOp::Eq:
            return ExtensionOp::Eq;
        case duql::CompareOp::Ne:
            return ExtensionOp::Ne;
        case duql::CompareOp::Lt:
            return ExtensionOp::Lt;
        case duql::CompareOp::Le:
            return ExtensionOp::Le;
        case duql::CompareOp::Gt:
            return ExtensionOp::Gt;
        case duql::CompareOp::Ge:
            return ExtensionOp::Ge;
    }
    return ExtensionOp::Eq;
}

trace::index::extensions::ExtensionOp match_op(duql::MatchOp op) {
    using trace::index::extensions::ExtensionOp;
    switch (op) {
        case duql::MatchOp::Like:
            return ExtensionOp::Like;
        case duql::MatchOp::Ilike:
            return ExtensionOp::Ilike;
        case duql::MatchOp::Regex:
            return ExtensionOp::Regex;
        case duql::MatchOp::Iregex:
            return ExtensionOp::Iregex;
        case duql::MatchOp::Icontains:
            return ExtensionOp::Icontains;
    }
    return ExtensionOp::Like;
}

// One query leaf as an extension leaf. `items` backs the leaf's list, so
// the holder stays where it is built.
struct LeafHolder {
    trace::index::extensions::ExtensionLeaf leaf;
    dstd::Vector<trace::index::extensions::ExtensionValue> items;

    // False when `node` is not a compare, `in`, `not in` or match leaf.
    bool assign(const duql::detail::QueryNode& node) {
        using trace::index::extensions::ExtensionOp;
        const duql::detail::FieldNode* field = nullptr;
        auto array = [&](const duql::detail::ArrayNode& values) {
            items.resize(values.elements.size());
            for (std::size_t i = 0; i < items.size(); ++i)
                to_value(values.elements[i], items[i]);
            leaf.items =
                dstd::Span<const trace::index::extensions::ExtensionValue>(
                    items.data(), items.size());
        };
        if (const auto* cmp =
                std::get_if<duql::detail::CompareNode>(&node.data)) {
            to_value(cmp->value, leaf.value);
            field = &cmp->field;
            leaf.op = compare_op(cmp->op);
        } else if (const auto* in =
                       std::get_if<duql::detail::InNode>(&node.data)) {
            array(in->values);
            field = &in->field;
            leaf.op = ExtensionOp::In;
        } else if (const auto* nin =
                       std::get_if<duql::detail::NotInNode>(&node.data)) {
            array(nin->values);
            field = &nin->field;
            leaf.op = ExtensionOp::NotIn;
        } else if (const auto* mt =
                       std::get_if<duql::detail::MatchNode>(&node.data)) {
            if (mt->negated) return false;
            leaf.value.kind =
                trace::index::extensions::ExtensionValue::Kind::String;
            leaf.value.str = &mt->pattern;
            field = &mt->field;
            leaf.op = match_op(mt->op);
        } else {
            return false;
        }
        leaf.path = field->path;
        leaf.any = field->any;
        return true;
    }

    bool has_uint64_above_int64() const {
        using trace::index::extensions::ExtensionValue;
        if (leaf.value.kind == ExtensionValue::Kind::UInt64) return true;
        for (const auto& item : items)
            if (item.kind == ExtensionValue::Kind::UInt64) return true;
        return false;
    }
};

bool is_range_op(duql::CompareOp op) {
    return op == duql::CompareOp::Gt || op == duql::CompareOp::Ge ||
           op == duql::CompareOp::Lt || op == duql::CompareOp::Le;
}

// The histogram counts whole timestamps, so a bound that is not a
// non-negative integer (a fraction, a negative number) leaves it out. A string
// that is not an integer is a malformed query.
dstd::Optional<std::uint64_t> timestamp_bound(
    FileIndexData& data, dstd::StringView text,
    const duql::detail::CompareNode& n) {
    std::uint64_t out = 0;
    const char* end = text.data() + text.size();
    const auto parsed = dstd::from_chars(text, out);
    if (parsed.ec == std::errc{} && parsed.ptr == end) return out;
    if (std::holds_alternative<dstd::String>(n.value.value))
        data.fail(common::make_error<
                      "ts compared with \"{}\", which is not an integer "
                      "timestamp">(common::ErrorCode::InvalidArgument, text)
                      .error());
    return dstd::nullopt;
}

// A zone recording that no record of its chunk holds a value of the path.
bool absent(const Zone& z) { return z.present && *z.present == 0; }

// Zero records in the queried range means the chunk can be skipped.
bool histogram_has_events(const Zone& z, duql::CompareOp op,
                          std::uint64_t ts_val) {
    if (!z.histogram || z.histogram->empty()) return true;
    const auto& hist = *z.histogram;
    switch (op) {
        case duql::CompareOp::Gt:
            return hist.count_in_range(ts_val + 1, UINT64_MAX) > 0;
        case duql::CompareOp::Ge:
            return hist.count_in_range(ts_val, UINT64_MAX) > 0;
        case duql::CompareOp::Lt:
            return hist.count_in_range(0, ts_val) > 0;
        case duql::CompareOp::Le:
            return hist.count_in_range(0, ts_val + 1) > 0;
        default:
            return true;
    }
}

// An all-match proof holds only if the kind observed every line of the chunk
// (no metadata or other unobserved records) and every observed record carries
// the path.
bool covers_every_line(FileIndexData& d, std::uint64_t ckpt,
                       std::uint64_t observed,
                       dstd::Optional<std::uint64_t> present) {
    const auto ln = d.chunk_lines.find(ckpt);
    return observed != 0 && ln != d.chunk_lines.end() &&
           ln->second == observed && present && *present == observed;
}

dstd::Optional<std::uint64_t> counted(const GranuleCounts& g) {
    const auto* vc = g.get();
    if (!vc) return dstd::nullopt;
    std::uint64_t carried = 0;
    for (const auto& [value, count] : *vc) carried += count;
    return carried;
}

ChunkSet intersect(const ChunkSet& a, const ChunkSet& b) {
    ChunkSet out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                          std::inserter(out, out.begin()));
    return out;
}

ChunkSet difference(const ChunkSet& a, const ChunkSet& b) {
    ChunkSet out;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                        std::inserter(out, out.begin()));
    return out;
}

// Paths named by equality-shaped leaves, the only ones the file blooms test.
void collect_equality_paths(const duql::detail::QueryNode& node,
                            dstd::StringViewSet& out) {
    std::visit(
        [&out](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::detail::CompareNode>) {
                if (n.op == duql::CompareOp::Eq) out.insert(n.field.path);
            } else if constexpr (std::is_same_v<T, duql::detail::InNode>) {
                out.insert(n.field.path);
            } else if constexpr (std::is_same_v<T, duql::detail::AndNode> ||
                                 std::is_same_v<T, duql::detail::OrNode>) {
                collect_equality_paths(*n.left, out);
                collect_equality_paths(*n.right, out);
            } else if constexpr (std::is_same_v<T, duql::detail::NotNode>) {
                collect_equality_paths(*n.operand, out);
            }
        },
        node.data);
}

dstd::StringView text_of(dstd::StringView s) { return s; }
dstd::StringView text_of(
    const dstd::Pair<dstd::String, trace::index::store::PathStat>& entry) {
    return entry.first;
}

// Whether a record carrying the fields `paths` may resolve `path` the way the
// evaluator does: as written, under "args.", or as an object or dotted flat
// key along the way.
template <class Paths>
bool path_may_exist(const Paths& paths, dstd::StringView path) {
    auto near = [&](dstd::StringView p) {
        for (const auto& entry : paths) {
            const dstd::StringView s = text_of(entry);
            if (s == p ||
                (s.size() > p.size() && s.starts_with(p) &&
                 s[p.size()] == '.') ||
                (p.size() > s.size() && p.starts_with(s) && p[s.size()] == '.'))
                return true;
        }
        return false;
    };
    return near(path) ||
           (!path.starts_with("args.") && near(dstd::fmt<"args.{}">(path)));
}

// Whether some record name satisfies `leaf`, a leaf on `name`; a pattern
// leaf counts as satisfiable.
bool name_may_match(dstd::Span<const dstd::String> names,
                    const duql::detail::QueryNode& leaf) {
    auto text = [](const duql::LiteralNode& lit) -> const dstd::String* {
        return std::get_if<dstd::String>(&lit.value);
    };
    return std::visit(
        [&](auto&& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::detail::CompareNode>) {
                const dstd::String* v = text(n.value);
                if (!v) return false;
                for (const auto& name : names) {
                    const int c = name.compare(dstd::StringView(*v));
                    switch (n.op) {
                        case duql::CompareOp::Eq:
                            if (c == 0) return true;
                            break;
                        case duql::CompareOp::Ne:
                            if (c != 0) return true;
                            break;
                        case duql::CompareOp::Gt:
                            if (c > 0) return true;
                            break;
                        case duql::CompareOp::Lt:
                            if (c < 0) return true;
                            break;
                        case duql::CompareOp::Ge:
                            if (c >= 0) return true;
                            break;
                        case duql::CompareOp::Le:
                            if (c <= 0) return true;
                            break;
                    }
                }
                return false;
            } else if constexpr (std::is_same_v<T, duql::detail::InNode> ||
                                 std::is_same_v<T, duql::detail::NotInNode>) {
                constexpr bool in = std::is_same_v<T, duql::detail::InNode>;
                for (const auto& name : names) {
                    bool listed = false;
                    for (const auto& e : n.values.elements) {
                        const dstd::String* v = text(e);
                        if (v && dstd::StringView(*v) == name) listed = true;
                    }
                    if (listed == in) return true;
                }
                return false;
            } else {
                return true;
            }
        },
        leaf.data);
}

const duql::detail::FieldNode* leaf_field(const duql::detail::QueryNode& leaf) {
    return std::visit(
        [](auto&& n) -> const duql::detail::FieldNode* {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::detail::CompareNode> ||
                          std::is_same_v<T, duql::detail::InNode> ||
                          std::is_same_v<T, duql::detail::NotInNode> ||
                          std::is_same_v<T, duql::detail::MatchNode>)
                return &n.field;
            else
                return nullptr;
        },
        leaf.data);
}

/// dft.metadata, over metadata records. The evaluator makes every leaf false
/// on a missing field, so a chunk none of whose records has the field is
/// ruled out.
class MetadataRecords final : public Condition {
   public:
    explicit MetadataRecords(
        const dstd::HashMap<std::uint64_t, trace::index::store::ChunkMetadata>&
            meta)
        : meta_(meta) {}

    IndexExtension extension() const override {
        return IndexExtension::Metadata;
    }

    dstd::Optional<ChunkSet> may_match(const duql::detail::QueryNode& leaf,
                                       const ChunkSet& candidates) override {
        const duql::detail::FieldNode* field = leaf_field(leaf);
        if (!field) return dstd::nullopt;
        ChunkSet out;
        for (auto c : candidates) {
            const auto it = meta_.find(c);
            if (it == meta_.end() ||
                may_hold(it->second, leaf, dstd::String(field->path)))
                out.insert(c);
        }
        return out;
    }

   private:
    static bool may_hold(const trace::index::store::ChunkMetadata& m,
                         const duql::detail::QueryNode& leaf,
                         dstd::StringView path) {
        if (m.records == 0) return false;
        if (m.paths && !path_may_exist(*m.paths, path)) return false;
        // A record without a top-level name resolves `name` under args.
        if (path == "name" && m.names &&
            !std::binary_search(m.names->begin(), m.names->end(), ""))
            return name_may_match(*m.names, leaf);
        return true;
    }

    const dstd::HashMap<std::uint64_t, trace::index::store::ChunkMetadata>&
        meta_;
};

// The summaries answer every leaf the planner prunes with. The planner reads
// each kind's data once per file and asks the compiled query about it.
using trace::index::extensions::BloomSummary;
using trace::index::extensions::CountsSummary;
using trace::index::extensions::PostingsSummary;
using trace::index::extensions::ZonemapSummary;

// A numeric path never equals a string, so only a path that may hold strings
// also probes a number literal by its text.
dataframe::TypeId probe_type(bool may_hold_strings) {
    return may_hold_strings ? dataframe::TypeId::Unknown
                            : dataframe::TypeId::Float64;
}

/// bloom kind, file level: rules the whole file out on equality leaves.
class FileBloom final : public Condition {
   public:
    explicit FileBloom(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override { return IndexExtension::Bloom; }
    bool file_may_match(const duql::detail::QueryNode& root) override {
        dstd::StringViewSet paths;
        collect_equality_paths(root, paths);
        if (paths.empty()) return true;
        return may_match_file(root);
    }
    dstd::Optional<ChunkSet> may_match(const duql::detail::QueryNode&,
                                       const ChunkSet&) override {
        return dstd::nullopt;
    }

   private:
    bool may_contain(const duql::detail::QueryNode& leaf,
                     const duql::detail::FieldNode& field) {
        const dstd::String path(field.path);
        const auto* bf = d_.file_bloom(path);
        if (!bf) return true;
        LeafHolder h;
        if (!h.assign(leaf)) return true;
        const auto q = BloomSummary({}, 0).compile_bloom(
            h.leaf, probe_type(d_.may_hold_strings(path)));
        return !q || q->may_match_filter(*bf);
    }

    bool may_match_file(const duql::detail::QueryNode& node) {
        return std::visit(
            [&](auto&& n) -> bool {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, duql::detail::CompareNode>) {
                    if (n.op != duql::CompareOp::Eq) return true;
                    return may_contain(node, n.field);
                } else if constexpr (std::is_same_v<T, duql::detail::InNode>) {
                    if (n.values.elements.size() > SEMI_JOIN_CAP) return true;
                    return may_contain(node, n.field);
                } else if constexpr (std::is_same_v<T, duql::detail::AndNode>) {
                    return may_match_file(*n.left) && may_match_file(*n.right);
                } else if constexpr (std::is_same_v<T, duql::detail::OrNode>) {
                    return may_match_file(*n.left) || may_match_file(*n.right);
                } else {
                    return true;
                }
            },
            node.data);
    }

    FileIndexData& d_;
};

// The path of an `exists(path)` leaf in the catalog's form (`a.0`, not
// `a[0]`), or nullopt for any other leaf.
dstd::Optional<dstd::String> exists_path(const duql::detail::QueryNode& node) {
    const auto* leaf = std::get_if<duql::detail::ExprLeaf>(&node.data);
    if (!leaf) return dstd::nullopt;
    const auto* call = std::get_if<duql::TCall>(&leaf->term->node);
    if (!call || call->fn != duql::Fn::Exists) return dstd::nullopt;
    const auto* field = std::get_if<duql::TField>(&call->args[0]->node);
    if (!field) return dstd::nullopt;
    dstd::String out;
    for (const char c : field->base) {
        if (c == '[')
            out += '.';
        else if (c != ']')
            out += c;
    }
    return out;
}

// Whether `p` lies below `base`.
bool under(dstd::StringView p, dstd::StringView base) {
    return p.size() > base.size() && p.starts_with(base) &&
           p[base.size()] == '.';
}

/// core.catalog, file level: `exists(p)` rules the file out when no catalog
/// path is `p` or lies under it.
class CatalogPresence final : public Condition {
   public:
    explicit CatalogPresence(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override {
        return IndexExtension::Catalog;
    }
    bool file_may_match(const duql::detail::QueryNode& root) override {
        return may_match_file(root);
    }
    dstd::Optional<ChunkSet> may_match(const duql::detail::QueryNode&,
                                       const ChunkSet&) override {
        return dstd::nullopt;
    }

   private:
    bool may_match_file(const duql::detail::QueryNode& node) {
        if (const auto* a = std::get_if<duql::detail::AndNode>(&node.data))
            return may_match_file(*a->left) && may_match_file(*a->right);
        if (const auto* o = std::get_if<duql::detail::OrNode>(&node.data))
            return may_match_file(*o->left) || may_match_file(*o->right);
        const auto path = exists_path(node);
        return !path || may_exist(*path);
    }

    bool may_exist(dstd::StringView path) {
        if (!d_.by_path && unrecorded(path)) return true;
        const auto& catalog = d_.catalog();
        if (catalog.empty()) return true;
        return path_may_exist(catalog, path);
    }

    FileIndexData& d_;
};

/// postings kind: exact chunks for equality and `in`.
class Postings final : public Condition {
   public:
    explicit Postings(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override {
        return IndexExtension::Postings;
    }
    bool exact() const override { return true; }
    dstd::Optional<ChunkSet> may_match(const duql::detail::QueryNode& leaf,
                                       const ChunkSet&) override {
        const auto* cmp = std::get_if<duql::detail::CompareNode>(&leaf.data);
        const auto* in = std::get_if<duql::detail::InNode>(&leaf.data);
        if (cmp ? cmp->op != duql::CompareOp::Eq : !in) return dstd::nullopt;
        const dstd::String path(cmp ? cmp->field.path : in->field.path);
        LeafHolder h;
        if (!h.assign(leaf)) return dstd::nullopt;
        const auto q = PostingsSummary(0).compile_postings(
            h.leaf, probe_type(d_.may_hold_strings(path)));
        if (!q) return dstd::nullopt;
        ChunkSet out;
        for (const std::uint64_t hash : q->hashes()) {
            const auto contains = d_.posting_contains(path, hash);
            // An equality leaf needs every probe answered; an `in` leaf
            // skips a value the postings cannot answer.
            if (!contains) {
                if (cmp) return dstd::nullopt;
                continue;
            }
            if (!*contains) continue;
            const auto& chunks = d_.posting_chunks(path, hash);
            if (chunks.empty() && cmp) return dstd::nullopt;
            out.insert(chunks.begin(), chunks.end());
        }
        if (in && out.empty()) return dstd::nullopt;
        return out;
    }

   private:
    FileIndexData& d_;
};

/// counts kind: value membership, and all-match proofs from the counts.
class Counts final : public Condition {
   public:
    explicit Counts(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override { return IndexExtension::Counts; }

    dstd::Optional<ChunkSet> may_match(const duql::detail::QueryNode& leaf,
                                       const ChunkSet& candidates) override {
        const auto* field = leaf_field(leaf);
        if (!field) return dstd::nullopt;
        LeafHolder h;
        if (!h.assign(leaf)) return dstd::nullopt;
        const auto q = CountsSummary(0, 0).compile_counts(h.leaf);
        if (!q) return dstd::nullopt;
        const auto& counts = d_.counts(dstd::String(field->path));
        ChunkSet out;
        for (auto ckpt : candidates) {
            const auto* vc = values(counts, ckpt);
            if (vc ? q->may_match_counts(*vc) : q->may_match_missing())
                out.insert(ckpt);
        }
        return out;
    }

    dstd::Optional<ChunkSet> all_match(
        const duql::detail::QueryNode& leaf) override {
        const auto* field = leaf_field(leaf);
        if (!field) return dstd::nullopt;
        LeafHolder h;
        if (!h.assign(leaf)) return dstd::nullopt;
        const auto q = CountsSummary(0, 0).compile_counts(h.leaf);
        if (!q) return dstd::nullopt;
        ChunkSet out;
        for (const auto& [ckpt, g] : d_.counts(dstd::String(field->path))) {
            const auto* vc = g.get();
            if (!vc || !covers_every_line(d_, ckpt, g.observed, counted(g)))
                continue;
            if (q->all_match_counts(*vc)) out.insert(ckpt);
        }
        return out;
    }

   private:
    static const dstd::StringViewMap<std::uint64_t>* values(
        const FileIndexData::Counts& counts, std::uint64_t ckpt) {
        auto it = counts.find(ckpt);
        return it == counts.end() ? nullptr : it->second.get();
    }

    FileIndexData& d_;
};

/// zonemap kind: value ranges, the timestamp histogram, and all-match
/// proofs for ranges.
class Zonemap final : public Condition {
   public:
    explicit Zonemap(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override {
        return IndexExtension::Zonemap;
    }

    dstd::Optional<ChunkSet> may_match(const duql::detail::QueryNode& leaf,
                                       const ChunkSet& candidates) override {
        if (const auto path = exists_path(leaf))
            return exists_chunks(*path, candidates);
        if (const auto* in = std::get_if<duql::detail::InNode>(&leaf.data))
            return in_list(*in, leaf, candidates);
        const auto* n = std::get_if<duql::detail::CompareNode>(&leaf.data);
        if (!n) return dstd::nullopt;
        // Equality is bounded only on the args fields: a fixed field's text
        // may spell a value differently from the query.
        const bool bounds_equality =
            n->op == duql::CompareOp::Eq && !d_.fixed_field(n->field.path);
        if (!is_range_op(n->op) && !bounds_equality) return dstd::nullopt;
        const auto q = compile(leaf);
        if (!q) return dstd::nullopt;
        const auto& zones = d_.zones(dstd::String(n->field.path));
        dstd::Optional<std::uint64_t> ts_val;
        if (!d_.by_path && n->field.path == "ts" && is_range_op(n->op))
            ts_val = timestamp_bound(d_, literal_to_string(n->value), *n);
        ChunkSet out;
        for (auto ckpt : candidates) {
            auto it = zones.find(ckpt);
            if (it == zones.end()) {
                out.insert(ckpt);
                continue;
            }
            const auto& z = it->second;
            if (!q->may_match_zone(z)) continue;
            if (ts_val && !histogram_has_events(z, n->op, *ts_val)) continue;
            out.insert(ckpt);
        }
        return out;
    }

    dstd::Optional<ChunkSet> all_match(
        const duql::detail::QueryNode& leaf) override {
        const auto* n = std::get_if<duql::detail::CompareNode>(&leaf.data);
        if (!n || !is_range_op(n->op)) return dstd::nullopt;
        const auto q = compile(leaf);
        if (!q) return dstd::nullopt;
        ChunkSet out;
        for (const auto& [ckpt, z] : d_.zones(dstd::String(n->field.path)))
            if (covers_every_line(d_, ckpt, z.observed, z.present) &&
                q->all_match_zone(z))
                out.insert(ckpt);
        return out;
    }

   private:
    // The zone query of a range, equality or `in` leaf; null when the zones
    // have no evidence for it.
    dstd::UniquePtr<trace::index::extensions::ZonemapQuery> compile(
        const duql::detail::QueryNode& leaf) {
        LeafHolder h;
        if (!h.assign(leaf)) return nullptr;
        return ZonemapSummary(0).compile_zone(h.leaf);
    }

    // A chunk may hold a listed value when one lies in its range; bounded,
    // as equality is, only on the args fields.
    dstd::Optional<ChunkSet> in_list(const duql::detail::InNode& in,
                                     const duql::detail::QueryNode& leaf,
                                     const ChunkSet& candidates) {
        if (d_.fixed_field(in.field.path)) return dstd::nullopt;
        const auto q = compile(leaf);
        if (!q) return dstd::nullopt;
        const auto& zones = d_.zones(dstd::String(in.field.path));
        ChunkSet out;
        for (auto ckpt : candidates) {
            auto it = zones.find(ckpt);
            if (it == zones.end() ? q->may_match_missing()
                                  : q->may_match_zone(it->second))
                out.insert(ckpt);
        }
        return out;
    }

    // `exists(path)` is false on a chunk whose zone records no value only
    // when no record of the file holds the path as null, an object or an
    // array, which the zone does not see.
    dstd::Optional<ChunkSet> exists_chunks(dstd::StringView path,
                                           const ChunkSet& candidates) {
        if (!absence_is_missing(path)) return dstd::nullopt;
        const auto& zones = d_.zones(path);
        ChunkSet out;
        for (auto ckpt : candidates) {
            auto it = zones.find(ckpt);
            if (it == zones.end() || !absent(it->second)) out.insert(ckpt);
        }
        return out;
    }

    bool absence_is_missing(dstd::StringView path) {
        const auto& name = d_.evidence_name(path);
        if (!name || d_.fixed_field(*name)) return false;
        const dstd::String leaf = d_.by_path ? *name : "args." + *name;
        constexpr auto NULL_BIT = 1U << static_cast<unsigned>(
                                      trace::index::store::PathType::NullValue);
        bool scalar = false;
        for (const auto& [p, stat] : d_.catalog()) {
            if (under(p, leaf)) return false;
            if (dstd::StringView(p) != leaf) continue;
            scalar = (stat.seen & NULL_BIT) == 0 &&
                     stat.type != trace::index::store::PathType::NullValue &&
                     stat.type < trace::index::store::PathType::Mixed;
        }
        return scalar;
    }

    FileIndexData& d_;
};

/// bloom kind, chunk level: equality and `in` probes.
class ChunkBloom final : public Condition {
   public:
    explicit ChunkBloom(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override { return IndexExtension::Bloom; }
    dstd::Optional<ChunkSet> may_match(const duql::detail::QueryNode& leaf,
                                       const ChunkSet& candidates) override {
        const auto* cmp = std::get_if<duql::detail::CompareNode>(&leaf.data);
        const auto* in = std::get_if<duql::detail::InNode>(&leaf.data);
        if (cmp ? cmp->op != duql::CompareOp::Eq : !in) return dstd::nullopt;
        const dstd::String path(cmp ? cmp->field.path : in->field.path);
        LeafHolder h;
        if (!h.assign(leaf)) return dstd::nullopt;
        const auto q = BloomSummary({}, 0).compile_bloom(
            h.leaf, probe_type(d_.may_hold_strings(path)));
        if (!q) return dstd::nullopt;
        const auto& blooms = d_.blooms(path);
        ChunkSet out;
        for (auto ckpt : candidates) {
            auto it = blooms.find(ckpt);
            if (it == blooms.end() ? q->may_match_missing()
                                   : q->may_match_filter(it->second))
                out.insert(ckpt);
        }
        return out;
    }

   private:
    FileIndexData& d_;
};

// A leaf the conditions test, `exists(p)` included; an any() leaf left
// unexpanded (a very wide array) proves nothing, since its path's evidence
// describes the whole value.
bool is_leaf(const duql::detail::QueryNode& node) {
    if (exists_path(node)) return true;
    const auto* field = leaf_field(node);
    return (std::holds_alternative<duql::detail::CompareNode>(node.data) ||
            std::holds_alternative<duql::detail::InNode>(node.data) ||
            std::holds_alternative<duql::detail::NotInNode>(node.data)) &&
           !(field && field->any) && !too_wide(node);
}

ChunkSet eval_leaf(const duql::detail::QueryNode& leaf, const Conditions& cs,
                   const ChunkSet& universe) {
    dstd::Optional<ChunkSet> acc;
    for (const auto& c : cs) {
        auto s = c->may_match(leaf, acc ? *acc : universe);
        if (!s) continue;
        acc = acc ? intersect(*acc, *s) : std::move(*s);
        if (acc->empty() || c->exact()) break;
    }
    return acc ? std::move(*acc) : universe;
}

ChunkSet eval_all_match(const duql::detail::QueryNode& node,
                        const Conditions& cs, const ChunkSet& universe);

ChunkSet eval_node(const duql::detail::QueryNode& node, const Conditions& cs,
                   const ChunkSet& universe) {
    if (is_leaf(node)) return eval_leaf(node, cs, universe);
    if (const auto* n = std::get_if<duql::detail::AndNode>(&node.data))
        return intersect(eval_node(*n->left, cs, universe),
                         eval_node(*n->right, cs, universe));
    if (const auto* n = std::get_if<duql::detail::OrNode>(&node.data)) {
        auto left = eval_node(*n->left, cs, universe);
        auto right = eval_node(*n->right, cs, universe);
        left.insert(right.begin(), right.end());
        return left;
    }
    if (const auto* n = std::get_if<duql::detail::NotNode>(&node.data))
        // A chunk may hold an event matching `not p` unless every event in
        // it matches `p`; complementing the may-match set would drop chunks
        // holding events on both sides of `p`.
        return difference(universe, eval_all_match(*n->operand, cs, universe));
    return universe;  // a pattern match proves nothing
}

ChunkSet eval_all_match(const duql::detail::QueryNode& node,
                        const Conditions& cs, const ChunkSet& universe) {
    if (is_leaf(node)) {
        ChunkSet out;
        for (const auto& c : cs)
            if (auto s = c->all_match(node)) out.insert(s->begin(), s->end());
        return out;
    }
    if (const auto* n = std::get_if<duql::detail::AndNode>(&node.data))
        return intersect(eval_all_match(*n->left, cs, universe),
                         eval_all_match(*n->right, cs, universe));
    if (const auto* n = std::get_if<duql::detail::OrNode>(&node.data)) {
        // An under-approximation: a chunk where the two sides cover every
        // event between them is left out.
        auto left = eval_all_match(*n->left, cs, universe);
        auto right = eval_all_match(*n->right, cs, universe);
        left.insert(right.begin(), right.end());
        return left;
    }
    if (const auto* n = std::get_if<duql::detail::NotNode>(&node.data))
        return difference(universe, eval_node(*n->operand, cs, universe));
    return {};
}

}  // namespace

bool too_wide(const duql::detail::QueryNode& node) {
    if (const auto* n = std::get_if<duql::detail::InNode>(&node.data))
        return n->values.elements.size() > SEMI_JOIN_CAP;
    if (const auto* n = std::get_if<duql::detail::NotInNode>(&node.data))
        return n->values.elements.size() > SEMI_JOIN_CAP;
    return false;
}

namespace {

/// An index extension: it compiles each leaf and judges each chunk's and the
/// file's payload.
class PluginCondition final : public Condition {
   public:
    PluginCondition(FileIndexData& d,
                    trace::index::extensions::ExtensionPtr ext)
        : d_(d), ext_(std::move(ext)) {}

    IndexExtension extension() const override { return IndexExtension::Plugin; }
    dstd::String name() const override { return dstd::String(ext_->name()); }

    bool file_may_match(const duql::detail::QueryNode& root) override {
        if (!ext_->has_file_check()) return true;
        if (!file_loaded_) {
            auto loaded = trace::index::plugin_file(*d_.snapshot, *d_.file,
                                                    dstd::String(ext_->name()));
            if (loaded)
                file_ = std::move(*loaded);
            else
                d_.fail(std::move(loaded).error());
            file_loaded_ = true;
        }
        return !file_ || may_match_file(root);
    }

    dstd::Optional<ChunkSet> may_match(const duql::detail::QueryNode& leaf,
                                       const ChunkSet& candidates) override {
        const auto* q = compile(leaf);
        if (!q) return dstd::nullopt;
        if (!granules_) {
            granules_.emplace();
            auto listed = trace::index::plugin_granules(
                *d_.snapshot, *d_.file, dstd::String(ext_->name()));
            if (!listed)
                d_.fail(std::move(listed).error());
            else if (*listed)
                for (auto& [g, bytes] : **listed)
                    granules_->emplace(g, std::move(bytes));
        }
        ChunkSet out;
        for (auto c : candidates) {
            auto it = granules_->find(c);
            if (it == granules_->end() ||
                q->may_match(
                    dstd::StringView(it->second.data(), it->second.size())))
                out.insert(c);
        }
        return out;
    }

   private:
    bool may_match_file(const duql::detail::QueryNode& node) {
        if (const auto* a = std::get_if<duql::detail::AndNode>(&node.data))
            return may_match_file(*a->left) && may_match_file(*a->right);
        if (const auto* o = std::get_if<duql::detail::OrNode>(&node.data))
            return may_match_file(*o->left) || may_match_file(*o->right);
        if (std::holds_alternative<duql::detail::NotNode>(node.data))
            return true;
        const auto* q = compile(node);
        return !q || q->file_may_match(
                         dstd::StringView(file_->data(), file_->size()));
    }

    // NULL when the leaf cannot be passed or the extension has no evidence.
    const trace::index::extensions::ExtensionQuery* compile(
        const duql::detail::QueryNode& leaf) {
        auto [it, inserted] = compiled_.try_emplace(&leaf);
        if (inserted) it->second = compile_leaf(leaf);
        return it->second.get();
    }

    dstd::UniquePtr<trace::index::extensions::ExtensionQuery> compile_leaf(
        const duql::detail::QueryNode& leaf) {
        // The plugin ABI carries no uint64 above int64.
        LeafHolder h;
        if (!h.assign(leaf) || h.has_uint64_above_int64()) return nullptr;
        h.leaf.type = d_.path_type(h.leaf.path);
        auto compiled = ext_->compile(h.leaf);
        if (!compiled) {
            DFTRACER_UTILS_LOG_WARN(
                "Index extension '{}' compile failed with {}; the "
                "leaf prunes nothing",
                ext_->name(), compiled.error().message());
            return nullptr;
        }
        return std::move(*compiled);
    }

    FileIndexData& d_;
    trace::index::extensions::ExtensionPtr ext_;
    dstd::HashMap<const duql::detail::QueryNode*,
                  dstd::UniquePtr<trace::index::extensions::ExtensionQuery>>
        compiled_;
    dstd::Optional<dstd::HashMap<std::uint64_t, dstd::String>> granules_;
    dstd::Optional<dstd::String> file_;
    bool file_loaded_ = false;
};

}  // namespace

Conditions make_query_conditions(FileIndexData& data) {
    Conditions cs;
    cs.push_back(dstd::make_unique<FileBloom>(data));
    cs.push_back(dstd::make_unique<CatalogPresence>(data));
    cs.push_back(dstd::make_unique<Postings>(data));
    cs.push_back(dstd::make_unique<Counts>(data));
    cs.push_back(dstd::make_unique<Zonemap>(data));
    cs.push_back(dstd::make_unique<ChunkBloom>(data));
    auto registered = trace::index::extensions::registered_extensions();
    if (registered.empty()) return cs;
    const auto copies = trace::index::plugins(*data.snapshot, *data.file);
    if (!copies) {
        data.fail(copies.error());
        return cs;
    }
    for (auto& ext : registered) {
        for (const auto& copy : *copies) {
            if (dstd::StringView(copy.name) != ext->name()) continue;
            const store::ExtensionState state{copy.version, copy.params_hash,
                                              copy.ready};
            if (ext->current(state))
                cs.push_back(
                    dstd::make_unique<PluginCondition>(data, std::move(ext)));
            break;
        }
    }
    return cs;
}

namespace {

// The path a leaf judges: the field of a comparison, `in` or match, or the
// argument of `exists`.
dstd::Optional<dstd::String> judged_path(const duql::detail::QueryNode& leaf) {
    if (const auto path = exists_path(leaf)) return *path;
    if (const auto* field = leaf_field(leaf))
        return dstd::String(field->path.data(), field->path.size());
    return dstd::nullopt;
}

void collect_leaf_paths(const duql::detail::QueryNode& node,
                        dstd::Vector<dstd::String>& out) {
    if (is_leaf(node)) {
        if (auto path = judged_path(node))
            if (std::find(out.begin(), out.end(), *path) == out.end())
                out.push_back(std::move(*path));
        return;
    }
    if (const auto* a = std::get_if<duql::detail::AndNode>(&node.data)) {
        collect_leaf_paths(*a->left, out);
        collect_leaf_paths(*a->right, out);
    } else if (const auto* o = std::get_if<duql::detail::OrNode>(&node.data)) {
        collect_leaf_paths(*o->left, out);
        collect_leaf_paths(*o->right, out);
    } else if (const auto* t = std::get_if<duql::detail::NotNode>(&node.data)) {
        collect_leaf_paths(*t->operand, out);
    }
}

// `inner`, asked about the leaves on one path only: every other leaf is a
// leaf it has no evidence for.
class PathScoped final : public Condition {
   public:
    PathScoped(dstd::UniquePtr<Condition> inner, dstd::String path)
        : inner_(std::move(inner)), path_(std::move(path)) {}

    IndexExtension extension() const override { return inner_->extension(); }
    dstd::Optional<ChunkSet> may_match(const duql::detail::QueryNode& leaf,
                                       const ChunkSet& candidates) override {
        if (!on_path(leaf)) return dstd::nullopt;
        return inner_->may_match(leaf, candidates);
    }
    dstd::Optional<ChunkSet> all_match(
        const duql::detail::QueryNode& leaf) override {
        if (!on_path(leaf)) return dstd::nullopt;
        return inner_->all_match(leaf);
    }

   private:
    bool on_path(const duql::detail::QueryNode& leaf) const {
        const auto path = judged_path(leaf);
        return path && *path == path_;
    }

    dstd::UniquePtr<Condition> inner_;
    dstd::String path_;
};

}  // namespace

dstd::Vector<SummarySkip> summary_skips(const duql::detail::QueryNode& root,
                                        FileIndexData& data,
                                        const ChunkSet& universe) {
    dstd::Vector<dstd::String> paths;
    collect_leaf_paths(root, paths);
    // The summary kinds of the query's conditions: the built-in ones, then
    // each registered index extension by name.
    struct Kind {
        IndexExtension ext;
        dstd::String name;
    };
    dstd::Vector<Kind> kinds;
    for (auto ext : {IndexExtension::Zonemap, IndexExtension::Bloom,
                     IndexExtension::Counts, IndexExtension::Postings})
        kinds.push_back(
            {ext, dstd::String(trace::index::store::extension_name(ext))});
    for (const auto& c : make_query_conditions(data))
        if (c && c->extension() == IndexExtension::Plugin) {
            const dstd::String name = c->name();
            if (std::none_of(kinds.begin(), kinds.end(), [&](const Kind& k) {
                    return k.ext == IndexExtension::Plugin && k.name == name;
                }))
                kinds.push_back({IndexExtension::Plugin, name});
        }
    dstd::Vector<SummarySkip> out;
    for (const dstd::String& path : paths) {
        for (const Kind& kind : kinds) {
            Conditions scoped;
            for (auto& c : make_query_conditions(data))
                if (c && c->extension() == kind.ext &&
                    (kind.ext != IndexExtension::Plugin ||
                     c->name() == kind.name))
                    scoped.push_back(
                        dstd::make_unique<PathScoped>(std::move(c), path));
            if (scoped.empty()) continue;
            const ChunkSet kept = evaluate(root, scoped, universe);
            if (kept.size() == universe.size()) continue;
            out.push_back({path, kind.name, universe.size() - kept.size(),
                           universe.size()});
        }
    }
    return out;
}

bool file_may_match(const duql::detail::QueryNode& root,
                    const Conditions& conditions) {
    for (const auto& c : conditions)
        if (!c->file_may_match(root)) return false;
    return true;
}

ChunkSet evaluate(const duql::detail::QueryNode& root,
                  const Conditions& conditions, const ChunkSet& universe) {
    return eval_node(root, conditions, universe);
}

Conditions make_metadata_conditions(
    const dstd::HashMap<std::uint64_t, trace::index::store::ChunkMetadata>&
        metadata) {
    Conditions out;
    out.push_back(dstd::make_unique<MetadataRecords>(metadata));
    return out;
}

}  // namespace dftracer::utils::trace::index::plan
