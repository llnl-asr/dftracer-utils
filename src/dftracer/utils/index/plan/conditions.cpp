#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/duql/numbers.h>
#include <dftracer/utils/duql/term.h>
#include <dftracer/utils/index/extensions/bloom_filter.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/index/plan/file_index_data.h>
#include <dftracer/utils/index/store/index_database.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dftracer::utils::index::plan {

namespace duql_ns = duql;
using index::extensions::kinds::Zone;

namespace {

// A literal as the index keys it; a bool is 1 or 0, as the build records it.
std::string literal_to_string(const duql_ns::LiteralNode& lit) {
    return std::visit(
        [](auto&& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "1" : "0";
            else if constexpr (std::is_same_v<T, int64_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, uint64_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, double>)
                return index::extensions::canonical_number_text(v);
            else
                return {};
        },
        lit.value);
}

bool is_numeric_type(const std::string& vtype) {
    return vtype == "uint" || vtype == "int" || vtype == "double";
}

int compare_values(const std::string& a, const std::string& b,
                   const std::string& vtype) {
    // Exact across integer and double text, so `x < 1.5` against an int zone
    // is not read as `x < 1`.
    if (is_numeric_type(vtype)) {
        const auto na = duql_ns::parse_number(a);
        const auto nb = duql_ns::parse_number(b);
        if (na && nb)
            if (const auto c = duql_ns::compare_numbers(*na, *nb)) return *c;
    }
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

bool is_range_op(duql_ns::CompareOp op) {
    return op == duql_ns::CompareOp::GT || op == duql_ns::CompareOp::GE ||
           op == duql_ns::CompareOp::LT || op == duql_ns::CompareOp::LE;
}

// Bounds of a zone that can be trusted: both present and, for numbers,
// min <= max. Corrupt stats would silently drop matching events.
bool trusted_bounds(const Zone& z, bool need_both) {
    if (need_both ? (z.min.empty() || z.max.empty())
                  : (z.min.empty() && z.max.empty()))
        return false;
    if (is_numeric_type(z.value_type) && !z.min.empty() && !z.max.empty()) {
        try {
            if (std::stod(z.min) > std::stod(z.max)) return false;
        } catch (...) {
            return false;
        }
    }
    return true;
}

bool zone_may_match(const Zone& z, duql_ns::CompareOp op,
                    const std::string& val) {
    if (!trusted_bounds(z, false)) return true;
    switch (op) {
        case duql_ns::CompareOp::GT:
            return compare_values(z.max, val, z.value_type) > 0;
        case duql_ns::CompareOp::GE:
            return compare_values(z.max, val, z.value_type) >= 0;
        case duql_ns::CompareOp::LT:
            return compare_values(z.min, val, z.value_type) < 0;
        case duql_ns::CompareOp::LE:
            return compare_values(z.min, val, z.value_type) <= 0;
        default:
            return true;
    }
}

// The dual of zone_may_match: every value in the zone satisfies `op val`.
bool zone_all_match(const Zone& z, duql_ns::CompareOp op,
                    const std::string& val) {
    if (!trusted_bounds(z, true)) return false;
    switch (op) {
        case duql_ns::CompareOp::GT:
            return compare_values(z.min, val, z.value_type) > 0;
        case duql_ns::CompareOp::GE:
            return compare_values(z.min, val, z.value_type) >= 0;
        case duql_ns::CompareOp::LT:
            return compare_values(z.max, val, z.value_type) < 0;
        case duql_ns::CompareOp::LE:
            return compare_values(z.max, val, z.value_type) <= 0;
        default:
            return false;
    }
}

// Zero records in the queried range means the chunk can be skipped.
bool histogram_has_events(const Zone& z, duql_ns::CompareOp op,
                          std::uint64_t ts_val) {
    if (!z.histogram || z.histogram->empty()) return true;
    const auto& hist = *z.histogram;
    switch (op) {
        case duql_ns::CompareOp::GT:
            return hist.count_in_range(ts_val + 1, UINT64_MAX) > 0;
        case duql_ns::CompareOp::GE:
            return hist.count_in_range(ts_val, UINT64_MAX) > 0;
        case duql_ns::CompareOp::LT:
            return hist.count_in_range(0, ts_val) > 0;
        case duql_ns::CompareOp::LE:
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
                       std::optional<std::uint64_t> present) {
    const auto ln = d.chunk_lines.find(ckpt);
    return observed != 0 && ln != d.chunk_lines.end() &&
           ln->second == observed && present && *present == observed;
}

std::optional<std::uint64_t> counted(const GranuleCounts& g) {
    const auto* vc = g.get();
    if (!vc) return std::nullopt;
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
void collect_equality_paths(const duql_ns::QueryNode& node,
                            StringViewSet& out) {
    std::visit(
        [&out](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql_ns::CompareNode>) {
                if (n.op == duql_ns::CompareOp::EQ) out.insert(n.field.path);
            } else if constexpr (std::is_same_v<T, duql_ns::InNode>) {
                out.insert(n.field.path);
            } else if constexpr (std::is_same_v<T, duql_ns::AndNode> ||
                                 std::is_same_v<T, duql_ns::OrNode>) {
                collect_equality_paths(*n.left, out);
                collect_equality_paths(*n.right, out);
            } else if constexpr (std::is_same_v<T, duql_ns::NotNode>) {
                collect_equality_paths(*n.operand, out);
            }
        },
        node.data);
}

// Whether a record carrying the fields `paths` may resolve `path` the way the
// evaluator does: as written, under "args.", or as an object or dotted flat
// key along the way.
bool path_may_exist(const std::vector<std::string>& paths,
                    const std::string& path) {
    auto near = [&](const std::string& p) {
        for (const auto& s : paths)
            if (s == p ||
                (s.size() > p.size() && s.starts_with(p) &&
                 s[p.size()] == '.') ||
                (p.size() > s.size() && p.starts_with(s) && p[s.size()] == '.'))
                return true;
        return false;
    };
    return near(path) || (!path.starts_with("args.") && near("args." + path));
}

// Whether some record name satisfies `leaf`, a leaf on `name`; a pattern
// leaf counts as satisfiable.
bool name_may_match(const std::vector<std::string>& names,
                    const duql_ns::QueryNode& leaf) {
    auto text = [](const duql_ns::LiteralNode& lit) -> const std::string* {
        return std::get_if<std::string>(&lit.value);
    };
    return std::visit(
        [&](auto&& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql_ns::CompareNode>) {
                const std::string* v = text(n.value);
                if (!v) return false;
                for (const auto& name : names) {
                    const int c = name.compare(*v);
                    switch (n.op) {
                        case duql_ns::CompareOp::EQ:
                            if (c == 0) return true;
                            break;
                        case duql_ns::CompareOp::NE:
                            if (c != 0) return true;
                            break;
                        case duql_ns::CompareOp::GT:
                            if (c > 0) return true;
                            break;
                        case duql_ns::CompareOp::LT:
                            if (c < 0) return true;
                            break;
                        case duql_ns::CompareOp::GE:
                            if (c >= 0) return true;
                            break;
                        case duql_ns::CompareOp::LE:
                            if (c <= 0) return true;
                            break;
                    }
                }
                return false;
            } else if constexpr (std::is_same_v<T, duql_ns::InNode> ||
                                 std::is_same_v<T, duql_ns::NotInNode>) {
                constexpr bool in = std::is_same_v<T, duql_ns::InNode>;
                for (const auto& name : names) {
                    bool listed = false;
                    for (const auto& e : n.values.elements) {
                        const std::string* v = text(e);
                        if (v && *v == name) listed = true;
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

const duql_ns::FieldNode* leaf_field(const duql_ns::QueryNode& leaf) {
    return std::visit(
        [](auto&& n) -> const duql_ns::FieldNode* {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql_ns::CompareNode> ||
                          std::is_same_v<T, duql_ns::InNode> ||
                          std::is_same_v<T, duql_ns::NotInNode> ||
                          std::is_same_v<T, duql_ns::MatchNode>)
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
    explicit MetadataRecords(const ankerl::unordered_dense::map<
                             std::uint64_t, index::store::ChunkMetadata>& meta)
        : meta_(meta) {}

    IndexExtension extension() const override {
        return IndexExtension::METADATA;
    }

    std::optional<ChunkSet> may_match(const duql_ns::QueryNode& leaf,
                                      const ChunkSet& candidates) override {
        const duql_ns::FieldNode* field = leaf_field(leaf);
        if (!field) return std::nullopt;
        ChunkSet out;
        for (auto c : candidates) {
            const auto it = meta_.find(c);
            if (it == meta_.end() || may_hold(it->second, leaf, field->path))
                out.insert(c);
        }
        return out;
    }

   private:
    static bool may_hold(const index::store::ChunkMetadata& m,
                         const duql_ns::QueryNode& leaf,
                         const std::string& path) {
        if (m.records == 0) return false;
        if (m.paths && !path_may_exist(*m.paths, path)) return false;
        // A record without a top-level name resolves `name` under args.
        if (path == "name" && m.names &&
            !std::binary_search(m.names->begin(), m.names->end(), ""))
            return name_may_match(*m.names, leaf);
        return true;
    }

    const ankerl::unordered_dense::map<std::uint64_t,
                                       index::store::ChunkMetadata>& meta_;
};

/// bloom kind, file level: rules the whole file out on equality leaves.
class FileBloom final : public Condition {
   public:
    explicit FileBloom(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override { return IndexExtension::BLOOM; }
    bool file_may_match(const duql_ns::QueryNode& root) override {
        StringViewSet paths;
        collect_equality_paths(root, paths);
        if (paths.empty()) return true;
        return may_match_file(root);
    }
    std::optional<ChunkSet> may_match(const duql_ns::QueryNode&,
                                      const ChunkSet&) override {
        return std::nullopt;
    }

   private:
    bool may_contain(const std::string& path, const std::string& val) {
        const auto* bf = d_.file_bloom(path);
        return !bf || bf->possibly_contains(val);
    }

    bool may_match_file(const duql_ns::QueryNode& node) {
        return std::visit(
            [this](auto&& n) -> bool {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, duql_ns::CompareNode>) {
                    if (n.op != duql_ns::CompareOp::EQ) return true;
                    return may_contain(n.field.path,
                                       literal_to_string(n.value));
                } else if constexpr (std::is_same_v<T, duql_ns::InNode>) {
                    if (n.values.elements.size() > SEMI_JOIN_CAP) return true;
                    for (const auto& elem : n.values.elements)
                        if (may_contain(n.field.path, literal_to_string(elem)))
                            return true;
                    return false;
                } else if constexpr (std::is_same_v<T, duql_ns::AndNode>) {
                    return may_match_file(*n.left) && may_match_file(*n.right);
                } else if constexpr (std::is_same_v<T, duql_ns::OrNode>) {
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
std::optional<std::string> exists_path(const duql_ns::QueryNode& node) {
    const auto* leaf = std::get_if<duql_ns::ExprLeaf>(&node.data);
    if (!leaf) return std::nullopt;
    const auto* call = std::get_if<duql_ns::TCall>(&leaf->term->node);
    if (!call || call->fn != duql_ns::Fn::EXISTS) return std::nullopt;
    const auto& base = std::get<duql_ns::TField>(call->args[0]->node).base;
    std::string out;
    for (const char c : base) {
        if (c == '[')
            out += '.';
        else if (c != ']')
            out += c;
    }
    return out;
}

/// core.catalog, file level: `exists(p)` rules the file out when no catalog
/// path is `p` or lies under it.
class CatalogPresence final : public Condition {
   public:
    explicit CatalogPresence(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override {
        return IndexExtension::CATALOG;
    }
    bool file_may_match(const duql_ns::QueryNode& root) override {
        return may_match_file(root);
    }
    std::optional<ChunkSet> may_match(const duql_ns::QueryNode&,
                                      const ChunkSet&) override {
        return std::nullopt;
    }

   private:
    bool may_match_file(const duql_ns::QueryNode& node) {
        if (const auto* a = std::get_if<duql_ns::AndNode>(&node.data))
            return may_match_file(*a->left) && may_match_file(*a->right);
        if (const auto* o = std::get_if<duql_ns::OrNode>(&node.data))
            return may_match_file(*o->left) || may_match_file(*o->right);
        const auto path = exists_path(node);
        return !path || may_exist(*path);
    }

    // The catalog leaves out the fields every record of the trace schema
    // carries.
    static bool unrecorded(std::string_view path) {
        const std::string_view head = path.substr(0, path.find('.'));
        return head == "pid" || head == "tid" || head == "ts" ||
               head == "dur" || head == "ph" || head == "id";
    }

    bool may_exist(const std::string& path) {
        if (!d_.by_path && unrecorded(path)) return true;
        if (!paths_) {
            paths_.emplace();
            if (d_.db->extension_current(d_.fid, IndexExtension::CATALOG))
                for (auto& [p, stat] : d_.db->catalog(d_.fid))
                    paths_->push_back(std::move(p));
        }
        if (paths_->empty()) return true;
        return path_may_exist(*paths_, path);
    }

    FileIndexData& d_;
    std::optional<std::vector<std::string>> paths_;
};

/// postings kind: exact chunks for equality and `in`.
class Postings final : public Condition {
   public:
    explicit Postings(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override {
        return IndexExtension::POSTINGS;
    }
    bool exact() const override { return true; }
    std::optional<ChunkSet> may_match(const duql_ns::QueryNode& leaf,
                                      const ChunkSet&) override {
        if (const auto* n = std::get_if<duql_ns::CompareNode>(&leaf.data)) {
            if (n->op != duql_ns::CompareOp::EQ) return std::nullopt;
            const auto val = literal_to_string(n->value);
            const auto contains = d_.posting_contains(n->field.path, val);
            if (!contains) return std::nullopt;
            if (!*contains) return ChunkSet{};
            const auto& chunks = d_.posting_chunks(n->field.path, val);
            if (chunks.empty()) return std::nullopt;
            return chunks;
        }
        if (const auto* n = std::get_if<duql_ns::InNode>(&leaf.data)) {
            ChunkSet out;
            for (const auto& elem : n->values.elements) {
                const auto val = literal_to_string(elem);
                const auto contains = d_.posting_contains(n->field.path, val);
                if (!contains || !*contains) continue;
                const auto& chunks = d_.posting_chunks(n->field.path, val);
                out.insert(chunks.begin(), chunks.end());
            }
            if (out.empty()) return std::nullopt;
            return out;
        }
        return std::nullopt;
    }

   private:
    FileIndexData& d_;
};

/// counts kind: value membership, and all-match proofs from the counts.
class Counts final : public Condition {
   public:
    explicit Counts(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override { return IndexExtension::COUNTS; }

    std::optional<ChunkSet> may_match(const duql_ns::QueryNode& leaf,
                                      const ChunkSet& candidates) override {
        if (const auto* n = std::get_if<duql_ns::CompareNode>(&leaf.data)) {
            if (n->op != duql_ns::CompareOp::EQ &&
                n->op != duql_ns::CompareOp::NE)
                return std::nullopt;
            const auto& counts = d_.counts(n->field.path);
            const auto val = literal_to_string(n->value);
            ChunkSet out;
            for (auto ckpt : candidates) {
                const auto* vc = values(counts, ckpt);
                if (!vc) {
                    out.insert(ckpt);
                } else if (n->op == duql_ns::CompareOp::EQ) {
                    if (vc->count(val) > 0) out.insert(ckpt);
                } else if (!(vc->size() == 1 && vc->count(val) > 0)) {
                    // A chunk whose only value is `val` holds no event != val.
                    out.insert(ckpt);
                }
            }
            return out;
        }
        if (const auto* n = std::get_if<duql_ns::InNode>(&leaf.data)) {
            const auto& counts = d_.counts(n->field.path);
            ChunkSet out;
            for (auto ckpt : candidates) {
                const auto* vc = values(counts, ckpt);
                if (!vc) {
                    if (!n->values.elements.empty()) out.insert(ckpt);
                    continue;
                }
                for (const auto& elem : n->values.elements) {
                    if (vc->count(literal_to_string(elem)) > 0) {
                        out.insert(ckpt);
                        break;
                    }
                }
            }
            return out;
        }
        if (const auto* n = std::get_if<duql_ns::NotInNode>(&leaf.data)) {
            const auto& counts = d_.counts(n->field.path);
            ChunkSet out;
            for (auto ckpt : candidates) {
                const auto* vc = values(counts, ckpt);
                if (!vc || !all_listed(*vc, n->values)) out.insert(ckpt);
            }
            return out;
        }
        return std::nullopt;
    }

    std::optional<ChunkSet> all_match(const duql_ns::QueryNode& leaf) override {
        if (const auto* n = std::get_if<duql_ns::CompareNode>(&leaf.data)) {
            if (n->op != duql_ns::CompareOp::EQ &&
                n->op != duql_ns::CompareOp::NE)
                return std::nullopt;
            const auto val = literal_to_string(n->value);
            return proven(n->field.path, [&](const auto& vc) {
                return n->op == duql_ns::CompareOp::EQ
                           ? vc.size() == 1 && vc.count(val) > 0
                           : vc.count(val) == 0;
            });
        }
        if (const auto* n = std::get_if<duql_ns::InNode>(&leaf.data))
            return proven(n->field.path, [&](const auto& vc) {
                return !vc.empty() && all_listed(vc, n->values);
            });
        if (const auto* n = std::get_if<duql_ns::NotInNode>(&leaf.data))
            return proven(n->field.path, [&](const auto& vc) {
                return !vc.empty() && none_listed(vc, n->values);
            });
        return std::nullopt;
    }

   private:
    static const StringViewMap<std::uint64_t>* values(
        const FileIndexData::Counts& counts, std::uint64_t ckpt) {
        auto it = counts.find(ckpt);
        return it == counts.end() ? nullptr : it->second.get();
    }

    static bool all_listed(const StringViewMap<std::uint64_t>& vc,
                           const duql_ns::ArrayNode& list) {
        for (const auto& entry : vc) {
            const auto& value = entry.first;
            bool listed = false;
            for (const auto& elem : list.elements)
                if (literal_to_string(elem) == value) {
                    listed = true;
                    break;
                }
            if (!listed) return false;
        }
        return true;
    }

    static bool none_listed(const StringViewMap<std::uint64_t>& vc,
                            const duql_ns::ArrayNode& list) {
        for (const auto& elem : list.elements)
            if (vc.count(literal_to_string(elem)) > 0) return false;
        return true;
    }

    template <typename Pred>
    ChunkSet proven(const std::string& path, Pred pred) {
        ChunkSet out;
        for (const auto& [ckpt, g] : d_.counts(path)) {
            const auto* vc = g.get();
            if (!vc || !covers_every_line(d_, ckpt, g.observed, counted(g)))
                continue;
            if (pred(*vc)) out.insert(ckpt);
        }
        return out;
    }

    FileIndexData& d_;
};

/// zonemap kind: value ranges, the timestamp histogram, and all-match
/// proofs for ranges.
class Zonemap final : public Condition {
   public:
    explicit Zonemap(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override {
        return IndexExtension::ZONEMAP;
    }

    std::optional<ChunkSet> may_match(const duql_ns::QueryNode& leaf,
                                      const ChunkSet& candidates) override {
        if (const auto* in = std::get_if<duql_ns::InNode>(&leaf.data))
            return in_list(*in, candidates);
        const auto* n = std::get_if<duql_ns::CompareNode>(&leaf.data);
        if (!n) return std::nullopt;
        // Equality is bounded only on the args fields: a fixed field's text
        // may spell a value differently from the query.
        const bool bounds_equality =
            n->op == duql_ns::CompareOp::EQ && !fixed_dimension(n->field.path);
        if (!is_range_op(n->op) && !bounds_equality) return std::nullopt;
        const auto& zones = d_.zones(n->field.path);
        const auto val = literal_to_string(n->value);
        std::optional<std::uint64_t> ts_val;
        if (!d_.by_path && n->field.path == "ts" && is_range_op(n->op)) {
            try {
                ts_val = std::stoull(val);
            } catch (...) {
            }
        }
        ChunkSet out;
        for (auto ckpt : candidates) {
            auto it = zones.find(ckpt);
            if (it == zones.end()) {
                out.insert(ckpt);
                continue;
            }
            const auto& z = it->second;
            const bool in_range =
                bounds_equality
                    ? zone_may_match(z, duql_ns::CompareOp::GE, val) &&
                          zone_may_match(z, duql_ns::CompareOp::LE, val)
                    : zone_may_match(z, n->op, val);
            if (!in_range) continue;
            if (ts_val && !histogram_has_events(z, n->op, *ts_val)) continue;
            out.insert(ckpt);
        }
        return out;
    }

    std::optional<ChunkSet> all_match(const duql_ns::QueryNode& leaf) override {
        const auto* n = std::get_if<duql_ns::CompareNode>(&leaf.data);
        if (!n || !is_range_op(n->op)) return std::nullopt;
        const auto val = literal_to_string(n->value);
        ChunkSet out;
        for (const auto& [ckpt, z] : d_.zones(n->field.path))
            if (covers_every_line(d_, ckpt, z.observed, z.present) &&
                zone_all_match(z, n->op, val))
                out.insert(ckpt);
        return out;
    }

   private:
    // A chunk may hold a listed value when one lies in its range; bounded,
    // as equality is, only on the args fields.
    std::optional<ChunkSet> in_list(const duql_ns::InNode& in,
                                    const ChunkSet& candidates) {
        if (fixed_dimension(in.field.path)) return std::nullopt;
        std::vector<std::string> vals;
        vals.reserve(in.values.elements.size());
        for (const auto& elem : in.values.elements)
            vals.push_back(literal_to_string(elem));
        const auto& zones = d_.zones(in.field.path);
        ChunkSet out;
        for (auto ckpt : candidates) {
            auto it = zones.find(ckpt);
            if (it == zones.end()) {
                if (!vals.empty()) out.insert(ckpt);
                continue;
            }
            for (const auto& v : vals)
                if (zone_may_match(it->second, duql_ns::CompareOp::GE, v) &&
                    zone_may_match(it->second, duql_ns::CompareOp::LE, v)) {
                    out.insert(ckpt);
                    break;
                }
        }
        return out;
    }

    bool fixed_dimension(const std::string& dim) const {
        if (d_.by_path) return false;
        return dim == "name" || dim == "cat" || dim == "pid" || dim == "tid" ||
               dim == "pid_tid" || dim == "hhash" || dim == "fhash" ||
               dim == "shash" || dim == "ts" || dim == "dur";
    }

    FileIndexData& d_;
};

/// bloom kind, chunk level: equality and `in` probes.
class ChunkBloom final : public Condition {
   public:
    explicit ChunkBloom(FileIndexData& d) : d_(d) {}
    IndexExtension extension() const override { return IndexExtension::BLOOM; }
    std::optional<ChunkSet> may_match(const duql_ns::QueryNode& leaf,
                                      const ChunkSet& candidates) override {
        std::vector<std::string> vals;
        std::string path;
        if (const auto* n = std::get_if<duql_ns::CompareNode>(&leaf.data)) {
            if (n->op != duql_ns::CompareOp::EQ) return std::nullopt;
            path = n->field.path;
            vals.push_back(literal_to_string(n->value));
        } else if (const auto* in = std::get_if<duql_ns::InNode>(&leaf.data)) {
            path = in->field.path;
            for (const auto& elem : in->values.elements)
                vals.push_back(literal_to_string(elem));
        } else {
            return std::nullopt;
        }
        const auto& blooms = d_.blooms(path);
        ChunkSet out;
        for (auto ckpt : candidates) {
            auto it = blooms.find(ckpt);
            if (it == blooms.end()) {
                if (!vals.empty()) out.insert(ckpt);
                continue;
            }
            for (const auto& v : vals)
                if (it->second.possibly_contains(v)) {
                    out.insert(ckpt);
                    break;
                }
        }
        return out;
    }

   private:
    FileIndexData& d_;
};

// A leaf the conditions test; an any() leaf left unexpanded (a very wide
// array) proves nothing, since its path's evidence describes the whole value.
bool is_leaf(const duql_ns::QueryNode& node) {
    const auto* field = leaf_field(node);
    return (std::holds_alternative<duql_ns::CompareNode>(node.data) ||
            std::holds_alternative<duql_ns::InNode>(node.data) ||
            std::holds_alternative<duql_ns::NotInNode>(node.data)) &&
           !(field && field->any) && !too_wide(node);
}

ChunkSet eval_leaf(const duql_ns::QueryNode& leaf, const Conditions& cs,
                   const ChunkSet& universe) {
    std::optional<ChunkSet> acc;
    for (const auto& c : cs) {
        auto s = c->may_match(leaf, acc ? *acc : universe);
        if (!s) continue;
        acc = acc ? intersect(*acc, *s) : std::move(*s);
        if (acc->empty() || c->exact()) break;
    }
    return acc ? std::move(*acc) : universe;
}

ChunkSet eval_all_match(const duql_ns::QueryNode& node, const Conditions& cs,
                        const ChunkSet& universe);

ChunkSet eval_node(const duql_ns::QueryNode& node, const Conditions& cs,
                   const ChunkSet& universe) {
    if (is_leaf(node)) return eval_leaf(node, cs, universe);
    if (const auto* n = std::get_if<duql_ns::AndNode>(&node.data))
        return intersect(eval_node(*n->left, cs, universe),
                         eval_node(*n->right, cs, universe));
    if (const auto* n = std::get_if<duql_ns::OrNode>(&node.data)) {
        auto left = eval_node(*n->left, cs, universe);
        auto right = eval_node(*n->right, cs, universe);
        left.insert(right.begin(), right.end());
        return left;
    }
    if (const auto* n = std::get_if<duql_ns::NotNode>(&node.data))
        // A chunk may hold an event matching `not p` unless every event in
        // it matches `p`; complementing the may-match set would drop chunks
        // holding events on both sides of `p`.
        return difference(universe, eval_all_match(*n->operand, cs, universe));
    return universe;  // a pattern match proves nothing
}

ChunkSet eval_all_match(const duql_ns::QueryNode& node, const Conditions& cs,
                        const ChunkSet& universe) {
    if (is_leaf(node)) {
        ChunkSet out;
        for (const auto& c : cs)
            if (auto s = c->all_match(node)) out.insert(s->begin(), s->end());
        return out;
    }
    if (const auto* n = std::get_if<duql_ns::AndNode>(&node.data))
        return intersect(eval_all_match(*n->left, cs, universe),
                         eval_all_match(*n->right, cs, universe));
    if (const auto* n = std::get_if<duql_ns::OrNode>(&node.data)) {
        // An under-approximation: a chunk where the two sides cover every
        // event between them is left out.
        auto left = eval_all_match(*n->left, cs, universe);
        auto right = eval_all_match(*n->right, cs, universe);
        left.insert(right.begin(), right.end());
        return left;
    }
    if (const auto* n = std::get_if<duql_ns::NotNode>(&node.data))
        return difference(universe, eval_node(*n->operand, cs, universe));
    return {};
}

}  // namespace

bool too_wide(const duql_ns::QueryNode& node) {
    if (const auto* n = std::get_if<duql_ns::InNode>(&node.data))
        return n->values.elements.size() > SEMI_JOIN_CAP;
    if (const auto* n = std::get_if<duql_ns::NotInNode>(&node.data))
        return n->values.elements.size() > SEMI_JOIN_CAP;
    return false;
}

namespace {

// A literal as a dftu_value; false for a uint64 past int64, which the ABI
// cannot carry. A string value borrows `lit`.
bool to_value(const duql_ns::LiteralNode& lit, ::dftu_value& out) {
    return std::visit(
        [&out](auto&& v) -> bool {
            using T = std::decay_t<decltype(v)>;
            out = ::dftu_value{};
            if constexpr (std::is_same_v<T, std::string>) {
                out.kind = DFTU_VAL_STR;
                out.count = static_cast<std::uint32_t>(v.size());
                out.as.str = v.c_str();
            } else if constexpr (std::is_same_v<T, bool>) {
                out.kind = DFTU_VAL_BOOL;
                out.as.b = v ? 1U : 0U;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out.kind = DFTU_VAL_I64;
                out.as.i64 = v;
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                if (v > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max()))
                    return false;
                out.kind = DFTU_VAL_I64;
                out.as.i64 = static_cast<std::int64_t>(v);
            } else {
                out.kind = DFTU_VAL_F64;
                out.as.f64 = v;
            }
            return true;
        },
        lit.value);
}

std::int32_t compare_op(duql_ns::CompareOp op) {
    switch (op) {
        case duql_ns::CompareOp::EQ:
            return DFTU_INDEX_EQ;
        case duql_ns::CompareOp::NE:
            return DFTU_INDEX_NE;
        case duql_ns::CompareOp::LT:
            return DFTU_INDEX_LT;
        case duql_ns::CompareOp::LE:
            return DFTU_INDEX_LE;
        case duql_ns::CompareOp::GT:
            return DFTU_INDEX_GT;
        case duql_ns::CompareOp::GE:
            return DFTU_INDEX_GE;
    }
    return DFTU_INDEX_EQ;
}

std::int32_t match_op(duql_ns::MatchOp op) {
    switch (op) {
        case duql_ns::MatchOp::LIKE:
            return DFTU_INDEX_LIKE;
        case duql_ns::MatchOp::ILIKE:
            return DFTU_INDEX_ILIKE;
        case duql_ns::MatchOp::REGEX:
            return DFTU_INDEX_REGEX;
        case duql_ns::MatchOp::IREGEX:
            return DFTU_INDEX_IREGEX;
        case duql_ns::MatchOp::ICONTAINS:
            return DFTU_INDEX_ICONTAINS;
    }
    return DFTU_INDEX_LIKE;
}

/// A plugin extension (plugins/abi/index.h): the plugin compiles each leaf
/// and judges each chunk's and the file's payload.
class PluginCondition final : public Condition {
   public:
    PluginCondition(FileIndexData& d, index::extensions::PluginExtensionPtr ext)
        : d_(d), ext_(std::move(ext)) {}
    ~PluginCondition() override {
        for (const auto& [leaf, h] : compiled_)
            if (h) ext_->vt.release(h);
    }
    PluginCondition(const PluginCondition&) = delete;
    PluginCondition& operator=(const PluginCondition&) = delete;

    IndexExtension extension() const override { return IndexExtension::PLUGIN; }
    std::string name() const override { return ext_->name; }

    bool file_may_match(const duql_ns::QueryNode& root) override {
        if (!ext_->vt.file_may_match) return true;
        if (!file_loaded_) {
            file_ = d_.db->path_file_value(d_.fid, IndexExtension::PLUGIN,
                                           ext_->name);
            file_loaded_ = true;
        }
        return !file_ || may_match_file(root);
    }

    std::optional<ChunkSet> may_match(const duql_ns::QueryNode& leaf,
                                      const ChunkSet& candidates) override {
        void* h = compile(leaf);
        if (!h) return std::nullopt;
        if (!granules_) {
            granules_.emplace();
            for (auto& [g, bytes] : d_.db->path_granules(
                     d_.fid, IndexExtension::PLUGIN, ext_->name))
                granules_->emplace(g, std::move(bytes));
        }
        ChunkSet out;
        for (auto c : candidates) {
            auto it = granules_->find(c);
            if (it == granules_->end() ||
                ext_->vt.may_match(
                    h, reinterpret_cast<const std::uint8_t*>(it->second.data()),
                    it->second.size()) != 0)
                out.insert(c);
        }
        return out;
    }

   private:
    bool may_match_file(const duql_ns::QueryNode& node) {
        if (const auto* a = std::get_if<duql_ns::AndNode>(&node.data))
            return may_match_file(*a->left) && may_match_file(*a->right);
        if (const auto* o = std::get_if<duql_ns::OrNode>(&node.data))
            return may_match_file(*o->left) || may_match_file(*o->right);
        if (std::holds_alternative<duql_ns::NotNode>(node.data)) return true;
        void* h = compile(node);
        return !h ||
               ext_->vt.file_may_match(
                   h, reinterpret_cast<const std::uint8_t*>(file_->data()),
                   file_->size()) != 0;
    }

    // NULL when the leaf cannot be passed or the plugin has no evidence.
    void* compile(const duql_ns::QueryNode& leaf) {
        auto [it, inserted] = compiled_.try_emplace(&leaf, nullptr);
        if (inserted) it->second = compile_leaf(leaf);
        return it->second;
    }

    void* compile_leaf(const duql_ns::QueryNode& leaf) {
        ::dftu_index_leaf l{};
        ::dftu_value value{};
        std::vector<::dftu_value> items;
        const duql_ns::FieldNode* field = nullptr;
        auto array = [&](const duql_ns::ArrayNode& values) {
            items.resize(values.elements.size());
            for (std::size_t i = 0; i < items.size(); ++i)
                if (!to_value(values.elements[i], items[i])) return false;
            value.kind = DFTU_VAL_ARRAY;
            value.count = static_cast<std::uint32_t>(items.size());
            value.as.items = items.data();
            return true;
        };
        if (const auto* cmp = std::get_if<duql_ns::CompareNode>(&leaf.data)) {
            if (!to_value(cmp->value, value)) return nullptr;
            field = &cmp->field;
            l.op = compare_op(cmp->op);
        } else if (const auto* in = std::get_if<duql_ns::InNode>(&leaf.data)) {
            if (!array(in->values)) return nullptr;
            field = &in->field;
            l.op = DFTU_INDEX_IN;
        } else if (const auto* nin =
                       std::get_if<duql_ns::NotInNode>(&leaf.data)) {
            if (!array(nin->values)) return nullptr;
            field = &nin->field;
            l.op = DFTU_INDEX_NOT_IN;
        } else if (const auto* mt =
                       std::get_if<duql_ns::MatchNode>(&leaf.data)) {
            if (mt->negated) return nullptr;
            value.kind = DFTU_VAL_STR;
            value.count = static_cast<std::uint32_t>(mt->pattern.size());
            value.as.str = mt->pattern.c_str();
            field = &mt->field;
            l.op = match_op(mt->op);
        } else {
            return nullptr;
        }
        l.path = field->path.c_str();
        l.path_len = static_cast<std::uint32_t>(field->path.size());
        l.any = field->any ? 1 : 0;
        l.value = &value;
        return ext_->vt.compile(ext_->self, &l);
    }

    FileIndexData& d_;
    index::extensions::PluginExtensionPtr ext_;
    ankerl::unordered_dense::map<const duql_ns::QueryNode*, void*> compiled_;
    std::optional<ankerl::unordered_dense::map<std::uint64_t, std::string>>
        granules_;
    std::optional<std::string> file_;
    bool file_loaded_ = false;
};

}  // namespace

Conditions make_query_conditions(FileIndexData& data) {
    Conditions cs;
    cs.push_back(std::make_unique<FileBloom>(data));
    cs.push_back(std::make_unique<CatalogPresence>(data));
    cs.push_back(std::make_unique<Postings>(data));
    cs.push_back(std::make_unique<Counts>(data));
    cs.push_back(std::make_unique<Zonemap>(data));
    cs.push_back(std::make_unique<ChunkBloom>(data));
    for (auto& ext : index::extensions::plugin_extensions()) {
        auto state = data.db->plugin_extension_state(data.fid, ext->name);
        if (state && ext->current(*state))
            cs.push_back(
                std::make_unique<PluginCondition>(data, std::move(ext)));
    }
    return cs;
}

bool file_may_match(const duql_ns::QueryNode& root,
                    const Conditions& conditions) {
    for (const auto& c : conditions)
        if (!c->file_may_match(root)) return false;
    return true;
}

ChunkSet evaluate(const duql_ns::QueryNode& root, const Conditions& conditions,
                  const ChunkSet& universe) {
    return eval_node(root, conditions, universe);
}

Conditions make_metadata_conditions(
    const ankerl::unordered_dense::map<std::uint64_t,
                                       index::store::ChunkMetadata>& metadata) {
    Conditions out;
    out.push_back(std::make_unique<MetadataRecords>(metadata));
    return out;
}

}  // namespace dftracer::utils::index::plan
